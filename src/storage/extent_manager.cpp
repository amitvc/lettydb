#include "storage_def.h"

#include "extent_manager.h"
#include "common/logger.h"
#include "common/db_exception.h"
#include "page_utils.h"
#include <limits>
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
  } else {
	auto opt_db_header = load_page_value<DatabaseHeader>(buffer_pool_, HEADER_PAGE_ID);
	if (!opt_db_header) {
	  throw DbException(DbErrorCode::IOError, "Failed to fetch database header page during extent manager initialization");
	}

	if (std::memcmp(opt_db_header.value().signature, DB_SIGNATURE, sizeof(DB_SIGNATURE)) != 0) {
	  throw DbException(DbErrorCode::Corruption, "Corrupt or invalid database file: Signature mismatch. Expected 'LETTYDB'.");
	}
	print_database_header(&opt_db_header.value());
  }
}

void ExtentManager::initialize_new_db() {
  LOG_STORAGE_INFO("Initializing new database file");

  // Since we are starting with empty db file we first initialize the DB header page and write it back to file.
  Page* header_frame = buffer_pool_.new_page(HEADER_PAGE_ID);
  if (!header_frame) {
	throw DbException(DbErrorCode::IOError, "Failed to create header page during initialization");
  }
  auto header = make_database_header();
  store_page_layout(header_frame, header);
   // sys_tables_iam_page and sys_columns_iam_page remain INVALID_PAGE_ID
  // They will be set during CatalogManager::bootstrap()
  buffer_pool_.unpin_page(HEADER_PAGE_ID, true); // BPM will write any pages we mark dirty to disk

  // Prepare the first GAM Page (Page 8 — first page of extent 1)
  Page* gam_frame = buffer_pool_.new_page(FIRST_GAM_PAGE_ID);
  if (!gam_frame) {
	throw DbException(DbErrorCode::IOError, "Failed to create GAM page during initialization");
  }
  auto gam_page = make_gam_page();

  // Mark reserved extents as allocated in the GAM bitmap
  Bitmap gam_bitmap(gam_page.bitmap, GAM_MAX_BITS);
  gam_bitmap.set(0);  // Extent 0 (pages 0-7)   - database header
  gam_bitmap.set(1);  // Extent 1 (pages 8-15)  - GAM pages
  gam_bitmap.set(2);  // Extent 2 (pages 16-23) - shared extent (IAM pages)
  store_page_layout(gam_frame, gam_page);
  buffer_pool_.unpin_page(FIRST_GAM_PAGE_ID, true);

  // Writing the last page of each reserved extent forces the OS to extend the file
  // to cover all pages in that extent, so subsequent reads won't return garbage.
  constexpr page_id_t EXTENT_0_LAST_PAGE = EXTENT_SIZE - 1;        // page 7
  constexpr page_id_t EXTENT_1_LAST_PAGE = 2 * EXTENT_SIZE - 1;    // page 15
  constexpr page_id_t EXTENT_2_LAST_PAGE = 3 * EXTENT_SIZE - 1;    // page 23

  Page* extent0_end_frame = buffer_pool_.new_page(EXTENT_0_LAST_PAGE);
  if (!extent0_end_frame) {
	throw DbException(DbErrorCode::IOError, "Failed to reserve extent 0 during initialization");
  }
  buffer_pool_.flush_page(EXTENT_0_LAST_PAGE);
  buffer_pool_.unpin_page(EXTENT_0_LAST_PAGE, false);

  // new_page marks the frame dirty, so flush_page writes the zeroed page and
  Page* extent1_end_frame = buffer_pool_.new_page(EXTENT_1_LAST_PAGE);
  if (!extent1_end_frame) {
	throw DbException(DbErrorCode::IOError, "Failed to reserve extent 1 (GAM extent) during initialization");
  }
  buffer_pool_.flush_page(EXTENT_1_LAST_PAGE);
  buffer_pool_.unpin_page(EXTENT_1_LAST_PAGE, false);

  // new_page marks the frame dirty, so flush_page writes the zeroed page and
  Page* extent2_end_frame = buffer_pool_.new_page(EXTENT_2_LAST_PAGE);
  if (!extent2_end_frame) {
	throw DbException(DbErrorCode::IOError, "Failed to reserve extent 2 (shared extent) during initialization");
  }
  buffer_pool_.flush_page(EXTENT_2_LAST_PAGE);
  buffer_pool_.unpin_page(EXTENT_2_LAST_PAGE, false);
}

