#include "iam_manager.h"
#include "slotted_page.h"
#include "page_utils.h"
#include "log_record.h"
#include "common/db_exception.h"
#include "common/logger.h"

namespace letty {

IamManager::IamManager(BufferPoolManager &buffer_pool, ExtentManager &extent_manager,
                        LogManager &log_manager, TransactionManager &txn_manager)
    : buffer_pool_(buffer_pool), extent_manager_(extent_manager),
      log_manager_(log_manager), txn_manager_(txn_manager) {}

page_id_t IamManager::create_iam_chain(const std::string &table_name, uint64_t txn_id) {
  LOG_STORAGE_DEBUG("Creating new IAM chain for table {}", table_name.c_str());

  page_id_t iam_page_id = extent_manager_.allocate_extent(txn_id);
  if (iam_page_id == INVALID_PAGE_ID) {
    throw DbException(DbErrorCode::NoSpace, "failed to allocate extent for IAM chain of table '" + table_name + "'");
  }

  PageGuard guard = buffer_pool_.allocate_page_guard(iam_page_id);
  if (!guard) {
    throw DbException(DbErrorCode::IOError, "failed to create IAM page " + std::to_string(iam_page_id) +
                                            " for table '" + table_name + "'");
  }

  auto iam_page = make_iam_page();
  store_page_layout(guard, iam_page);

  LOG_STORAGE_INFO("Created new IAM chain for table {} at page {}", table_name, iam_page_id);
  return iam_page_id;
}


bool IamManager::page_has_space(page_id_t page_id, uint32_t required) {
  PageGuard guard = buffer_pool_.fetch_page_guard(page_id);
  if (!guard) return false;
  SlottedPage sp = SlottedPage::from_page(guard);
  return sp.get_free_space() >= required;
}

page_id_t IamManager::link_new_iam_page(page_id_t prev_iam_page, uint32_t extent_id, uint64_t txn_id) {
  // IAM pages fill their extent front to back, so the tail's position tells us
  // whether the next page is free or a new extent is needed.
  page_id_t new_iam_page_id;
  if (prev_iam_page != INVALID_PAGE_ID && (prev_iam_page % EXTENT_SIZE) < EXTENT_SIZE - 1) {
    new_iam_page_id = prev_iam_page + 1;
  } else {
    new_iam_page_id = extent_manager_.allocate_extent(txn_id);
  }
  if (new_iam_page_id == INVALID_PAGE_ID) {
    throw DbException(DbErrorCode::NoSpace, "failed to allocate new IAM page");
  }

  PageGuard new_iam_guard = buffer_pool_.allocate_page_guard(new_iam_page_id);
  if (!new_iam_guard) {
    throw DbException(DbErrorCode::IOError, "failed to create new IAM page " + std::to_string(new_iam_page_id));
  }
  auto iam_page = make_iam_page();
  iam_page.add_extent(extent_id);
  iam_page.page_lsn = log_iam_extent_change(LogRecordType::IAM_ADD_EXTENT, txn_id, new_iam_page_id, extent_id);
  store_page_layout(new_iam_guard, iam_page);

  if (prev_iam_page == INVALID_PAGE_ID){
	return new_iam_page_id;
  }

  PageGuard prev_iam_guard = buffer_pool_.fetch_page_guard(prev_iam_page);
  if (!prev_iam_guard) {
    throw DbException(DbErrorCode::IOError, "failed to fetch previous IAM page " + std::to_string(prev_iam_page) +
                                            " while linking IAM chain");
  }
  auto prev_iam = load_page_layout<IAMPage>(prev_iam_guard);

  ByteBuffer before_image(sizeof(page_id_t));
  write_le32(before_image.data(), prev_iam.next_page_id);

  prev_iam.next_page_id = new_iam_page_id;

  ByteBuffer after_image(sizeof(page_id_t));
  write_le32(after_image.data(), prev_iam.next_page_id);

  PagePatch patch;
  patch.page_id = prev_iam_page;
  patch.offset = 0;
  patch.before_payload = before_image;
  patch.after_payload = after_image;

  LogRecord wal_record{};
  if (build_page_patch_record(txn_id, patch, wal_record)) {
    wal_record.header.prev_lsn = txn_manager_.get_last_lsn(txn_id);
    lsn_t lsn = log_manager_.append_record(wal_record);
    if (lsn != INVALID_LSN) {
      txn_manager_.record_lsn(txn_id, lsn);
	  buffer_pool_.update_dirtied_page_lsn(prev_iam_page, lsn);
      prev_iam.page_lsn = lsn;
    }
  }

  store_page_layout(prev_iam_guard, prev_iam);

  LOG_STORAGE_INFO("Created new IAM page {} and added extent {}", new_iam_page_id, extent_id);
  return new_iam_page_id;
}


page_id_t IamManager::allocate_extent_for_table(page_id_t iam_head_page_id, uint64_t txn_id) {
  LOG_STORAGE_DEBUG("Allocating extent for IAM chain at page {}", iam_head_page_id);

  if (iam_head_page_id == INVALID_PAGE_ID) {
    throw DbException(DbErrorCode::InvalidArgument, "invalid IAM head page ID");
  }

  page_id_t extent_start_page = extent_manager_.allocate_extent(txn_id);
  if (extent_start_page == INVALID_PAGE_ID) {
    throw DbException(DbErrorCode::NoSpace, "failed to allocate physical extent for table");
  }

  uint32_t extent_id = extent_id_from_page(extent_start_page);

  page_id_t current_iam_page = iam_head_page_id;
  page_id_t prev_iam_page = INVALID_PAGE_ID;

  while (current_iam_page != INVALID_PAGE_ID) {
    PageGuard iam_guard = buffer_pool_.fetch_page_guard(current_iam_page);
    if (!iam_guard) {
      throw DbException(DbErrorCode::IOError, "failed to fetch IAM page " + std::to_string(current_iam_page));
    }

    auto iam_page = load_page_layout<IAMPage>(iam_guard);
    if (iam_page.has_space()) {
      iam_page.add_extent(extent_id);
      iam_page.page_lsn = log_iam_extent_change(LogRecordType::IAM_ADD_EXTENT, txn_id, current_iam_page, extent_id);
      store_page_layout(iam_guard, iam_page);
      LOG_STORAGE_DEBUG("Added extent {} to IAM page {}", extent_id, current_iam_page);
      table_page_hints_[iam_head_page_id] = extent_start_page;
      return extent_start_page;
    }

    prev_iam_page = current_iam_page;
    current_iam_page = iam_page.next_page_id;
  }

  // No IAM page has space — allocate a new one and link it
  if (link_new_iam_page(prev_iam_page, extent_id, txn_id) == INVALID_PAGE_ID) {
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

bool IamManager::remove_extent_from_iam(page_id_t iam_head_page_id, uint32_t extent_id, uint64_t txn_id) {
  page_id_t current_iam_page = iam_head_page_id;
  page_id_t prev_iam_page = INVALID_PAGE_ID;

  while (current_iam_page != INVALID_PAGE_ID) {
    PageGuard iam_guard = buffer_pool_.fetch_page_guard(current_iam_page);
    if (!iam_guard) return false;

    auto iam_page = load_page_layout<IAMPage>(iam_guard);
    page_id_t next_iam = iam_page.next_page_id;

    if (!iam_page.remove_extent(extent_id)) {
      prev_iam_page = current_iam_page;
      current_iam_page = next_iam;
      continue;
    }

    iam_page.page_lsn = log_iam_extent_change(LogRecordType::IAM_REMOVE_EXTENT, txn_id, current_iam_page, extent_id);
    store_page_layout(iam_guard, iam_page);

    // Deallocate the extent from the GAM so the system can reuse it.
    page_id_t extent_start = first_page_of_extent(extent_id);
    extent_manager_.deallocate_extent(extent_start, txn_id);

    if (iam_page.extent_count == 0 && current_iam_page != iam_head_page_id) {
      PageGuard prev_guard = buffer_pool_.fetch_page_guard(prev_iam_page);
      if (!prev_guard) return false;

      auto prev_iam = load_page_layout<IAMPage>(prev_guard);

      ByteBuffer before_image(sizeof(page_id_t));
      write_le32(before_image.data(), prev_iam.next_page_id);

      prev_iam.next_page_id = next_iam;

      ByteBuffer after_image(sizeof(page_id_t));
      write_le32(after_image.data(), prev_iam.next_page_id);

      PagePatch patch;
      patch.page_id = prev_iam_page;
      patch.offset = 0;
      patch.before_payload = before_image;
      patch.after_payload = after_image;

      LogRecord wal_record{};
      if (build_page_patch_record(txn_id, patch, wal_record)) {
        wal_record.header.prev_lsn = txn_manager_.get_last_lsn(txn_id);
        lsn_t lsn = log_manager_.append_record(wal_record);
        if (lsn != INVALID_LSN) {
          txn_manager_.record_lsn(txn_id, lsn);
		  buffer_pool_.update_dirtied_page_lsn(prev_iam_page, lsn);
          prev_iam.page_lsn = lsn;
        }
      }

      store_page_layout(prev_guard, prev_iam);
    }

    table_page_hints_.erase(iam_head_page_id);
    LOG_STORAGE_INFO("Removed extent {} from IAM chain", extent_id);
    return true;
  }

  return false;
}

lsn_t IamManager::log_iam_extent_change(LogRecordType type, uint64_t txn_id, page_id_t iam_page_id,
                                         uint32_t extent_id) {
  IamPatch patch;
  patch.iam_page_id = iam_page_id;
  patch.extent_id = extent_id;

  LogRecord wal_record{};
  if (!build_iam_extent_record(type, txn_id, patch, wal_record)) {
    throw DbException(DbErrorCode::Internal, "Failed to build WAL record for IAM extent change");
  }
  wal_record.header.prev_lsn = txn_manager_.get_last_lsn(txn_id);

  lsn_t lsn = log_manager_.append_record(wal_record);
  if (lsn == INVALID_LSN) {
    throw DbException(DbErrorCode::Internal, "Failed to append WAL record for IAM extent change");
  }

  txn_manager_.record_lsn(txn_id, lsn);
  buffer_pool_.update_dirtied_page_lsn(iam_page_id, lsn);
  return lsn;
}

}
