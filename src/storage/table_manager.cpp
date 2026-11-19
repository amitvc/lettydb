#include "table_manager.h"
#include "slotted_page.h"
#include "storage_def.h"
#include "common/logger.h"
#include "common/db_exception.h"
#include "log_record.h"
#include <optional>
#include <cassert>

namespace letty {

TableManager::TableManager(BufferPoolManager& buffer_pool, IamManager& iam_manager, CatalogManager& catalog_manager,
                           LogManager& log_manager, TransactionManager& txn_manager)
  : buffer_pool_(buffer_pool),
    iam_manager_(iam_manager),
    catalog_manager_(catalog_manager),
    log_manager_(log_manager),
    txn_manager_(txn_manager) {}

std::optional<uint16_t> TableManager::insert_row(const std::string& table_name, const Tuple& tuple, uint64_t txn_id) {
  auto meta = catalog_manager_.get_table(table_name);
  if (!meta) {
    throw DbException(DbErrorCode::UndefinedTable, "table '" + table_name + "' does not exist");
  }
  return insert_row(meta.value(), tuple, txn_id);
}

std::optional<uint16_t> TableManager::insert_row(const TableMetadata& meta, const Tuple& tuple, uint64_t txn_id) {
  std::lock_guard<std::mutex> table_lock(get_table_latch(meta.iam_page_id));
  Byte serialized_tuple[PAGE_SIZE];
  uint32_t data_size = tuple.serialize(meta.schema, serialized_tuple, PAGE_SIZE);
  if (data_size == 0) {
    return std::nullopt;
  }

  page_id_t target_page_id = acquire_page_for_insert(meta.iam_page_id, data_size, txn_id);
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
  sp.set_page_lsn(lsn);
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
  return TableScanner(buffer_pool_, meta->get().iam_page_id);
}

bool TableManager::delete_row(const std::string& table_name, page_id_t page_id, uint16_t slot_id, uint64_t txn_id) {
  auto meta = catalog_manager_.get_table(table_name);
  if (!meta.has_value()) {
    throw DbException(DbErrorCode::UndefinedTable, "table '" + table_name + "' does not exist");
  }
  return delete_row(meta->get(), page_id, slot_id, txn_id);
}

bool TableManager::delete_row(const TableMetadata& meta, page_id_t page_id, uint16_t slot_id, uint64_t txn_id) {
  std::lock_guard<std::mutex> table_lock(get_table_latch(meta.iam_page_id));
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
        sp.set_page_lsn(lsn);
      }
    }
  }

  return ok;
}

CompactionStats TableManager::compact_table(const TableMetadata &metadata) {
	return compact_extents(metadata);
}