page_id_t ExtentManager::allocate_extent() {
  LOG_STORAGE_DEBUG("Starting extent allocation");
  std::lock_guard<std::mutex> guard(lock_);

  while (true) {
      Page* gam_frame = buffer_pool_.fetch_page(current_gam_page_id_);
      if (!gam_frame) return INVALID_PAGE_ID;

      auto gam_page = load_page_layout<GAMPage>(gam_frame);

      page_id_t allocated_page_id = allocate_extent_in_gam_page(&gam_page);

      if (allocated_page_id != INVALID_PAGE_ID) {
          store_page_layout(gam_frame, gam_page);
          buffer_pool_.unpin_page(current_gam_page_id_, true);
          return allocated_page_id;
      }

      page_id_t next_gam_id = gam_page.next_page_id;
      buffer_pool_.unpin_page(current_gam_page_id_, false);

	  //
      if (next_gam_id == INVALID_PAGE_ID) break;
      advance_gam_cursor(next_gam_id);
  }

  // No free space found in existing GAM pages. Create a new one.
  if (!create_and_link_new_gam()) {
    return INVALID_PAGE_ID;
  }

  // Try one last time from the newly created GAM page (guaranteed to have space)
  Page* gam_frame = buffer_pool_.fetch_page(current_gam_page_id_);
  if (!gam_frame) return INVALID_PAGE_ID;

  auto gam_page = load_page_layout<GAMPage>(gam_frame);
  page_id_t result = allocate_extent_in_gam_page(&gam_page);
  if (result != INVALID_PAGE_ID) {
    store_page_layout(gam_frame, gam_page);
  }
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

  // Next GAM page goes to the next sequential page.
  // If it's still within the same extent as the current GAM page, use it directly.
  // Otherwise, allocate a new extent at end of file.
  page_id_t candidate = current_gam_page_id + 1;
  bool fits_in_current_extent = (candidate / EXTENT_SIZE == current_gam_page_id / EXTENT_SIZE);
  page_id_t new_gam_page_id = fits_in_current_extent ? candidate : buffer_pool_.get_file_size_in_pages();

  // Initialize the new GAM page in the buffer pool
  Page* new_gam_frame = buffer_pool_.new_page(new_gam_page_id);
  if (!new_gam_frame) {
    new_gam_frame = buffer_pool_.fetch_page(new_gam_page_id);
    if (new_gam_frame && (new_gam_frame->get_pin_count() != 1 || new_gam_frame->is_dirty())) {
      buffer_pool_.unpin_page(new_gam_page_id, false);
      new_gam_frame = nullptr;
    }
  }
  if (!new_gam_frame) {
    LOG_STORAGE_ERROR("Failed to create new GAM page {}", new_gam_page_id);
    return false;
  }
  auto new_gam_page = make_gam_page();
  new_gam_page.next_page_id = INVALID_PAGE_ID;

  // GAM pages outside the dedicated GAM extent live at the start of a new extent
  // and must self-protect by marking bit 0 to prevent that extent from being re-allocated.
  if (!fits_in_current_extent) {
    Bitmap new_gam_bitmap(new_gam_page.bitmap, GAM_MAX_BITS);
    new_gam_bitmap.set(0);
  }
  store_page_layout(new_gam_frame, new_gam_page);
  buffer_pool_.unpin_page(new_gam_page_id, true);

  // Link the old GAM page to the new one
  Page* old_gam_frame = buffer_pool_.fetch_page(current_gam_page_id);
  if (!old_gam_frame) {
      LOG_STORAGE_ERROR("Failed to load current GAM page {} to link new GAM", current_gam_page_id);
      return false;
  }
  auto old_gam_page = load_page_layout<GAMPage>(old_gam_frame);
  old_gam_page.next_page_id = new_gam_page_id;
  store_page_layout(old_gam_frame, old_gam_page);
  buffer_pool_.unpin_page(current_gam_page_id, true);

  // Advance cursor to the new page
  advance_gam_cursor(new_gam_page_id);
  return true;
}

bool ExtentManager::is_valid_extent_for_deallocation(page_id_t start_page_id) const {
    if (start_page_id < 0) {
        LOG_STORAGE_DEBUG("Ignoring deallocation of invalid page ID {}", start_page_id);
        return false;
    }
    if (start_page_id % EXTENT_SIZE != 0) {
        LOG_STORAGE_DEBUG("Ignoring deallocation of non-extent-aligned page ID {}", start_page_id);
        return false;
    }
    if (start_page_id == 0) {
        LOG_STORAGE_ERROR("Attempted to deallocate system extent 0 - this would corrupt the database");
        return false;
    }
    return true;
}

