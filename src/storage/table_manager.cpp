#include "table_manager.h"
#include "slotted_page.h"
#include "storage_def.h"
#include "common/logger.h"
#include "common/db_exception.h"
#include <optional>
#include <cassert>

namespace letty {

TableManager::TableManager(BufferPoolManager& buffer_pool, IamManager& iam_manager, CatalogManager& catalog_manager)
  : buffer_pool_(buffer_pool),
    iam_manager_(iam_manager),
    catalog_manager_(catalog_manager) {}

bool TableManager::insert_row(const std::string& table_name, const Tuple& tuple) {
  auto* meta = catalog_manager_.get_table(table_name);
  if (!meta) {
    throw DbException(DbErrorCode::UndefinedTable, "table '" + table_name + "' does not exist");
  }
  return insert_row(*meta, tuple);
}

bool TableManager::insert_row(const TableMetadata& meta, const Tuple& tuple) {
  char buf[PAGE_SIZE];
  uint32_t data_size = tuple.serialize(meta.schema, buf, PAGE_SIZE);
  if (data_size == 0) {
    return false;
  }

  page_id_t target_page_id = acquire_page_for_insert(meta.iam_page_id, data_size);
  LOG_STORAGE_DEBUG("TableManager inserting to page: {}", target_page_id);
  Page* page = buffer_pool_.fetch_page(target_page_id);
  assert(page != nullptr);

  SlottedPage sp = SlottedPage::from_page(page);
  bool ok = sp.insert_tuple(buf, data_size).has_value();
  if (!ok) {
    throw DbException(DbErrorCode::Internal, "Failed to insert tuple after ensuring target page has space");
  }
  buffer_pool_.unpin_page(target_page_id, ok);
  return ok;
}

uint32_t TableManager::insert_rows(const std::string& table_name, const std::vector<Tuple>& tuples) {
  auto* meta = catalog_manager_.get_table(table_name);
  if (!meta) {
    throw DbException(DbErrorCode::UndefinedTable, "table '" + table_name + "' does not exist");
  }

  Page* page = nullptr;
  page_id_t target_page_id = INVALID_PAGE_ID;
  uint32_t inserted = 0;

  for (const Tuple& tuple : tuples) {
    char buf[PAGE_SIZE];
    uint32_t data_size = tuple.serialize(meta->schema, buf, PAGE_SIZE);
    if (data_size == 0) {
      LOG_STORAGE_WARN("Tuple of size 0 skipped for table '{}'", table_name);
      continue;
    }

    if (page && try_insert_into_page(page, buf, data_size)) {
      ++inserted;
      continue;
    }

    // Release old page and acquire new
    if (page) {
      buffer_pool_.unpin_page(target_page_id, inserted > 0);
      page = nullptr;
      target_page_id = INVALID_PAGE_ID;
    }

    page_id_t new_page = acquire_page_for_insert(meta->iam_page_id, data_size);
    Page* new_frame = buffer_pool_.fetch_page(new_page);
    if (!new_frame) {
      LOG_STORAGE_ERROR("Failed to acquire page for table '{}'", table_name);
      break;
    }

    if (try_insert_into_page(new_frame, buf, data_size)) {
      page = new_frame;
      target_page_id = new_page;
      ++inserted;
    } else {
      // Page had no room even after allocation
      buffer_pool_.unpin_page(new_page, false);
      break;
    }
  }

  if (page) {
    buffer_pool_.unpin_page(target_page_id, true);
  }

  return inserted;
}

TableScanner TableManager::scan_table(const std::string& table_name) {
  auto* meta = catalog_manager_.get_table(table_name);
  if (!meta) {
    throw DbException(DbErrorCode::UndefinedTable, "table '" + table_name + "' does not exist");
  }
  return TableScanner(buffer_pool_, meta->iam_page_id);
}

bool TableManager::delete_row(const std::string& table_name, page_id_t page_id, uint16_t slot_id) {
  auto* meta = catalog_manager_.get_table(table_name);
  if (!meta) {
    throw DbException(DbErrorCode::UndefinedTable, "table '" + table_name + "' does not exist");
  }
  return delete_row(*meta, page_id, slot_id);
}

bool TableManager::delete_row(const TableMetadata& meta, page_id_t page_id, uint16_t slot_id) {
  Page* page = buffer_pool_.fetch_page(page_id);
  if (!page) {
    LOG_STORAGE_ERROR("Failed to fetch page {} for delete on table '{}'", page_id, meta.name);
    return false;
  }

  SlottedPage sp = SlottedPage::from_page(page);
  bool ok = sp.delete_tuple(slot_id);
  buffer_pool_.unpin_page(page_id, ok);
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
        Page* page = buffer_pool_.fetch_page(pid);
        if (!page) continue;
        SlottedPage sp = SlottedPage::from_page(page);
        if (sp.is_empty()) empty_pages_before++;
        buffer_pool_.unpin_page(pid, false);
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
        Page* page = buffer_pool_.fetch_page(pid);
        if (!page) continue;

        SlottedPage sp = SlottedPage::from_page(page);
        if (sp.get_num_slots() > 0) {
          sp.compact();
        }
        buffer_pool_.unpin_page(pid, true);
      }
    }
  }

  stats.extents_freed = free_empty_extents(meta.iam_page_id);

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
        Page* page = buffer_pool_.fetch_page(pid);
        if (!page) continue;
        SlottedPage sp = SlottedPage::from_page(page);
        if (sp.is_empty()) empty_pages_after++;
        buffer_pool_.unpin_page(pid, false);
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
        Page* data_page = buffer_pool_.fetch_page(data_page_id);
        if (!data_page) continue;

        SlottedPage sp = SlottedPage::from_page(data_page);
        uint16_t live = 0;
        auto* slot_dir = sp.slotted_page_dir();
        for (uint16_t s = 0; s < sp.get_num_slots(); ++s) {
          if (slot_dir[s].length > 0) ++live;
        }
        buffer_pool_.unpin_page(data_page_id, false);

        pages.push_back({data_page_id, live, static_cast<uint32_t>(sp.get_free_space())});
      }
    }
	// Move the pointer to the next IAM page in the chain.
	iam_page_id = iam_page->next_page_id;
  }

  LOG_STORAGE_INFO("Surveyed {} data pages for table '{}'", pages.size(), table_name);
}