CompactionStats TableManager::compact_extents(const TableMetadata& meta) {
  CompactionStats stats;
  std::vector<PageStats> pages;
  survey_pages(meta.iam_page_id, meta.name, pages);
  if (pages.empty()) return stats;

  std::sort(pages.begin(), pages.end(), [](const PageStats& a, const PageStats& b) {
    return a.live_count < b.live_count;
  });

  uint32_t pages_before = static_cast<uint32_t>(pages.size());
  stats.tuples_migrated = migrate_tuples(pages);

  // Count empty pages before defrag (these will become freed pages)
  page_id_t defrag_iam = meta.iam_page_id;
  int empty_pages_before = 0;
  while (defrag_iam != INVALID_PAGE_ID) {
    auto iam_page = load_page_value<IAMPage>(buffer_pool_, defrag_iam);
    if (!iam_page) break;
    defrag_iam = iam_page->next_page_id;

    for (uint16_t idx = 0; idx < iam_page->extent_count; ++idx) {
      uint32_t extent_id = iam_page->extent_ids[idx];
      for (uint8_t offset = 0; offset < EXTENT_SIZE; ++offset) {
        page_id_t pid = static_cast<page_id_t>(extent_id * EXTENT_SIZE + offset);
        PageGuard guard = buffer_pool_.fetch_page_guard(pid);
        if (!guard) continue;
        SlottedPage sp = SlottedPage::from_page(guard);
        if (sp.is_empty()) empty_pages_before++;
      }
    }
  }

  // Defragment all pages that still contain tuples
  defrag_iam = meta.iam_page_id;
  while (defrag_iam != INVALID_PAGE_ID) {
    auto iam_page = load_page_value<IAMPage>(buffer_pool_, defrag_iam);
    if (!iam_page) break;
    defrag_iam = iam_page->next_page_id;

    for (uint16_t idx = 0; idx < iam_page->extent_count; ++idx) {
      uint32_t extent_id = iam_page->extent_ids[idx];
      for (uint8_t offset = 0; offset < EXTENT_SIZE; ++offset) {
        page_id_t pid = static_cast<page_id_t>(extent_id * EXTENT_SIZE + offset);
        PageGuard guard = buffer_pool_.fetch_page_guard(pid);
        if (!guard) continue;

        SlottedPage sp = SlottedPage::from_page(guard);
        if (sp.get_num_slots() > 0) {
          sp.compact();
        }
        guard.mark_dirty();
      }
    }
  }

  uint64_t txn_id = txn_manager_.begin();
  stats.extents_freed = free_empty_extents(meta.iam_page_id, txn_id);
  if (!txn_manager_.commit(txn_id)) {
    throw DbException(DbErrorCode::Internal, "failed to commit compaction transaction for table '" + meta.name + "'");
  }

  // Count remaining empty pages after deallocation
  int empty_pages_after = 0;
  page_id_t count_iam = meta.iam_page_id;
  while (count_iam != INVALID_PAGE_ID) {
    auto iam_page = load_page_value<IAMPage>(buffer_pool_, count_iam);
    if (!iam_page) break;
    count_iam = iam_page->next_page_id;

    for (uint16_t idx = 0; idx < iam_page->extent_count; ++idx) {
      uint32_t extent_id = iam_page->extent_ids[idx];
      for (uint8_t offset = 0; offset < EXTENT_SIZE; ++offset) {
        page_id_t pid = static_cast<page_id_t>(extent_id * EXTENT_SIZE + offset);
        PageGuard guard = buffer_pool_.fetch_page_guard(pid);
        if (!guard) continue;
        SlottedPage sp = SlottedPage::from_page(guard);
        if (sp.is_empty()) empty_pages_after++;
      }
    }
  }

  stats.pages_freed = empty_pages_before - empty_pages_after;
  buffer_pool_.force_flush();
  return stats;
}

void TableManager::survey_pages(page_id_t iam_head, const std::string& table_name, std::vector<PageStats>& pages) {
  page_id_t iam_page_id = iam_head;

  while (iam_page_id != INVALID_PAGE_ID) {
    auto iam_page = load_page_value<IAMPage>(buffer_pool_, iam_page_id);
    if (!iam_page) {
      LOG_STORAGE_ERROR("Problem loading IAM page: {} for table: '{}'", iam_page_id, table_name);
      break;
    }

	// Get all extents from the current iam_page
    for (uint16_t idx = 0; idx < iam_page->extent_count; ++idx) {
      uint32_t extent_id = iam_page->extent_ids[idx];
      for (uint8_t offset = 0; offset < EXTENT_SIZE; ++offset) {
        auto data_page_id = static_cast<page_id_t>(extent_id * EXTENT_SIZE + offset);
        PageGuard guard = buffer_pool_.fetch_page_guard(data_page_id);
        if (!guard) continue;

        SlottedPage sp = SlottedPage::from_page(guard);
        uint16_t live = 0;
        for (uint16_t s = 0; s < sp.get_num_slots(); ++s) {
          if (auto slot = sp.get_slot(s); slot && slot->length > 0) ++live;
        }

        pages.push_back({data_page_id, live, static_cast<uint32_t>(sp.get_free_space())});
      }
    }
	// Move the pointer to the next IAM page in the chain.
	iam_page_id = iam_page->next_page_id;
  }

  LOG_STORAGE_DEBUG("Surveyed {} data pages for table '{}'", pages.size(), table_name);
}

