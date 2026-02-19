#include "iam_manager.h"
#include "slotted_page.h"
#include "common/logger.h"
#include <cstring>
#include <new>

namespace letty {

IamManager::IamManager(BufferPoolManager &buffer_pool, ExtentManager &extent_manager)
    : buffer_pool_(buffer_pool), extent_manager_(extent_manager) {}

page_id_t IamManager::get_metadata_pool_page_id() {
  if (metadata_pool_page_id_ != INVALID_PAGE_ID) {
    return metadata_pool_page_id_;
  }

  // Load from header page 0
  Page* page = buffer_pool_.fetch_page(HEADER_PAGE_ID);
  if (!page) {
    LOG_STORAGE_ERROR("Failed to fetch header page for metadata pool lookup");
    return INVALID_PAGE_ID;
  }

  metadata_pool_page_id_ = reinterpret_cast<const DatabaseHeader*>(page->get_data())->metadata_pool_page_id;
  buffer_pool_.unpin_page(HEADER_PAGE_ID, false);
  LOG_STORAGE_DEBUG("Cached metadata_pool_page_id = {}", metadata_pool_page_id_);
  return metadata_pool_page_id_;
}

void IamManager::init_metadata_pool(page_id_t pool_page_id) {
  LOG_STORAGE_DEBUG("Initializing metadata pool at page {}", pool_page_id);

  Page* page = buffer_pool_.fetch_page(pool_page_id);
  if (page == nullptr) {
    LOG_STORAGE_ERROR("Failed to fetch metadata pool page {}", pool_page_id);
    return;
  }

  auto* header = new(page->get_data()) MetadataPoolDirectoryPage();
  header->next_pool_page = INVALID_PAGE_ID;
  header->slots_bitmap = 0;

  buffer_pool_.unpin_page(pool_page_id, true);

  // Cache it immediately since we know the value
  metadata_pool_page_id_ = pool_page_id;
}

page_id_t IamManager::allocate_metadata_page() {
  LOG_STORAGE_DEBUG("Allocating metadata page from pool");

  page_id_t pool_page_id = get_metadata_pool_page_id();
  if (pool_page_id == INVALID_PAGE_ID) {
    LOG_STORAGE_ERROR("No metadata pool available");
    return INVALID_PAGE_ID;
  }

  page_id_t current_pool_page = pool_page_id;

  // Traverse metadata pool chain looking for a free slot
  while (current_pool_page != INVALID_PAGE_ID) {
    Page* pool_page_obj = buffer_pool_.fetch_page(current_pool_page);
    if (!pool_page_obj) {
      LOG_STORAGE_ERROR("Failed to fetch metadata pool page {}", current_pool_page);
      return INVALID_PAGE_ID;
    }

    auto* pool_header = reinterpret_cast<MetadataPoolDirectoryPage*>(pool_page_obj->get_data());
    uint8_t free_slot = pool_header->find_free_slot();

    if (free_slot != MetadataPoolDirectoryPage::NO_FREE_SLOT) {
      pool_header->mark_slot_used(free_slot);
      buffer_pool_.unpin_page(current_pool_page, true);

      page_id_t allocated_page = current_pool_page + free_slot;
      LOG_STORAGE_INFO("Allocated metadata page {} from pool (slot {})", allocated_page, free_slot);
      return allocated_page;
    }

    // No free slot — advance or extend
    page_id_t next_pool = pool_header->next_pool_page;
    if (next_pool == INVALID_PAGE_ID) {
      next_pool = extend_metadata_pool(pool_header, current_pool_page);
      if (next_pool == INVALID_PAGE_ID) {
        buffer_pool_.unpin_page(current_pool_page, false);
        return INVALID_PAGE_ID;
      }
      buffer_pool_.unpin_page(current_pool_page, true);
    } else {
      buffer_pool_.unpin_page(current_pool_page, false);
    }
    current_pool_page = next_pool;
  }

  LOG_STORAGE_ERROR("Failed to allocate metadata page - no pool available");
  return INVALID_PAGE_ID;
}

page_id_t IamManager::extend_metadata_pool(MetadataPoolDirectoryPage* current_header,
										   page_id_t current_pool_page) {
  page_id_t new_pool_extent = extent_manager_.allocate_extent();
  if (new_pool_extent == INVALID_PAGE_ID) {
    LOG_STORAGE_ERROR("Failed to allocate new metadata pool extent");
    return INVALID_PAGE_ID;
  }

  // Initialize the new pool header page (placement-new requires raw API)
  Page* new_page = buffer_pool_.fetch_page(new_pool_extent);
  if (new_page == nullptr) {
    LOG_STORAGE_ERROR("Failed to fetch new metadata pool page {}", new_pool_extent);
    return INVALID_PAGE_ID;
  }
  auto* new_pool_header = new(new_page->get_data()) MetadataPoolDirectoryPage();
  new_pool_header->next_pool_page = INVALID_PAGE_ID;
  new_pool_header->slots_bitmap = 0;
  buffer_pool_.unpin_page(new_pool_extent, true);

  // Link current pool to the new one
  current_header->next_pool_page = new_pool_extent;
  LOG_STORAGE_INFO("Created new metadata pool extent at page {}", new_pool_extent);
  return new_pool_extent;
}

page_id_t IamManager::create_iam_chain() {
  LOG_STORAGE_DEBUG("Creating new IAM chain");

  // Allocate a page from the metadata pool
  page_id_t iam_page_id = allocate_metadata_page();
  if (iam_page_id == INVALID_PAGE_ID) {
    LOG_STORAGE_ERROR("Failed to allocate IAM page from metadata pool");
    return INVALID_PAGE_ID;
  }

  // Initialize as empty IAMPage — page already exists in the file,
  // so use fetch_page (not new_page)
  Page* page = buffer_pool_.fetch_page(iam_page_id);
  if (page == nullptr) {
    LOG_STORAGE_ERROR("Failed to fetch newly allocated IAM page {}", iam_page_id);
    return INVALID_PAGE_ID;
  }

  auto* iam_page = new(page->get_data()) IAMPage();
  iam_page->next_page_id = INVALID_PAGE_ID;
  iam_page->extent_count = 0;

  buffer_pool_.unpin_page(iam_page_id, true);

  LOG_STORAGE_INFO("Created new IAM chain at page {}", iam_page_id);
  return iam_page_id;
}

page_id_t IamManager::allocate_extent_for_table(page_id_t iam_head_page_id) {
  LOG_STORAGE_DEBUG("Allocating extent for IAM chain {}", iam_head_page_id);

  if (iam_head_page_id == INVALID_PAGE_ID) {
    LOG_STORAGE_ERROR("Invalid IAM head page ID");
    return INVALID_PAGE_ID;
  }

  // 1. Allocate a physical extent from ExtentManager
  page_id_t extent_start_page = extent_manager_.allocate_extent();
  if (extent_start_page == INVALID_PAGE_ID) {
    LOG_STORAGE_ERROR("Failed to allocate physical extent");
    return INVALID_PAGE_ID;
  }

  uint32_t extent_id = extent_id_from_page(extent_start_page);
  LOG_STORAGE_DEBUG("Got extent {} (page {})", extent_id, extent_start_page);

  // 2. Find the IAM page with space for this extent ID
  page_id_t current_iam_page = iam_head_page_id;
  page_id_t prev_iam_page = INVALID_PAGE_ID;

  while (current_iam_page != INVALID_PAGE_ID) {
    Page* iam_page_obj = buffer_pool_.fetch_page(current_iam_page);
    if (!iam_page_obj) {
      LOG_STORAGE_ERROR("Failed to fetch IAM page {}", current_iam_page);
      return INVALID_PAGE_ID;
    }

    auto* iam_page = reinterpret_cast<IAMPage*>(iam_page_obj->get_data());

    if (iam_page->has_space()) {
      // Add the extent to this page in-place
      iam_page->add_extent(extent_id);
      buffer_pool_.unpin_page(current_iam_page, true);

      LOG_STORAGE_INFO("Added extent {} to IAM page {} (now has {} extents)",
                      extent_id, current_iam_page, iam_page->extent_count);

      // Update hint: the new extent's first page is guaranteed to have space
      last_page_hint_[iam_head_page_id] = extent_start_page;
      return extent_start_page;
    }

    prev_iam_page = current_iam_page;
    page_id_t next_iam = iam_page->next_page_id;
    buffer_pool_.unpin_page(current_iam_page, false);
    current_iam_page = next_iam;
  }

  // 3. No IAM page has space — allocate a new one
  page_id_t new_iam_page_id = allocate_metadata_page();
  if (new_iam_page_id == INVALID_PAGE_ID) {
    LOG_STORAGE_ERROR("Failed to allocate new IAM page");
    return INVALID_PAGE_ID;
  }

  // Initialize new IAM page (placement-new requires raw API)
  Page* new_page = buffer_pool_.fetch_page(new_iam_page_id);
  if (new_page == nullptr) {
    LOG_STORAGE_ERROR("Failed to fetch new IAM page {}", new_iam_page_id);
    return INVALID_PAGE_ID;
  }
  auto* new_iam = new(new_page->get_data()) IAMPage();
  new_iam->next_page_id = INVALID_PAGE_ID;
  new_iam->add_extent(extent_id);
  buffer_pool_.unpin_page(new_iam_page_id, true);

  // Link it to the chain
  if (prev_iam_page != INVALID_PAGE_ID) {
    Page* prev_page_obj = buffer_pool_.fetch_page(prev_iam_page);
    if (!prev_page_obj) {
      LOG_STORAGE_ERROR("Failed to fetch previous IAM page {} for linking", prev_iam_page);
      return extent_start_page;  // Extent allocated but IAM chain broken — log and return
    }
    reinterpret_cast<IAMPage*>(prev_page_obj->get_data())->next_page_id = new_iam_page_id;
    buffer_pool_.unpin_page(prev_iam_page, true);
  }

  LOG_STORAGE_INFO("Created new IAM page {} and added extent {}", new_iam_page_id, extent_id);

  // Update hint: the new extent's first page is guaranteed to have space
  last_page_hint_[iam_head_page_id] = extent_start_page;
  return extent_start_page;
}

page_id_t IamManager::find_page_with_space(page_id_t iam_head_page_id, uint32_t required_space) {
  LOG_STORAGE_DEBUG("Finding page with {} bytes of space in IAM chain {}",
                   required_space, iam_head_page_id);

  if (iam_head_page_id == INVALID_PAGE_ID) {
    LOG_STORAGE_ERROR("Invalid IAM head page ID");
    return INVALID_PAGE_ID;
  }

  uint32_t total_needed = required_space + sizeof(Slot);

  // Fast path: check cached hint page, then scan forward within its extent
  auto hint_it = last_page_hint_.find(iam_head_page_id);
  if (hint_it != last_page_hint_.end()) {
    page_id_t result = find_hint_page(iam_head_page_id, total_needed);
    if (result != INVALID_PAGE_ID) return result;

    // Hint page full — scan forward in same extent
    result = scan_extent_forward(iam_head_page_id, hint_it->second, total_needed);
    if (result != INVALID_PAGE_ID) return result;

    // No space in hint extent — signal caller to allocate a new extent
    LOG_STORAGE_DEBUG("No space in hint extent, signaling new extent needed");
    return INVALID_PAGE_ID;
  }

  // No hint available (first insert) — full IAM chain scan
  return scan_iam_chain(iam_head_page_id, total_needed);
}

page_id_t IamManager::find_hint_page(page_id_t iam_head_page_id, uint32_t total_needed) {
  page_id_t hint_page_id = last_page_hint_[iam_head_page_id];
  Page* page = buffer_pool_.fetch_page(hint_page_id);
  if (!page) return INVALID_PAGE_ID;

  SlottedPage sp(page->get_data());
  page_id_t result = INVALID_PAGE_ID;
  if (sp.get_free_space() >= total_needed) {
    LOG_STORAGE_DEBUG("Hint hit: page {} has {} bytes free", hint_page_id, sp.get_free_space());
    result = hint_page_id;
  }
  buffer_pool_.unpin_page(hint_page_id, false);
  return result;
}

page_id_t IamManager::scan_extent_forward(page_id_t iam_head_page_id,
                                          page_id_t hint_page_id,
                                          uint32_t total_needed) {
  page_id_t extent_start = (hint_page_id / EXTENT_SIZE) * EXTENT_SIZE;
  for (int offset = static_cast<int>(hint_page_id - extent_start) + 1;
       offset < EXTENT_SIZE; ++offset) {
    page_id_t next_page_id = extent_start + offset;
    Page* page = buffer_pool_.fetch_page(next_page_id);
    if (!page) continue;

    SlottedPage sp(page->get_data());
    if (sp.get_free_space() >= total_needed) {
      LOG_STORAGE_DEBUG("Forward scan hit: page {} has {} bytes free", next_page_id, sp.get_free_space());
      last_page_hint_[iam_head_page_id] = next_page_id;
      buffer_pool_.unpin_page(next_page_id, false);
      return next_page_id;
    }
    buffer_pool_.unpin_page(next_page_id, false);
  }
  return INVALID_PAGE_ID;
}

page_id_t IamManager::scan_iam_chain(page_id_t iam_head_page_id, uint32_t total_needed) {
  page_id_t current_iam_page = iam_head_page_id;

  while (current_iam_page != INVALID_PAGE_ID) {
    Page* iam_page_obj = buffer_pool_.fetch_page(current_iam_page);
    if (!iam_page_obj) {
      LOG_STORAGE_ERROR("Failed to fetch IAM page {}", current_iam_page);
      return INVALID_PAGE_ID;
    }

    auto* iam_page = reinterpret_cast<const IAMPage*>(iam_page_obj->get_data());

    for (uint16_t i = 0; i < iam_page->extent_count; ++i) {
      page_id_t extent_start = first_page_of_extent(iam_page->extent_ids[i]);

      for (int page_offset = 0; page_offset < EXTENT_SIZE; ++page_offset) {
        page_id_t data_page_id = extent_start + page_offset;
        Page* data_page = buffer_pool_.fetch_page(data_page_id);
        if (!data_page) continue;

        SlottedPage sp(data_page->get_data());
        if (sp.get_free_space() >= total_needed) {
          LOG_STORAGE_DEBUG("Found page {} with {} bytes free", data_page_id, sp.get_free_space());
          last_page_hint_[iam_head_page_id] = data_page_id;
          buffer_pool_.unpin_page(data_page_id, false);
          buffer_pool_.unpin_page(current_iam_page, false);
          return data_page_id;
        }
        buffer_pool_.unpin_page(data_page_id, false);
      }
    }

    page_id_t next_iam = iam_page->next_page_id;
    buffer_pool_.unpin_page(current_iam_page, false);
    current_iam_page = next_iam;
  }

  LOG_STORAGE_DEBUG("No page found with sufficient space");
  return INVALID_PAGE_ID;
}

}

