#include "table_manager.h"

#include <algorithm>
#include <cassert>
#include <optional>

#include "common/db_exception.h"
#include "common/logger.h"
#include "log_record.h"
#include "page_utils.h"
#include "slotted_page.h"
#include "storage_def.h"

namespace letty {

TableManager::TableManager(BufferPoolManager& buffer_pool, CatalogManager& catalog_manager,
                           LogManager& log_manager, TransactionManager& txn_manager, PageAllocator& page_allocator)
  : buffer_pool_(buffer_pool),
    catalog_manager_(catalog_manager),
    log_manager_(log_manager),
    txn_manager_(txn_manager),
    page_allocator_(page_allocator) {}

std::optional<uint16_t> TableManager::insert_row(const std::string& table_name, const Tuple& tuple, uint64_t txn_id) {
  auto meta = catalog_manager_.get_table(table_name);
  if (!meta) {
    throw DbException(DbErrorCode::UndefinedTable, "table '" + table_name + "' does not exist");
  }
  return insert_row(meta.value(), tuple, txn_id);
}

std::optional<uint16_t> TableManager::insert_row(const TableMetadata& meta, const Tuple& tuple, uint64_t txn_id) {
  std::lock_guard<std::mutex> table_lock(get_table_latch(meta.table_id));
  Byte serialized_tuple[PAGE_SIZE];
  uint32_t data_size = tuple.serialize(meta.schema, serialized_tuple, PAGE_SIZE);
  if (data_size == 0) {
    return std::nullopt;
  }

  page_id_t target_page_id = acquire_page_for_insert(meta, data_size, txn_id);
  LOG_STORAGE_DEBUG("TableManager inserting to page: {}", target_page_id);
  PageGuard guard = buffer_pool_.fetch_page_guard(target_page_id);
  assert(guard.is_valid());

  SlottedPage sp = SlottedPage::from_page(guard);
  auto slot_id = sp.insert_tuple(reinterpret_cast<const Byte*>(serialized_tuple), data_size);
  if (!slot_id.has_value()) {
    throw DbException(DbErrorCode::Internal, "Failed to insert tuple after ensuring target page has space");
  }

  PagePatch patch;
  patch.page_id = target_page_id;
  patch.offset = slot_id.value();
  patch.after_payload = ByteBuffer(serialized_tuple, serialized_tuple + data_size);

  LogRecord wal_record{};
  if (!build_page_patch_record(txn_id, patch, wal_record)) {
    throw DbException(DbErrorCode::Internal, "Failed to build WAL record for insert");
  }
  wal_record.header.prev_lsn = txn_manager_.get_last_lsn(txn_id);

  lsn_t lsn = log_manager_.append_record(wal_record);
  if (lsn == INVALID_LSN) {
    throw DbException(DbErrorCode::Internal, "Failed to append WAL record for insert");
  }

  txn_manager_.record_lsn(txn_id, lsn);
  buffer_pool_.update_dirtied_page_lsn(target_page_id, lsn);
  guard.get_page()->set_page_lsn(lsn);
  guard.mark_dirty();

  return slot_id;
}

uint32_t TableManager::insert_rows(const std::string& table_name, const std::vector<Tuple>& tuples, uint64_t txn_id) {
  auto meta = catalog_manager_.get_table(table_name);
  if (!meta) {
    throw DbException(DbErrorCode::UndefinedTable, "table '" + table_name + "' does not exist");
  }

  uint32_t inserted_cnt = 0;
  for (const Tuple& tuple : tuples) {
    if (insert_row(meta->get(), tuple, txn_id).has_value()) {
      ++inserted_cnt;
    }
  }
  return inserted_cnt;
}

TableScanner TableManager::scan_table(const std::string& table_name) {
  auto meta = catalog_manager_.get_table(table_name);
  if (!meta) {
    throw DbException(DbErrorCode::UndefinedTable, "table '" + table_name + "' does not exist");
  }
  return TableScanner(buffer_pool_, meta->get().first_page_id);
}

bool TableManager::delete_row(const std::string& table_name, page_id_t page_id, uint16_t slot_id, uint64_t txn_id) {
  auto meta = catalog_manager_.get_table(table_name);
  if (!meta.has_value()) {
    throw DbException(DbErrorCode::UndefinedTable, "table '" + table_name + "' does not exist");
  }
  return delete_row(meta->get(), page_id, slot_id, txn_id);
}

bool TableManager::delete_row(const TableMetadata& meta, page_id_t page_id, uint16_t slot_id, uint64_t txn_id) {
  std::lock_guard<std::mutex> table_lock(get_table_latch(meta.table_id));
  PageGuard guard = buffer_pool_.fetch_page_guard(page_id);
  if (!guard) {
    LOG_STORAGE_ERROR("Failed to fetch page {} for delete on table '{}'", page_id, meta.name);
    return false;
  }

  SlottedPage sp = SlottedPage::from_page(guard);
  uint32_t tuple_size = 0;
  const Byte* tuple_data = sp.get_tuple(slot_id, &tuple_size);
  if (!tuple_data) {
    return false;
  }

  ByteBuffer before_image(tuple_data, tuple_data + tuple_size);

  bool ok = sp.delete_tuple(slot_id);
  if (ok) {
    guard.mark_dirty();
    PagePatch patch;
    patch.page_id = page_id;
    patch.offset = slot_id;
    patch.before_payload = std::move(before_image);

    LogRecord wal_record{};
    if (build_page_patch_record(txn_id, patch, wal_record)) {
      wal_record.header.prev_lsn = txn_manager_.get_last_lsn(txn_id);
      lsn_t lsn = log_manager_.append_record(wal_record);
      if (lsn != INVALID_LSN) {
        txn_manager_.record_lsn(txn_id, lsn);
		buffer_pool_.update_dirtied_page_lsn(page_id, lsn);
        guard.get_page()->set_page_lsn(lsn);
      }
    }
  }

  return ok;
}

CompactionStats TableManager::compact_table(const TableMetadata& tbl_meta_data) {
  std::lock_guard<std::mutex> table_lock(get_table_latch(tbl_meta_data.table_id));
  CompactionStats stats;
  std::vector<PageStats> pages;
  survey_pages(tbl_meta_data.first_page_id, tbl_meta_data.name, pages);
  if (pages.empty()) {
    return stats;
  }

  // Sort the PageStats in ascending order.
  std::sort(pages.begin(), pages.end(), [](const PageStats& a, const PageStats& b) {
    return a.live_count < b.live_count;
  });

  stats.tuples_migrated = migrate_tuples(pages);

  page_id_t current_page_id = tbl_meta_data.first_page_id;
  while (current_page_id != INVALID_PAGE_ID) {
    PageGuard guard = buffer_pool_.fetch_page_guard(current_page_id);
    if (!guard) {
      break;
    }

    SlottedPage sp = SlottedPage::from_page(guard);
    if (sp.get_num_slots() > 0 && sp.compact()) {
      guard.mark_dirty();
    }

    PageHeader header = load_page_layout<PageHeader>(guard);
    current_page_id = header.next_page_id;
  }

  uint64_t txn_id = txn_manager_.begin();
  stats.pages_freed = static_cast<uint32_t>(free_empty_pages(tbl_meta_data, txn_id));
  if (!txn_manager_.commit(txn_id)) {
    throw DbException(DbErrorCode::Internal, "failed to commit compaction transaction for table '" + tbl_meta_data.name + "'");
  }

  buffer_pool_.force_flush();
  return stats;
}

void TableManager::survey_pages(page_id_t first_page_id, const std::string& table_name, std::vector<PageStats>& pages) {
  page_id_t current_page_id = first_page_id;

  while (current_page_id != INVALID_PAGE_ID) {
    PageGuard guard = buffer_pool_.fetch_page_guard(current_page_id);
    if (!guard) {
      LOG_STORAGE_ERROR("Problem loading data page: {} for table: '{}'", current_page_id, table_name);
      break;
    }

    SlottedPage sp = SlottedPage::from_page(guard);
    uint16_t live_tuple_cnt = 0;
    for (uint16_t s = 0; s < sp.get_num_slots(); ++s) {
      if (auto slot = sp.get_slot(s); slot && slot->length > 0) {
        ++live_tuple_cnt;
      }
    }

	// Only consider pages that have live tuples.
	if (live_tuple_cnt > 0) {
	  pages.push_back({current_page_id, live_tuple_cnt, static_cast<uint32_t>(sp.get_free_space())});
	}

    PageHeader header = load_page_layout<PageHeader>(guard);
    current_page_id = header.next_page_id;
  }

  LOG_STORAGE_DEBUG("Surveyed {} data pages for table '{}'", pages.size(), table_name);
}

int TableManager::migrate_tuples(std::vector<PageStats>& page_stats) {
  int migrated = 0;

  for (auto& pg_stat : page_stats) {

    PageGuard src_guard = buffer_pool_.fetch_page_guard(pg_stat.page_id);
    if (!src_guard) continue;

    SlottedPage src_sp = SlottedPage::from_page(src_guard);

    for (uint16_t s_id = 0; s_id < src_sp.get_num_slots(); ++s_id) {
      auto slot = src_sp.get_slot(s_id);
      if (!slot || slot->length == 0) continue;

      uint32_t tuple_size = slot->length;

      int dst_idx = -1;
      for (int i = static_cast<int>(page_stats.size()) - 1; i >= 0; --i) {
        if (page_stats[i].page_id != pg_stat.page_id && page_stats[i].free_space >= tuple_size) {
          dst_idx = i;
          break;
        }
      }
	  // Did not find any page that can accommodate this tuple.
      if (dst_idx == -1) continue;

      PageGuard dst_guard = buffer_pool_.fetch_page_guard(page_stats[dst_idx].page_id);
      if (!dst_guard) continue;

      SlottedPage dst_sp = SlottedPage::from_page(dst_guard);
      uint32_t tuple_sz = 0;
      const Byte* data = src_sp.get_tuple(s_id, &tuple_sz);

      bool ok = dst_sp.insert_tuple(data, tuple_sz).has_value();
	  // Tuple migrated to destination page.
      if (ok) {
        src_sp.delete_tuple(s_id);
        dst_guard.mark_dirty();
        src_guard.mark_dirty();
        migrated++;
		page_stats[dst_idx].free_space -= tuple_sz + sizeof(Slot);
      } else {
		 // TODO log an error/warning.
	  }
    }
  }
  return migrated;
}

int TableManager::free_empty_pages(const TableMetadata& tbl_meta_data, uint64_t txn_id) {
  int pages_freed = 0;
  page_id_t current_page_id = tbl_meta_data.first_page_id;
  page_id_t new_first_page_id = tbl_meta_data.first_page_id;
  page_id_t new_last_page_id = INVALID_PAGE_ID;

  while (current_page_id != INVALID_PAGE_ID) {
    PageGuard guard = buffer_pool_.fetch_page_guard(current_page_id);
    if (!guard) {
      break;
    }

    PageHeader header = load_page_layout<PageHeader>(guard);
    page_id_t next_page_id = header.next_page_id;
    bool is_empty = SlottedPage::from_page(guard).is_empty();
    guard.drop();

    bool can_free = is_empty && (new_first_page_id != current_page_id || next_page_id != INVALID_PAGE_ID);
    if (can_free) {
      if (page_allocator_.unlink_page(current_page_id, txn_id) &&
          page_allocator_.free_page(current_page_id, txn_id)) {
        if (current_page_id == new_first_page_id) {
          new_first_page_id = next_page_id;
        }
        ++pages_freed;
      } else {
        new_last_page_id = current_page_id;
      }
    } else {
      new_last_page_id = current_page_id;
    }

    current_page_id = next_page_id;
  }

  if (new_first_page_id != tbl_meta_data.first_page_id || new_last_page_id != tbl_meta_data.last_page_id) {
    catalog_manager_.update_table_page_bounds(tbl_meta_data.name, new_first_page_id, new_last_page_id, txn_id);
  }

  return pages_freed;
}

page_id_t TableManager::acquire_page_for_insert(const TableMetadata& meta, uint32_t needed_space, uint64_t txn_id) {
  if (meta.last_page_id != INVALID_PAGE_ID) {
    PageGuard guard = buffer_pool_.fetch_page_guard(meta.last_page_id);
    if (!guard) {
      throw DbException(DbErrorCode::Internal, "Failed to fetch tail page for insert");
    }

    SlottedPage sp = SlottedPage::from_page(guard);
    if (sp.get_free_space() >= needed_space + sizeof(Slot)) {
      return meta.last_page_id;
    }
  }

  page_id_t new_page_id = page_allocator_.allocate_page(static_cast<uint16_t>(meta.table_id), PageType::DATA, txn_id);
  if (new_page_id == INVALID_PAGE_ID) {
    throw DbException(DbErrorCode::NoSpace, "Failed to allocate new data page for table '" + meta.name + "'");
  }

  if (meta.last_page_id != INVALID_PAGE_ID) {
    if (!page_allocator_.link_page(meta.last_page_id, new_page_id, txn_id)) {
      throw DbException(DbErrorCode::Internal, "Failed to link new data page for table '" + meta.name + "'");
    }
    catalog_manager_.update_table_page_bounds(meta.name, meta.first_page_id, new_page_id, txn_id);
  } else {
    catalog_manager_.update_table_page_bounds(meta.name, new_page_id, new_page_id, txn_id);
  }

  return new_page_id;
}

}
