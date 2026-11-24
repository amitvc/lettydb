//
// Created by Amit Chavan on 11/22/26.
//

#include "page_allocator.h"

#include <cstring>

#include "common/db_exception.h"
#include "common/logger.h"
#include "storage/log_record.h"
#include "storage/page_utils.h"
#include "storage/slotted_page.h"

namespace letty {

PageAllocator::PageAllocator(BufferPoolManager &buffer_pool,
							 LogManager &log_manager,
							 TransactionManager &txn_manager)
	: bpm_(buffer_pool),
	  log_manager_(log_manager),
	  txn_manager_(txn_manager) {
  if (bpm_.get_file_size_in_pages() == 0) {
	initialize_new_db();
  } else {
	validate_existing_db();
  }
}

page_id_t PageAllocator::allocate_page(uint16_t table_id, PageType type, txn_id tran_id) {
  std::lock_guard<std::mutex> guard(lock_);

  PageGuard db_header_guard = bpm_.fetch_page_guard(DATABASE_HEADER_PAGE_ID);
  if (!db_header_guard) {
    LOG_STORAGE_ERROR("Failed to fetch page 0 during allocate_page");
    return INVALID_PAGE_ID;
  }

  DatabaseMetadata db_meta = load_page_body<DatabaseMetadata>(db_header_guard);
  DbMetadataState before_db_state = extract_state(db_meta);

  page_id_t target_page_id = INVALID_PAGE_ID;
  PageGuard target_page_guard;
  PageHeader target_current_header{};

  if (db_meta.free_page_list_head_page != INVALID_PAGE_ID) {
    target_page_id = db_meta.free_page_list_head_page;
    target_page_guard = bpm_.fetch_page_guard(target_page_id);
    if (!target_page_guard) {
      LOG_STORAGE_ERROR("Failed to fetch free page {} during allocate_page", target_page_id);
      return INVALID_PAGE_ID;
    }
    target_current_header = load_page_layout<PageHeader>(target_page_guard);
    db_meta.free_page_list_head_page = target_current_header.next_page_id;
  } else {
    target_page_id = static_cast<page_id_t>(db_meta.page_count);
    target_page_guard = bpm_.allocate_page_guard(target_page_id);
    if (!target_page_guard) {
      LOG_STORAGE_ERROR("Failed to allocate new page {} during allocate_page", target_page_id);
      return INVALID_PAGE_ID;
    }
    db_meta.page_count++;
  }

  if (!apply_db_metadata_update(db_header_guard, before_db_state, db_meta, tran_id)) {
    return INVALID_PAGE_ID;
  }

  PageHeader target_modified_header{};
  target_modified_header.table_id = table_id;
  target_modified_header.page_type = type;

  if (!apply_page_header_update(target_page_guard, target_page_id,
                                target_current_header, target_modified_header, tran_id)) {
    return INVALID_PAGE_ID;
  }

  if (type == PageType::DATA) {
    SlottedPage::init(reinterpret_cast<Byte*>(target_page_guard.get_body()));
    target_page_guard.mark_dirty();
  }

  return target_page_id;
}

bool PageAllocator::free_page_locked(page_id_t page_id, txn_id tran_id) {
  if (page_id <= DATABASE_HEADER_PAGE_ID) {
    LOG_PAGE_ALLOCATOR_WARN("Tried to free page {} which is a restricted action", page_id);
    return false;
  }

  PageGuard db_header_guard = bpm_.fetch_page_guard(DATABASE_HEADER_PAGE_ID);
  if (!db_header_guard) {
    LOG_PAGE_ALLOCATOR_ERROR("Failed to fetch database header page during free_page");
    return false;
  }

  PageGuard target_page_guard = bpm_.fetch_page_guard(page_id);
  if (!target_page_guard) {
    LOG_PAGE_ALLOCATOR_ERROR("Failed to fetch page {} during free_page", page_id);
    return false;
  }

  PageHeader target_current_header = load_page_layout<PageHeader>(target_page_guard);
  if (target_current_header.page_type == PageType::FREE) {
    LOG_PAGE_ALLOCATOR_WARN("Page {} is already free", page_id);
    return false;
  }

  DatabaseMetadata db_meta = load_page_body<DatabaseMetadata>(db_header_guard);
  page_id_t old_head = db_meta.free_page_list_head_page;

  PageHeader target_modified_header{};
  target_modified_header.table_id = 0;
  target_modified_header.page_type = PageType::FREE;
  target_modified_header.next_page_id = old_head;
  target_modified_header.prev_page_id = INVALID_PAGE_ID;

  if (!apply_page_header_update(target_page_guard, page_id, target_current_header, target_modified_header, tran_id)) {
    LOG_PAGE_ALLOCATOR_ERROR("Failed to apply page header update while freeing page {}", page_id);
    return false;
  }

  DbMetadataState before_db_state = extract_state(db_meta);
  db_meta.free_page_list_head_page = page_id;

  if (!apply_db_metadata_update(db_header_guard, before_db_state, db_meta, tran_id)) {
    LOG_PAGE_ALLOCATOR_ERROR("Failed to apply db metadata update while freeing page {}", page_id);
    return false;
  }

  return true;
}

bool PageAllocator::free_page(page_id_t page_id, txn_id tran_id) {
  std::lock_guard<std::mutex> guard(lock_);
  return free_page_locked(page_id, tran_id);
}

bool PageAllocator::unlink_page(page_id_t page_id, txn_id tran_id) {
  if (page_id <= DATABASE_HEADER_PAGE_ID) {
    LOG_PAGE_ALLOCATOR_WARN("Cannot unlink database header page or invalid page: {}", page_id);
    return false;
  }

  std::lock_guard<std::mutex> guard(lock_);

  PageGuard target_page_guard = bpm_.fetch_page_guard(page_id);
  if (!target_page_guard) {
    LOG_PAGE_ALLOCATOR_ERROR("Failed to fetch target page {} during unlink_page", page_id);
    return false;
  }

  auto target_current_header = load_page_layout<PageHeader>(target_page_guard);
  if (target_current_header.page_type == PageType::FREE) {
    LOG_PAGE_ALLOCATOR_WARN("Cannot unlink page {} because it is already free", page_id);
    return false;
  }

  page_id_t prev_page_id = target_current_header.prev_page_id;
  page_id_t next_page_id = target_current_header.next_page_id;

  if (prev_page_id != INVALID_PAGE_ID) {
    PageGuard prev_page_guard = bpm_.fetch_page_guard(prev_page_id);
    if (!prev_page_guard) {
      LOG_PAGE_ALLOCATOR_ERROR("Failed to fetch prev page {} during unlink_page", prev_page_id);
      return false;
    }
    PageHeader prev_current_header = load_page_layout<PageHeader>(prev_page_guard);
    PageHeader prev_modified_header = prev_current_header;
    prev_modified_header.next_page_id = next_page_id;

    if (!apply_page_header_update(prev_page_guard, prev_page_id, prev_current_header, prev_modified_header, tran_id)) {
      LOG_PAGE_ALLOCATOR_ERROR("Failed to update prev page {} during unlink_page", prev_page_id);
      return false;
    }
  }

  if (next_page_id != INVALID_PAGE_ID) {
    PageGuard next_page_guard = bpm_.fetch_page_guard(next_page_id);
    if (!next_page_guard) {
      LOG_PAGE_ALLOCATOR_ERROR("Failed to fetch next page {} during unlink_page", next_page_id);
      return false;
    }
    PageHeader next_current_header = load_page_layout<PageHeader>(next_page_guard);
    PageHeader next_modified_header = next_current_header;
    next_modified_header.prev_page_id = prev_page_id;

    if (!apply_page_header_update(next_page_guard, next_page_id, next_current_header, next_modified_header, tran_id)) {
      LOG_PAGE_ALLOCATOR_ERROR("Failed to update next page {} during unlink_page", next_page_id);
      return false;
    }
  }

  PageHeader target_modified_header = target_current_header;
  target_modified_header.prev_page_id = INVALID_PAGE_ID;
  target_modified_header.next_page_id = INVALID_PAGE_ID;

  if (!apply_page_header_update(target_page_guard, page_id, target_current_header, target_modified_header, tran_id)) {
    LOG_PAGE_ALLOCATOR_ERROR("Failed to clear links on unlinked page {}", page_id);
    return false;
  }

  return true;
}

bool PageAllocator::link_page(page_id_t tail_page_id, page_id_t new_page_id, txn_id tran_id) {
  if (tail_page_id == INVALID_PAGE_ID || new_page_id == INVALID_PAGE_ID) {
	LOG_PAGE_ALLOCATOR_WARN("Cannot process page linking when page ids are invalid. "
							"tail_page_id = {}, new_page_id = {}", tail_page_id, new_page_id);
	return false;
  }

  if (tail_page_id == new_page_id) {
    LOG_PAGE_ALLOCATOR_WARN("Cannot link a page to itself: page_id = {}", tail_page_id);
    return false;
  }

  if (tail_page_id <= DATABASE_HEADER_PAGE_ID || new_page_id <= DATABASE_HEADER_PAGE_ID) {
    LOG_PAGE_ALLOCATOR_WARN("Cannot link database header page. tail_page_id = {}, new_page_id = {}",
                            tail_page_id, new_page_id);
    return false;
  }

  std::lock_guard<std::mutex> guard(lock_);

  PageGuard tail_page_guard = bpm_.fetch_page_guard(tail_page_id);
  if (!tail_page_guard) {
    LOG_PAGE_ALLOCATOR_ERROR("Failed to fetch tail page {} during link_page", tail_page_id);
    return false;
  }

  PageGuard new_page_guard = bpm_.fetch_page_guard(new_page_id);
  if (!new_page_guard) {
    LOG_PAGE_ALLOCATOR_ERROR("Failed to fetch new page {} during link_page", new_page_id);
    return false;
  }

  PageHeader tail_current_header = load_page_layout<PageHeader>(tail_page_guard);
  PageHeader new_current_header = load_page_layout<PageHeader>(new_page_guard);

  PageHeader tail_modified_header = tail_current_header;
  tail_modified_header.next_page_id = new_page_id;

  if (!apply_page_header_update(tail_page_guard, tail_page_id, tail_current_header, tail_modified_header, tran_id)) {
    LOG_PAGE_ALLOCATOR_ERROR("Failed to apply page header update on tail page {}", tail_page_id);
    return false;
  }

  PageHeader new_modified_header = new_current_header;
  new_modified_header.prev_page_id = tail_page_id;

  if (!apply_page_header_update(new_page_guard, new_page_id, new_current_header, new_modified_header, tran_id)) {
    LOG_PAGE_ALLOCATOR_ERROR("Failed to apply page header update on new page {}", new_page_id);
    return false;
  }

  return true;
}

bool PageAllocator::free_page_chain(page_id_t first_page_id, txn_id tran_id) {
  if (first_page_id == INVALID_PAGE_ID) {
    return true;
  }

  if (first_page_id <= DATABASE_HEADER_PAGE_ID) {
    LOG_PAGE_ALLOCATOR_WARN("Cannot free page chain starting at invalid or header page: {}", first_page_id);
    return false;
  }

  std::lock_guard<std::mutex> guard(lock_);
  page_id_t current_page_id = first_page_id;

  while (current_page_id != INVALID_PAGE_ID) {
    PageGuard page_guard = bpm_.fetch_page_guard(current_page_id);
    if (!page_guard) {
      LOG_PAGE_ALLOCATOR_ERROR("Failed to fetch page {} during free_page_chain", current_page_id);
      return false;
    }

    PageHeader current_header = load_page_layout<PageHeader>(page_guard);
    page_id_t next_page_id = current_header.next_page_id;

    page_guard.drop(); // calling drop unpins the page without marking it dirty

    if (!free_page_locked(current_page_id, tran_id)) {
      LOG_PAGE_ALLOCATOR_ERROR("Failed to free page {} in chain", current_page_id);
      return false;
    }

    current_page_id = next_page_id;
  }

  return true;
}

void PageAllocator::initialize_new_db() {
  PageGuard header_guard = bpm_.allocate_page_guard(DATABASE_HEADER_PAGE_ID);
  if (!header_guard) {
	throw DbException(DbErrorCode::IOError, "Failed to allocate database header page");
  }

  PageHeader page_header{};
  page_header.page_lsn = 0;
  page_header.next_page_id = INVALID_PAGE_ID;
  page_header.prev_page_id = INVALID_PAGE_ID;
  page_header.table_id = 0;
  page_header.page_type = PageType::DATABASE_HEADER;
  store_page_layout(header_guard, page_header);

  DatabaseMetadata metadata = make_database_metadata();
  store_page_body(header_guard, metadata);

  if (!bpm_.flush_page(DATABASE_HEADER_PAGE_ID)) {
	throw DbException(DbErrorCode::IOError, "Failed to flush database header page");
  }

  LOG_STORAGE_INFO("Initialized new database file (Page 0 metadata written)");
}

void PageAllocator::validate_existing_db() {
  PageGuard header_guard = bpm_.fetch_page_guard(DATABASE_HEADER_PAGE_ID);
  if (!header_guard) {
	throw DbException(DbErrorCode::IOError, "Failed to read database header page");
  }

  auto page_header = load_page_layout<PageHeader>(header_guard);
  if (page_header.page_type != PageType::DATABASE_HEADER) {
	throw DbException(DbErrorCode::Corruption, "Page 0 is not a valid database header page");
  }

  auto metadata = load_page_body<DatabaseMetadata>(header_guard);
  if (std::memcmp(metadata.signature, DB_SIGNATURE, sizeof(DB_SIGNATURE)) != 0) {
	throw DbException(DbErrorCode::Corruption, "Invalid database file signature");
  }
  if (metadata.version != 1) {
	throw DbException(DbErrorCode::Corruption,
					  "Unsupported database format version: " + std::to_string(metadata.version));
  }
  if (metadata.page_size != DEFAULT_PAGE_SIZE) {
	throw DbException(DbErrorCode::Corruption,
					  "Unsupported database page size: " + std::to_string(metadata.page_size));
  }

  LOG_STORAGE_INFO("Validated existing database file (pages={}, free_head={})",
				   metadata.page_count, metadata.free_page_list_head_page);
}

DbMetadataState PageAllocator::extract_state(const DatabaseMetadata &metadata) {
  DbMetadataState state{};
  state.free_page_list_head_page = metadata.free_page_list_head_page;
  state.page_count = metadata.page_count;
  state.table_count = metadata.table_count;
  state.next_table_id = metadata.next_table_id;
  return state;
}

lsn_t PageAllocator::log_db_metadata_change(txn_id tran_id,
											const DbMetadataState &before,
											const DbMetadataState &after) {
  DbMetadataPatch patch{before, after};
  LogRecord wal_record{};
  if (!build_db_metadata_record(tran_id, patch, wal_record)) {
	LOG_STORAGE_ERROR("Failed to build DB_METADATA_UPDATE WAL record");
	return INVALID_LSN;
  }

  wal_record.header.prev_lsn = txn_manager_.get_last_lsn(tran_id);
  lsn_t lsn = log_manager_.append_record(wal_record);
  if (lsn == INVALID_LSN) {
	LOG_STORAGE_ERROR("Failed to append DB_METADATA_UPDATE WAL record");
	return INVALID_LSN;
  }

  txn_manager_.record_lsn(tran_id, lsn);
  bpm_.update_dirtied_page_lsn(DATABASE_HEADER_PAGE_ID, lsn);
  return lsn;
}

lsn_t PageAllocator::log_page_header_change(txn_id tran_id,
											page_id_t page_id,
											const PageHeader &before,
											const PageHeader &after) {
  PageHeaderPatch patch{page_id, before, after};
  LogRecord wal_record{};
  if (!build_page_header_record(tran_id, patch, wal_record)) {
	LOG_STORAGE_ERROR("Failed to build PAGE_HEADER_UPDATE WAL record for page {}", page_id);
	return INVALID_LSN;
  }

  wal_record.header.prev_lsn = txn_manager_.get_last_lsn(tran_id);
  lsn_t lsn = log_manager_.append_record(wal_record);
  if (lsn == INVALID_LSN) {
	LOG_STORAGE_ERROR("Failed to append PAGE_HEADER_UPDATE WAL record for page {}", page_id);
	return INVALID_LSN;
  }

  txn_manager_.record_lsn(tran_id, lsn);
  bpm_.update_dirtied_page_lsn(page_id, lsn);
  return lsn;
}

bool PageAllocator::apply_db_metadata_update(PageGuard& db_header_guard,
                                             const DbMetadataState& before_state,
                                             const DatabaseMetadata& modified_meta,
                                             txn_id tran_id) {
  DbMetadataState after_state = extract_state(modified_meta);
  lsn_t lsn = log_db_metadata_change(tran_id, before_state, after_state);
  if (lsn == INVALID_LSN) {
    return false;
  }

  db_header_guard.get_page()->set_page_lsn(lsn);
  store_page_body(db_header_guard, modified_meta);
  return true;
}

bool PageAllocator::apply_page_header_update(PageGuard& page_guard,
                                             page_id_t page_id,
                                             const PageHeader& current_header,
                                             PageHeader modified_header,
                                             txn_id tran_id) {
  lsn_t lsn = log_page_header_change(tran_id, page_id, current_header, modified_header);
  if (lsn == INVALID_LSN) {
    return false;
  }

  modified_header.page_lsn = lsn;
  store_page_layout(page_guard, modified_header);
  return true;
}

}