int TableManager::migrate_tuples(std::vector<PageStats>& pages) {
  int migrated = 0;

  for (auto& src_info : pages) {
    if (src_info.live_count == 0) continue;

    PageGuard src_guard = buffer_pool_.fetch_page_guard(src_info.id);
    if (!src_guard) continue;

    SlottedPage src_sp = SlottedPage::from_page(src_guard);

    for (uint16_t s_id = 0; s_id < src_sp.get_num_slots(); ++s_id) {
      auto slot = src_sp.get_slot(s_id);
      if (!slot || slot->length == 0) continue;

      uint32_t tuple_size = slot->length;

      int dst_idx = -1;
      for (int i = static_cast<int>(pages.size()) - 1; i >= 0; --i) {
        if (pages[i].id != src_info.id && pages[i].free_space >= tuple_size) {
          dst_idx = i;
          break;
        }
      }
      if (dst_idx == -1) continue;

      PageGuard dst_guard = buffer_pool_.fetch_page_guard(pages[dst_idx].id);
      if (!dst_guard) continue;

      SlottedPage dst_sp = SlottedPage::from_page(dst_guard);
      uint32_t read_size = 0;
      const Byte* data = src_sp.get_tuple(s_id, &read_size);
      if (!data) {
        continue;
      }

      bool ok = dst_sp.insert_tuple(data, read_size).has_value();
      if (ok) {
        src_sp.delete_tuple(s_id);
        dst_guard.mark_dirty();
        src_guard.mark_dirty();
        migrated++;
        pages[dst_idx].free_space -= read_size + sizeof(Slot);
      }
    }
  }
  return migrated;
}

int TableManager::free_empty_extents(page_id_t iam_head, uint64_t txn_id) {
  int extents_freed = 0;
  page_id_t dealloc_iam_page = iam_head;

  while (dealloc_iam_page != INVALID_PAGE_ID) {
    auto iam_page = load_page_value<IAMPage>(buffer_pool_, dealloc_iam_page);
    if (!iam_page) break;

    dealloc_iam_page = iam_page->next_page_id;

    for (uint16_t idx = 0; idx < iam_page->extent_count; ++idx) {
      uint32_t extent_id = iam_page->extent_ids[idx];

      bool all_empty = true;
      for (uint8_t offset = 0; offset < EXTENT_SIZE; ++offset) {
        page_id_t page_id = static_cast<page_id_t>(extent_id * EXTENT_SIZE + offset);
        PageGuard guard = buffer_pool_.fetch_page_guard(page_id);
        if (!guard) {
          all_empty = false;
          continue;
        }

        SlottedPage sp = SlottedPage::from_page(guard);
        if (!sp.is_empty()) all_empty = false;
      }

      if (all_empty) {
        bool removed = iam_manager_.remove_extent_from_iam(iam_head, extent_id, txn_id);
        if (removed) {
          extents_freed++;
          --idx;  // compensate for the shift-left in extent_ids array
        }
      }
    }
  }

  return extents_freed;
}

page_id_t TableManager::acquire_page_for_insert(page_id_t iam_head, uint32_t needed_space, uint64_t txn_id) {
  page_id_t target_page = iam_manager_.find_page_with_space(iam_head, needed_space);

  if (target_page != INVALID_PAGE_ID) {
    PageGuard guard = buffer_pool_.fetch_page_guard(target_page);
    if (!guard) {
      throw DbException(DbErrorCode::Internal, "Failed to fetch page for insert");
    }
    return target_page;
  }

  page_id_t extent_start = iam_manager_.allocate_extent_for_table(iam_head, txn_id);
  if (extent_start == INVALID_PAGE_ID) {
    throw DbException(DbErrorCode::Internal, "Failed to allocate extent for table");
  }

  LOG_STORAGE_DEBUG("Materializing empty SlottedPages for new extent starting at page {} for table IAM page {}",
                   extent_start, iam_head);
  for (uint8_t offset = 0; offset < EXTENT_SIZE; ++offset) {
    page_id_t new_page_id = extent_start + offset;
    PageGuard guard = buffer_pool_.allocate_page_guard(new_page_id);
    assert(guard.is_valid());
    SlottedPage::init(reinterpret_cast<Byte*>(guard.get_data()));
    guard.mark_dirty();
  }

  return extent_start;
}

}