page_id_t ExtentManager::find_gam_page_at_index(size_t gam_page_index) {
    page_id_t current = FIRST_GAM_PAGE_ID;
    for (size_t steps = 0; steps < gam_page_index; ++steps) {
        auto gam_page = load_page_value<GAMPage>(buffer_pool_, current);
        if (!gam_page) {
            LOG_STORAGE_ERROR("Failed to fetch GAM page {} during deallocation", current);
            return INVALID_PAGE_ID;
        }
        page_id_t next_id = gam_page->next_page_id;
        if (next_id == INVALID_PAGE_ID) {
            LOG_STORAGE_ERROR("GAM chain broken at page {} during deallocation", current);
            return INVALID_PAGE_ID;
        }
        current = next_id;
    }
    return current;
}

bool ExtentManager::clear_extent_bit(page_id_t gam_page_id, uint16_t bit_in_gam, size_t gam_page_index) {
    Page* gam_frame = buffer_pool_.fetch_page(gam_page_id);
    if (!gam_frame) {
        LOG_STORAGE_ERROR("Failed to fetch GAM page {} for bit clearing", gam_page_id);
        return false;
    }

    auto gam_page = load_page_layout<GAMPage>(gam_frame);
    Bitmap bitmap(gam_page.bitmap, GAM_MAX_BITS);
    bitmap.clear(bit_in_gam);

    if (bit_in_gam < gam_page.first_free_bit_hint) {
        gam_page.first_free_bit_hint = bit_in_gam;
        LOG_STORAGE_DEBUG("Rewound first_free_hint to {} in GAM page {}", bit_in_gam, gam_page_id);
    }
    store_page_layout(gam_frame, gam_page);
    buffer_pool_.unpin_page(gam_page_id, true);

    // Rewind cursor so future allocations can reuse this freed extent
    if (gam_page_index < current_gam_chain_index_) {
        current_gam_chain_index_ = gam_page_index;
        current_gam_page_id_ = gam_page_id;
    }
    return true;
}

bool ExtentManager::deallocate_extent(page_id_t start_page_id) {
    LOG_STORAGE_INFO("Deallocating extent at page {}", start_page_id);

    if (!is_valid_extent_for_deallocation(start_page_id)) return false;

    std::lock_guard<std::mutex> guard(lock_);

    size_t extent_index = start_page_id / EXTENT_SIZE;
    size_t gam_page_index = extent_index / GAM_MAX_BITS;
    uint16_t bit_in_gam = static_cast<uint16_t>(extent_index % GAM_MAX_BITS);

    page_id_t target_gam_page_id = find_gam_page_at_index(gam_page_index);
    if (target_gam_page_id == INVALID_PAGE_ID) return false;

    return clear_extent_bit(target_gam_page_id, bit_in_gam, gam_page_index);
}

page_id_t ExtentManager::claim_extent_at_bit(GAMPage* gam_page,
                                              size_t bit_index) {
  Bitmap bitmap(gam_page->bitmap, GAM_MAX_BITS);
  bitmap.set(bit_index);

  // Advance the hint so the next allocation starts after this bit.
  gam_page->first_free_bit_hint = static_cast<uint16_t>(bit_index + 1);

  LOG_STORAGE_DEBUG("Found free bit {} in GAM page {}", bit_index, current_gam_page_id_);

  // Page ID formula:
  // (chain_index * GAM_MAX_BITS + bit_index) * EXTENT_SIZE
  // chain_index tells us how many full GAM pages precede this one;
  // bit_index is the extent's position within this GAM page.
  int64_t base_page_id = static_cast<int64_t>(current_gam_chain_index_) * GAM_MAX_BITS * EXTENT_SIZE;
  LOG_STORAGE_DEBUG("Base Page ID: {}, GAM Page Chain ID {}", base_page_id, current_gam_chain_index_);
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
          return claim_extent_at_bit(gam_page, i);
      }
  }

  // Wrap around - search from 0 to hint (handles fragmentation after deallocation)
  for (size_t i = 0; i < hint; ++i) {
      if (!bitmap.is_set(i)) {
          return claim_extent_at_bit(gam_page, i);
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
