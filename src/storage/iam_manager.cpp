#include "iam_manager.h"
#include "slotted_page.h"
#include "page_utils.h"
#include "common/logger.h"

namespace letty {

IamManager::IamManager(BufferPoolManager &buffer_pool, ExtentManager &extent_manager)
    : buffer_pool_(buffer_pool), extent_manager_(extent_manager) {}

page_id_t IamManager::get_shared_extent_page_id() {
  if (shared_extent_page_id_ != INVALID_PAGE_ID) {
    return shared_extent_page_id_;
  }

  auto db_header = load_page_value<DatabaseHeader>(buffer_pool_, HEADER_PAGE_ID);
  if (!db_header) {
    LOG_STORAGE_ERROR("Failed to fetch header page for shared extent lookup");
    return INVALID_PAGE_ID;
  }

  shared_extent_page_id_ = db_header->shared_extent_page_id;
  LOG_STORAGE_DEBUG("Cached shared_extent_page_id = {}", shared_extent_page_id_);
  return shared_extent_page_id_;
}

void IamManager::init_shared_extent(page_id_t pool_page_id) {
  LOG_STORAGE_DEBUG("Initializing shared extent at page {}", pool_page_id);

  Page* directory_frame = buffer_pool_.fetch_page(pool_page_id);
  if (!directory_frame) {
    LOG_STORAGE_ERROR("Failed to fetch shared extent page {}", pool_page_id);
    return;
  }

  auto directory_page = make_shared_extent_directory_page();
  // Write data into the page->char * from the struct.
  store_page_layout(directory_frame, directory_page);
  buffer_pool_.unpin_page(pool_page_id, true);

  // Cache it immediately since we know the value
  shared_extent_page_id_ = pool_page_id;
}

page_id_t IamManager::allocate_shared_page() {
  LOG_STORAGE_DEBUG("Allocating page from shared extent");

  page_id_t pool_page_id = get_shared_extent_page_id();
  if (pool_page_id == INVALID_PAGE_ID) {
    LOG_STORAGE_ERROR("No shared extent available");
    return INVALID_PAGE_ID;
  }

  page_id_t current_pool_page = pool_page_id;

  while (current_pool_page != INVALID_PAGE_ID) {
    Page* pool_frame = buffer_pool_.fetch_page(current_pool_page);
    if (!pool_frame) {
      LOG_STORAGE_ERROR("Failed to fetch shared extent page {}", current_pool_page);
      return INVALID_PAGE_ID;
    }

    auto pool_header = load_page_layout<SharedExtentDirectoryPage>(pool_frame);
    uint8_t free_slot = pool_header.find_free_slot();

    if (free_slot != SharedExtentDirectoryPage::NO_FREE_SLOT) {
      pool_header.mark_slot_used(free_slot);
      store_page_layout(pool_frame, pool_header);
      buffer_pool_.unpin_page(current_pool_page, true);

      // The directory page sits at current_pool_page; slots 1–7 are the adjacent
      // pages in the same extent. Slot N maps directly to page (current_pool_page + N).
      page_id_t allocated_page = current_pool_page + free_slot;
      LOG_STORAGE_INFO("Allocated shared page {} (slot {})", allocated_page, free_slot);
      return allocated_page;
    }

    // No free slot — advance or extend
    page_id_t next_pool = pool_header.next_pool_page;
    if (next_pool == INVALID_PAGE_ID) {
      next_pool = extend_shared_extent();
      if (next_pool == INVALID_PAGE_ID) {
        buffer_pool_.unpin_page(current_pool_page, false);
        return INVALID_PAGE_ID;
      }
      pool_header.next_pool_page = next_pool;
      store_page_layout(pool_frame, pool_header);
      buffer_pool_.unpin_page(current_pool_page, true);
    } else {
      buffer_pool_.unpin_page(current_pool_page, false);
    }
    current_pool_page = next_pool;
  }

  LOG_STORAGE_ERROR("Failed to allocate shared page - no extent available");
  return INVALID_PAGE_ID;
}

page_id_t IamManager::extend_shared_extent() {
  page_id_t new_pool_extent = extent_manager_.allocate_extent();
  if (new_pool_extent == INVALID_PAGE_ID) {
    LOG_STORAGE_ERROR("Failed to allocate new shared extent");
    return INVALID_PAGE_ID;
  }

  Page* new_directory_frame = buffer_pool_.new_page(new_pool_extent);
  if (!new_directory_frame) {
    new_directory_frame = buffer_pool_.fetch_page(new_pool_extent);
    if (new_directory_frame && (new_directory_frame->get_pin_count() != 1 || new_directory_frame->is_dirty())) {
      buffer_pool_.unpin_page(new_pool_extent, false);
      new_directory_frame = nullptr;
    }
  }
  if (!new_directory_frame) {
    LOG_STORAGE_ERROR("Failed to create new shared extent page {}", new_pool_extent);
    return INVALID_PAGE_ID;
  }
  auto shared_extent_directory_page = make_shared_extent_directory_page();
  store_page_layout(new_directory_frame, shared_extent_directory_page);
  buffer_pool_.unpin_page(new_pool_extent, true);

  LOG_STORAGE_INFO("Created new shared extent at page {}", new_pool_extent);
  return new_pool_extent;
}

page_id_t IamManager::create_iam_chain(const std::string &table_name) {
  LOG_STORAGE_DEBUG("Creating new IAM chain");

  page_id_t iam_page_id = allocate_shared_page();
  if (iam_page_id == INVALID_PAGE_ID) {
    LOG_STORAGE_ERROR("Failed to allocate IAM page from shared extent");
    return INVALID_PAGE_ID;
  }

  Page* iam_frame = buffer_pool_.new_page(iam_page_id);
  if (!iam_frame) {
    iam_frame = buffer_pool_.fetch_page(iam_page_id);
    if (iam_frame && (iam_frame->get_pin_count() != 1 || iam_frame->is_dirty())) {
      buffer_pool_.unpin_page(iam_page_id, false);
      iam_frame = nullptr;
    }
  }
  if (!iam_frame) {
    LOG_STORAGE_ERROR("Failed to create newly allocated IAM page {}", table_name, iam_page_id);
    return INVALID_PAGE_ID;
  }

  auto iam_page = make_iam_page();
  store_page_layout(iam_frame, iam_page);
  buffer_pool_.unpin_page(iam_page_id, true);

  LOG_STORAGE_INFO("Created new IAM chain for table {} at page {}", table_name, iam_page_id);
  return iam_page_id;
}


bool IamManager::page_has_space(page_id_t page_id, uint32_t required) {
  Page* data_frame = buffer_pool_.fetch_page(page_id);
  if (!data_frame) return false;
  SlottedPage sp(data_frame->get_data());
  bool has_space = sp.get_free_space() >= required;
  buffer_pool_.unpin_page(page_id, false);
  return has_space;
}

page_id_t IamManager::link_new_iam_page(page_id_t prev_iam_page, uint32_t extent_id) {
  page_id_t new_iam_page_id = allocate_shared_page();
  if (new_iam_page_id == INVALID_PAGE_ID) {
    LOG_STORAGE_ERROR("Failed to allocate new IAM page");
    return INVALID_PAGE_ID;
  }

  Page* new_iam_frame = buffer_pool_.new_page(new_iam_page_id);
  if (!new_iam_frame) {
    new_iam_frame = buffer_pool_.fetch_page(new_iam_page_id);
    if (new_iam_frame && (new_iam_frame->get_pin_count() != 1 || new_iam_frame->is_dirty())) {
      buffer_pool_.unpin_page(new_iam_page_id, false);
      new_iam_frame = nullptr;
    }
  }
  if (!new_iam_frame) {
    LOG_STORAGE_ERROR("Failed to create new IAM page {}", new_iam_page_id);
    return INVALID_PAGE_ID;
  }
  auto iam_page = make_iam_page();
  iam_page.add_extent(extent_id);
  store_page_layout(new_iam_frame, iam_page);
  buffer_pool_.unpin_page(new_iam_page_id, true);

  if (prev_iam_page == INVALID_PAGE_ID) return new_iam_page_id;

  Page* prev_iam_frame = buffer_pool_.fetch_page(prev_iam_page);
  if (!prev_iam_frame) {
    LOG_STORAGE_ERROR("Failed to fetch previous IAM page {} for linking", prev_iam_page);
    return new_iam_page_id;  // New IAM page exists but chain is broken — log and return
  }
  auto prev_iam = load_page_layout<IAMPage>(prev_iam_frame);
  prev_iam.next_page_id = new_iam_page_id;
  store_page_layout(prev_iam_frame, prev_iam);
  buffer_pool_.unpin_page(prev_iam_page, true);

  LOG_STORAGE_INFO("Created new IAM page {} and added extent {}", new_iam_page_id, extent_id);
  return new_iam_page_id;
}


page_id_t IamManager::allocate_extent_for_table(page_id_t iam_head_page_id) {
  LOG_STORAGE_DEBUG("Allocating extent for IAM chain {}", iam_head_page_id);

  if (iam_head_page_id == INVALID_PAGE_ID) {
    LOG_STORAGE_ERROR("Invalid IAM head page ID");
    return INVALID_PAGE_ID;
  }

  page_id_t extent_start_page = extent_manager_.allocate_extent();
  if (extent_start_page == INVALID_PAGE_ID) {
    LOG_STORAGE_ERROR("Failed to allocate physical extent");
    return INVALID_PAGE_ID;
  }

  uint32_t extent_id = extent_id_from_page(extent_start_page);
  LOG_STORAGE_DEBUG("Got extent {} (page {})", extent_id, extent_start_page);

  page_id_t current_iam_page = iam_head_page_id;
  page_id_t prev_iam_page = INVALID_PAGE_ID;

  while (current_iam_page != INVALID_PAGE_ID) {
    Page* iam_frame = buffer_pool_.fetch_page(current_iam_page);
    if (!iam_frame) {
      LOG_STORAGE_ERROR("Failed to fetch IAM page {}", current_iam_page);
      return INVALID_PAGE_ID;
    }

    auto iam_page = load_page_layout<IAMPage>(iam_frame);
    if (iam_page.has_space()) {
      iam_page.add_extent(extent_id);
      store_page_layout(iam_frame, iam_page);
      buffer_pool_.unpin_page(current_iam_page, true);
      LOG_STORAGE_INFO("Added extent {} to IAM page {}", extent_id, current_iam_page);
      table_page_hints_[iam_head_page_id] = extent_start_page;
      return extent_start_page;
    }

    prev_iam_page = current_iam_page;
    page_id_t next_iam = iam_page.next_page_id;
    buffer_pool_.unpin_page(current_iam_page, false);
    current_iam_page = next_iam;
  }

  // No IAM page has space — allocate a new one and link it
  if (link_new_iam_page(prev_iam_page, extent_id) == INVALID_PAGE_ID) {
    return INVALID_PAGE_ID;
  }

  table_page_hints_[iam_head_page_id] = extent_start_page;
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

  auto hint_it = table_page_hints_.find(iam_head_page_id);
  if (hint_it != table_page_hints_.end()) {
    page_id_t result = find_hint_page(iam_head_page_id, total_needed);
    if (result != INVALID_PAGE_ID) return result;

    result = scan_extent_forward(iam_head_page_id, hint_it->second, total_needed);
    if (result != INVALID_PAGE_ID) return result;
  }

  // Hint missed or hint extent exhausted — scan all extents in the IAM chain
  return scan_iam_chain(iam_head_page_id, total_needed);
}

page_id_t IamManager::find_hint_page(page_id_t iam_head_page_id, uint32_t total_needed) {
  page_id_t hint_page_id = table_page_hints_.at(iam_head_page_id);
  if (!page_has_space(hint_page_id, total_needed)) return INVALID_PAGE_ID;
  LOG_STORAGE_DEBUG("Hint hit: page {} has sufficient space", hint_page_id);
  return hint_page_id;
}

page_id_t IamManager::scan_extent_forward(page_id_t iam_head_page_id,
                                          page_id_t hint_page_id,
                                          uint32_t total_needed) {
  page_id_t extent_start = (hint_page_id / EXTENT_SIZE) * EXTENT_SIZE;
  for (int offset = static_cast<int>(hint_page_id - extent_start) + 1;
       offset < EXTENT_SIZE; ++offset) {
    page_id_t next_page_id = extent_start + offset;
    if (page_has_space(next_page_id, total_needed)) {
      LOG_STORAGE_DEBUG("Forward scan hit: page {} has sufficient space", next_page_id);
      table_page_hints_[iam_head_page_id] = next_page_id;
      return next_page_id;
    }
  }
  return INVALID_PAGE_ID;
}

page_id_t IamManager::scan_iam_chain(page_id_t iam_head_page_id, uint32_t total_needed) {
  page_id_t current_iam_page = iam_head_page_id;

  while (current_iam_page != INVALID_PAGE_ID) {
    auto iam_page = load_page_value<IAMPage>(buffer_pool_, current_iam_page);
    if (!iam_page) {
      LOG_STORAGE_ERROR("Failed to fetch IAM page {}", current_iam_page);
      return INVALID_PAGE_ID;
    }

    page_id_t next_iam = iam_page->next_page_id;

    for (uint16_t i = 0; i < iam_page->extent_count; ++i) {
      page_id_t extent_start = first_page_of_extent(iam_page->extent_ids[i]);
      for (int offset = 0; offset < EXTENT_SIZE; ++offset) {
        page_id_t data_page_id = extent_start + offset;
        if (page_has_space(data_page_id, total_needed)) {
          LOG_STORAGE_DEBUG("Found page {} with sufficient space", data_page_id);
          table_page_hints_[iam_head_page_id] = data_page_id;
          return data_page_id;
        }
      }
    }

    current_iam_page = next_iam;
  }

  LOG_STORAGE_DEBUG("No page found with sufficient space");
  return INVALID_PAGE_ID;
}

}
