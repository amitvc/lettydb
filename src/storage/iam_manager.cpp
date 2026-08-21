#include "iam_manager.h"
#include "slotted_page.h"
#include "page_utils.h"
#include "common/db_exception.h"
#include "common/logger.h"

namespace letty {

IamManager::IamManager(BufferPoolManager &buffer_pool, ExtentManager &extent_manager)
    : buffer_pool_(buffer_pool), extent_manager_(extent_manager) {}

page_id_t IamManager::create_iam_chain(const std::string &table_name) {
  LOG_STORAGE_DEBUG("Creating new IAM chain for table {}", table_name.c_str());

  page_id_t iam_page_id = extent_manager_.allocate_extent();
  if (iam_page_id == INVALID_PAGE_ID) {
    throw DbException(DbErrorCode::NoSpace, "failed to allocate extent for IAM chain of table '" + table_name + "'");
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
    throw DbException(DbErrorCode::IOError, "failed to create IAM page " + std::to_string(iam_page_id) +
                                            " for table '" + table_name + "'");
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
  // IAM pages fill their extent front to back, so the tail's position tells us
  // whether the next page is free or a new extent is needed.
  page_id_t new_iam_page_id;
  if (prev_iam_page != INVALID_PAGE_ID && (prev_iam_page % EXTENT_SIZE) < EXTENT_SIZE - 1) {
    new_iam_page_id = prev_iam_page + 1;
  } else {
    new_iam_page_id = extent_manager_.allocate_extent();
  }
  if (new_iam_page_id == INVALID_PAGE_ID) {
    throw DbException(DbErrorCode::NoSpace, "failed to allocate new IAM page");
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
    throw DbException(DbErrorCode::IOError, "failed to create new IAM page " + std::to_string(new_iam_page_id));
  }
  auto iam_page = make_iam_page();
  iam_page.add_extent(extent_id);
  store_page_layout(new_iam_frame, iam_page);
  buffer_pool_.unpin_page(new_iam_page_id, true);

  if (prev_iam_page == INVALID_PAGE_ID) return new_iam_page_id;

  Page* prev_iam_frame = buffer_pool_.fetch_page(prev_iam_page);
  if (!prev_iam_frame) {
    throw DbException(DbErrorCode::IOError, "failed to fetch previous IAM page " + std::to_string(prev_iam_page) +
                                            " while linking IAM chain");
  }
  auto prev_iam = load_page_layout<IAMPage>(prev_iam_frame);
  prev_iam.next_page_id = new_iam_page_id;
  store_page_layout(prev_iam_frame, prev_iam);
  buffer_pool_.unpin_page(prev_iam_page, true);

  LOG_STORAGE_INFO("Created new IAM page {} and added extent {}", new_iam_page_id, extent_id);
  return new_iam_page_id;
}


page_id_t IamManager::allocate_extent_for_table(page_id_t iam_head_page_id) {
  LOG_STORAGE_DEBUG("Allocating extent for IAM chain at page {}", iam_head_page_id);

  if (iam_head_page_id == INVALID_PAGE_ID) {
    throw DbException(DbErrorCode::InvalidArgument, "invalid IAM head page ID");
  }

  page_id_t extent_start_page = extent_manager_.allocate_extent();
  if (extent_start_page == INVALID_PAGE_ID) {
    throw DbException(DbErrorCode::NoSpace, "failed to allocate physical extent for table");
  }

  uint32_t extent_id = extent_id_from_page(extent_start_page);

  page_id_t current_iam_page = iam_head_page_id;
  page_id_t prev_iam_page = INVALID_PAGE_ID;

  while (current_iam_page != INVALID_PAGE_ID) {
    Page* iam_frame = buffer_pool_.fetch_page(current_iam_page);
    if (!iam_frame) {
      throw DbException(DbErrorCode::IOError, "failed to fetch IAM page " + std::to_string(current_iam_page));
    }

    auto iam_page = load_page_layout<IAMPage>(iam_frame);
    if (iam_page.has_space()) {
      iam_page.add_extent(extent_id);
      store_page_layout(iam_frame, iam_page);
      buffer_pool_.unpin_page(current_iam_page, true);
      LOG_STORAGE_DEBUG("Added extent {} to IAM page {}", extent_id, current_iam_page);
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
    throw DbException(DbErrorCode::NoSpace, "failed to link new IAM page");
  }

  table_page_hints_[iam_head_page_id] = extent_start_page;
  return extent_start_page;
}


page_id_t IamManager::find_page_with_space(page_id_t iam_head_page_id, uint32_t required_space) {
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
      throw DbException(DbErrorCode::IOError, "failed to fetch IAM page " + std::to_string(current_iam_page));
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
