#include "storage_def.h"

#include "extent_manager.h"
#include "common/logger.h"
#include <limits>
#include <new>
#include <stdexcept>
#include <string>

namespace letty {

ExtentManager::ExtentManager(BufferPoolManager& buffer_pool) : buffer_pool_(buffer_pool) {
  LOG_STORAGE_INFO("Initializing ExtentManager");

  // Detect empty database by checking file size.
  // We cannot rely on fetch_page returning nullptr for an empty file because
  // BufferPoolManager reads into a zeroed page regardless of disk read result.
  if (buffer_pool_.get_file_size_in_pages() == 0) {
      LOG_STORAGE_INFO("****Database file empty****");
      initialize_new_db();
      return;
  }

  // Existing database — validate header
  Page* header_page = buffer_pool_.fetch_page(HEADER_PAGE_ID);
  if (!header_page) {
      throw std::runtime_error("Failed to fetch header page from buffer pool");
  }
  auto* header = reinterpret_cast<DatabaseHeader*>(header_page->get_data());
  if (std::string(header->signature) != DB_SIGNATURE) {
      buffer_pool_.unpin_page(HEADER_PAGE_ID, false);
      throw std::runtime_error("Corrupt or invalid database file: Signature mismatch. Expected 'LETTYDB'.");
  }
  print_database_header(header);
  buffer_pool_.unpin_page(HEADER_PAGE_ID, false);
}

/**
 * @brief Bootstraps a fresh database file with initial system pages.
 * Creates:
 * - Page 0: Database Header
 * - Page 1: GAM (Global Allocation Map)
 * - Extent 0 and 1 reserved via flush to extend the file
 *
 * Note: IAM pages for sys_tables and sys_columns are allocated dynamically
 * from the metadata pool during CatalogManager::bootstrap()
 */
void ExtentManager::initialize_new_db() {
  LOG_STORAGE_INFO("Initializing new database file (version 2 - metadata pool)");

  // Prepare Header Page (Page 0)
  Page* header_page = buffer_pool_.new_page(HEADER_PAGE_ID);
  if (!header_page) {
    throw std::runtime_error("Failed to create header page during initialization");
  }
  auto* header = new(header_page->get_data()) DatabaseHeader();
  header->gam_page_id = FIRST_GAM_PAGE_ID;
  header->metadata_pool_page_id = FIRST_METADATA_POOL_PAGE_ID;
  // sys_tables_iam_page and sys_columns_iam_page remain INVALID_PAGE_ID
  // They will be set during CatalogManager::bootstrap()
  buffer_pool_.unpin_page(HEADER_PAGE_ID, true);

  // Prepare GAM Page (Page 1)
  Page* gam_page_obj = buffer_pool_.new_page(FIRST_GAM_PAGE_ID);
  if (!gam_page_obj) {
    throw std::runtime_error("Failed to create GAM page during initialization");
  }
  auto* gam_page = new(gam_page_obj->get_data()) GAMPage();

  // Mark Extent 0 as allocated (Header, GAM, pages 2-7 reserved)
  Bitmap gam_bitmap(gam_page->bitmap, GAM_MAX_BITS);
  gam_bitmap.set(0);  // Extent 0 (pages 0-7)
  gam_bitmap.set(1);  // Extent 1 (pages 8-15) - metadata pool
  buffer_pool_.unpin_page(FIRST_GAM_PAGE_ID, true);

  // Reserve Extent 0 by writing its last page (extends file on disk)
  Page* extent0_end = buffer_pool_.new_page(EXTENT_SIZE - 1);
  if (!extent0_end) {
    throw std::runtime_error("Failed to reserve extent 0 during initialization");
  }
  buffer_pool_.unpin_page(EXTENT_SIZE - 1, true);
  buffer_pool_.flush_page(EXTENT_SIZE - 1);

  // Reserve Extent 1 (metadata pool) by writing its last page
  Page* extent1_end = buffer_pool_.new_page(2 * EXTENT_SIZE - 1);
  if (!extent1_end) {
    throw std::runtime_error("Failed to reserve extent 1 (metadata pool) during initialization");
  }
  buffer_pool_.unpin_page(2 * EXTENT_SIZE - 1, true);
  buffer_pool_.flush_page(2 * EXTENT_SIZE - 1);
}

page_id_t ExtentManager::allocate_extent() {
  LOG_STORAGE_DEBUG("Starting extent allocation");
  std::lock_guard<std::mutex> guard(lock_);

  page_id_t next_gam_id = INVALID_PAGE_ID;
  do {
      Page* gam_page_obj = buffer_pool_.fetch_page(current_gam_page_id_);
      if (!gam_page_obj) return INVALID_PAGE_ID;

      auto* gam_page = reinterpret_cast<GAMPage*>(gam_page_obj->get_data());
      page_id_t allocated_page_id = allocate_extent_in_gam_page(gam_page);

      if (allocated_page_id != INVALID_PAGE_ID) {
          buffer_pool_.unpin_page(current_gam_page_id_, true);
          return allocated_page_id;
      }

      // Current page is full. Check if we can advance.
      next_gam_id = gam_page->next_page_id;
      buffer_pool_.unpin_page(current_gam_page_id_, false);

      if (next_gam_id != INVALID_PAGE_ID) {
          advance_gam_cursor(next_gam_id);
      }
  } while (next_gam_id != INVALID_PAGE_ID);

  // No free space found in existing GAM pages. Create a new one.
  if (!create_and_link_new_gam()) {
    return INVALID_PAGE_ID;
  }

  // Try one last time from the newly created GAM page (guaranteed to have space)
  Page* gam_page_obj = buffer_pool_.fetch_page(current_gam_page_id_);
  if (!gam_page_obj) return INVALID_PAGE_ID;

  auto* gam_page = reinterpret_cast<GAMPage*>(gam_page_obj->get_data());
  page_id_t result = allocate_extent_in_gam_page(gam_page);
  buffer_pool_.unpin_page(current_gam_page_id_, result != INVALID_PAGE_ID);
  return result;
}

void ExtentManager::advance_gam_cursor(page_id_t next_gam_id) {
  LOG_INFO("Moving to next GAM Page in the chain. Current page ID {} chainID {} Next page ID {} chainID {}",
       current_gam_page_id_, current_gam_chain_index_, next_gam_id, current_gam_chain_index_ + 1);
  current_gam_page_id_ = next_gam_id;
  current_gam_chain_index_++;
}

bool ExtentManager::create_and_link_new_gam() {
  page_id_t current_gam_page_id = current_gam_page_id_;

  // Logic to determine location of new GAM page
  // New GAM pages pack into pages 2-7 of extent 0, then extend to new extents
  page_id_t candidate = current_gam_page_id + 1;

  page_id_t new_gam_page_id = INVALID_PAGE_ID;
  bool is_packed = false;

  if (candidate < EXTENT_SIZE) {
    new_gam_page_id = candidate;
    is_packed = true;
  } else {
    new_gam_page_id = buffer_pool_.get_file_size_in_pages();
    is_packed = false;
  }

  // Initialize the new GAM page in the buffer pool
  Page* new_gam_page_obj = buffer_pool_.new_page(new_gam_page_id);
  if (!new_gam_page_obj) {
    LOG_STORAGE_ERROR("Failed to create new GAM page {}", new_gam_page_id);
    return false;
  }
  auto* new_gam_page = new(new_gam_page_obj->get_data()) GAMPage();
  new_gam_page->next_page_id = INVALID_PAGE_ID;

  if (!is_packed) {
    // If we allocated a new extent for this GAM, mark it as used to protect itself.
    Bitmap new_gam_bitmap(new_gam_page->bitmap, GAM_MAX_BITS);
    new_gam_bitmap.set(0);
  }
  buffer_pool_.unpin_page(new_gam_page_id, true);

  // Link the old GAM page to the new one
  Page* old_gam_page_obj = buffer_pool_.fetch_page(current_gam_page_id);
  if (!old_gam_page_obj) {
      LOG_STORAGE_ERROR("Failed to load current GAM page {} to link new GAM", current_gam_page_id);
      return false;
  }
  reinterpret_cast<GAMPage*>(old_gam_page_obj->get_data())->next_page_id = new_gam_page_id;
  buffer_pool_.unpin_page(current_gam_page_id, true);

  // Advance cursor to the new page
  advance_gam_cursor(new_gam_page_id);
  return true;
}

bool ExtentManager::deallocate_extent(page_id_t start_page_id) {
    LOG_STORAGE_INFO("Deallocating extent at page {}", start_page_id);

    // Early validation: reject invalid page IDs
    if (start_page_id < 0 || start_page_id == INVALID_PAGE_ID) {
        LOG_STORAGE_DEBUG("Ignoring deallocation of invalid page ID {}", start_page_id);
        return false;
    }

    // Validate extent alignment: page ID must be at the start of an extent
    if (start_page_id % EXTENT_SIZE != 0) {
        LOG_STORAGE_DEBUG("Ignoring deallocation of non-extent-aligned page ID {}", start_page_id);
        return false;
    }

    // Protect system extent (extent 0) from deallocation - it contains Header, GAM, and IAM pages
    if (start_page_id == 0) {
        LOG_STORAGE_ERROR("Attempted to deallocate system extent 0 - this would corrupt the database");
        return false;
    }

    std::lock_guard<std::mutex> guard(lock_);

    size_t extent_index = start_page_id / EXTENT_SIZE;
    size_t bits_per_gam = GAM_MAX_BITS;
    size_t gam_page_index = extent_index / bits_per_gam;
    uint16_t bit_in_gam = static_cast<uint16_t>(extent_index % bits_per_gam);

    // Walk the GAM chain to find the correct GAM page
    page_id_t target_gam_page_id = FIRST_GAM_PAGE_ID;
    size_t steps = gam_page_index;
    while (steps-- > 0) {
        Page* gam_page_obj = buffer_pool_.fetch_page(target_gam_page_id);
        if (!gam_page_obj) {
            LOG_STORAGE_ERROR("Failed to fetch GAM page {} during deallocation", target_gam_page_id);
            return false;
        }
        page_id_t next_id = reinterpret_cast<const GAMPage*>(gam_page_obj->get_data())->next_page_id;
        buffer_pool_.unpin_page(target_gam_page_id, false);

        if (next_id == INVALID_PAGE_ID) {
            LOG_STORAGE_ERROR("GAM chain broken at page {} during deallocation", target_gam_page_id);
            return false;
        }
        target_gam_page_id = next_id;
    }

    // Fetch the target GAM page and clear the bit
    Page* target_gam_page_obj = buffer_pool_.fetch_page(target_gam_page_id);
    if (!target_gam_page_obj) {
        LOG_STORAGE_ERROR("Failed to fetch GAM page {} for bit clearing", target_gam_page_id);
        return false;
    }

    auto* gam_page = reinterpret_cast<GAMPage*>(target_gam_page_obj->get_data());
    Bitmap bitmap(gam_page->bitmap, bits_per_gam);
    bitmap.clear(bit_in_gam);

    // Update first_free_hint if we're freeing a bit before the current hint
    if (bit_in_gam < gam_page->first_free_bit_hint) {
        gam_page->first_free_bit_hint = bit_in_gam;
        LOG_STORAGE_DEBUG("Rewound first_free_hint to {} in GAM page {}", bit_in_gam, target_gam_page_id);
    }

    buffer_pool_.unpin_page(target_gam_page_id, true);

    // Optimization: if we deallocated in a GAM page before our cached "last known free",
    // we should rewind our pointer to ensure we can reuse this newly freed space.
    if (gam_page_index < current_gam_chain_index_) {
        current_gam_chain_index_ = gam_page_index;
        current_gam_page_id_ = target_gam_page_id;
    }

    return true;
}

page_id_t ExtentManager::commit_extent_allocation(GAMPage* gam_page,
                                                   size_t bit_index) {
  Bitmap bitmap(gam_page->bitmap, GAM_MAX_BITS);
  bitmap.set(bit_index);

  // Update hint to next position for future allocations
  gam_page->first_free_bit_hint = static_cast<uint16_t>(bit_index + 1);

  // No disk write here — the caller will unpin the page as dirty

  LOG_STORAGE_DEBUG("Found free bit {} in GAM page {}", bit_index, current_gam_page_id_);

  /*
   * The calculation of page id below is important to understand.
   * We use GAM page index and the bit set within this page to calculate the page id we return to the caller.
   *
   * 1. gam_page_index: Which GAM page are we looking at?
   * 2. GAM_MAX_BITS: How many extents does one GAM page cover?
   *    - Each bit represents one EXTENT.
   *    - If PAGE_SIZE=4096, a GAM has ~32,704 bits.
   *    - So one GAM page covers ~32,704 extents.
   *    - 8 bits are reserved for page type and next page id. See the GAMPage struct in storage_def.h
   *
   * 3. EXTENT_SIZE: How many pages are in one extent? (Defined as 8)
   *
   * FORMULA:
   * Absolute Page ID = (Pages covered by previous GAMs) + (Pages covered by bits before 'bit_index' in current GAM)
   *
   * EXAMPLE:
   * Pattern: [GAM 0] [GAM 1] ...
   * Let's say GAM_MAX_BITS = 4 (simplification) and EXTENT_SIZE = 8.
   *
   * Case A: gam_page_index = 0, bit_index = 2 (3rd bit)
   *    base_page_id = 0 * 4 * 8 = 0
   *    offset       = 2 * 8 = 16
   *    Result       = Page 16. (Extent 2 covers pages 16-23)
   *
   * Case B: gam_page_index = 1, bit_index = 2 (3rd bit of 2nd GAM)
   *    base_page_id = 1 * 4 * 8 = 32
   *    offset       = 2 * 8 = 16
   *    Result       = Page 48.
   */
  int64_t base_page_id = static_cast<int64_t>(current_gam_chain_index_) * GAM_MAX_BITS * EXTENT_SIZE;
  LOG_INFO("Base Page ID: {}, GAM Page Chain ID {}", base_page_id, current_gam_chain_index_);
  int64_t result = base_page_id + static_cast<int64_t>(bit_index * EXTENT_SIZE);

  // Check for overflow before casting to page_id_t
  if (result > std::numeric_limits<page_id_t>::max()) {
      LOG_STORAGE_ERROR("Page ID overflow: calculated page {} exceeds maximum", result);
      return INVALID_PAGE_ID;
  }

  LOG_STORAGE_INFO("Successfully allocated extent at page {}", result);
  return static_cast<page_id_t>(result);
}

page_id_t ExtentManager::allocate_extent_in_gam_page(GAMPage* gam_page) {
  Bitmap bitmap(gam_page->bitmap, GAM_MAX_BITS);

  // Use the hint to start searching - avoids O(n) scan in common case
  size_t hint = gam_page->first_free_bit_hint;
  if (hint >= GAM_MAX_BITS) {
	// Safety: reset invalid hint
	LOG_STORAGE_WARN("GamPage free page hint greater than {}", GAM_MAX_BITS);
	hint = 0;
  }

  // Search from hint to end of bitmap
  for (size_t i = hint; i < GAM_MAX_BITS; ++i) {
      if (!bitmap.is_set(i)) {
          return commit_extent_allocation(gam_page, i);
      }
  }

  // Wrap around - search from 0 to hint (handles fragmentation after deallocation)
  for (size_t i = 0; i < hint; ++i) {
      if (!bitmap.is_set(i)) {
          return commit_extent_allocation(gam_page, i);
      }
  }

  // This means Page is completely full
  return INVALID_PAGE_ID;
}

void ExtentManager::print_database_header(const DatabaseHeader *header) {
  LOG_STORAGE_INFO("******* Database Header *********");
  LOG_STORAGE_INFO("Signature: {}, Version: {}, Page size: {}", header->signature, header->version, header->page_size);
  LOG_STORAGE_INFO("GAM PageID : {}, System tables PageID: {}", header->gam_page_id, header->sys_tables_iam_page);
}

}