int TableManager::migrate_tuples(std::vector<PageStats>& pages) {
  int migrated = 0;

  for (auto& src_info : pages) {
    if (src_info.live_count == 0) continue;

    Page* src = buffer_pool_.fetch_page(src_info.id);
    if (!src) continue;

    SlottedPage src_sp = SlottedPage::from_page(src);
    auto* slot_dir = src_sp.slotted_page_dir();

    for (uint16_t s_id = 0; s_id < src_sp.get_num_slots(); ++s_id) {
      if (slot_dir[s_id].length == 0) continue;

      uint32_t tuple_size = slot_dir[s_id].length;

      int dst_idx = -1;
      for (int i = static_cast<int>(pages.size()) - 1; i >= 0; --i) {
        if (pages[i].id != src_info.id && pages[i].free_space >= tuple_size) {
          dst_idx = i;
          break;
        }
      }
      if (dst_idx == -1) continue;

      Page* dst = buffer_pool_.fetch_page(pages[dst_idx].id);
      if (!dst) continue;

      SlottedPage dst_sp = SlottedPage::from_page(dst);
      uint32_t read_size = 0;
      const char* data = src_sp.get_tuple(s_id, &read_size);
      if (!data) {
        buffer_pool_.unpin_page(pages[dst_idx].id, false);
        continue;
      }

      bool ok = dst_sp.insert_tuple(data, read_size).has_value();
      if (ok) {
        src_sp.delete_tuple(s_id);
        migrated++;
        pages[dst_idx].free_space -= read_size + sizeof(Slot);
      }
      buffer_pool_.unpin_page(pages[dst_idx].id, ok);
    }
    buffer_pool_.unpin_page(src_info.id, true);
  }
  return migrated;
}

int TableManager::free_empty_extents(page_id_t iam_head) {
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
        Page* page = buffer_pool_.fetch_page(page_id);
        if (!page) {
		  all_empty = false;
		  continue;
		}

        SlottedPage sp = SlottedPage::from_page(page);
        if (!sp.is_empty()) all_empty = false;
        buffer_pool_.unpin_page(page_id, false);
      }

      if (all_empty) {
        bool removed = iam_manager_.remove_extent_from_iam(iam_head, extent_id);
        if (removed) {
          extents_freed++;
          --idx;  // compensate for the shift-left in extent_ids array
        }
      }
    }
  }

  return extents_freed;
}

page_id_t TableManager::acquire_page_for_insert(page_id_t iam_head, uint32_t needed_space) {
  page_id_t target_page = iam_manager_.find_page_with_space(iam_head, needed_space);

  if (target_page != INVALID_PAGE_ID) {
    Page* page = buffer_pool_.fetch_page(target_page);
    if (!page) {
      throw DbException(DbErrorCode::Internal, "Failed to fetch page for insert");
    }
    buffer_pool_.unpin_page(target_page, false);
    return target_page;
  }

  page_id_t extent_start = iam_manager_.allocate_extent_for_table(iam_head);
  if (extent_start == INVALID_PAGE_ID) {
    throw DbException(DbErrorCode::Internal, "Failed to allocate extent for table");
  }

  LOG_STORAGE_INFO("Materializing empty SlottedPages for new extent starting at page {} for table IAM page {}",
                   extent_start, iam_head);
  for (uint8_t offset = 0; offset < EXTENT_SIZE; ++offset) {
    page_id_t new_page_id = extent_start + offset;
    Page* page = buffer_pool_.new_or_fetch_page(new_page_id);
    assert(page != nullptr);
    SlottedPage::init(page->get_data());
    buffer_pool_.unpin_page(new_page_id, true);
  }

  return extent_start;
}

bool TableManager::try_insert_into_page(Page* page, const char* buf, uint32_t data_size) {
  SlottedPage sp = SlottedPage::from_page(page);
  return sp.insert_tuple(buf, data_size).has_value();
}

}
