#include "table_manager.h"
#include "slotted_page.h"
#include "storage_def.h"
#include "common/logger.h"
#include "common/db_exception.h"

namespace letty {

TableManager::TableManager(BufferPoolManager& buffer_pool, IamManager& iam_manager, CatalogManager& catalog_manager)
    : buffer_pool_(buffer_pool), iam_manager_(iam_manager), catalog_manager_(catalog_manager) {}

page_id_t TableManager::acquire_page_for_insert(page_id_t iam_head, uint32_t needed_space) {
  page_id_t pid = iam_manager_.find_page_with_space(iam_head, needed_space);
  if (pid != INVALID_PAGE_ID) return pid;

  // No existing page has room — grow the table by one extent
  pid = iam_manager_.allocate_extent_for_table(iam_head);
  if (pid == INVALID_PAGE_ID) return INVALID_PAGE_ID;

  // Materialize all 8 pages in the new extent as empty SlottedPages.
  // Without this, uninitialized pages have free_space_pointer = 0 and appear
  // full to page_has_space, so only the first page of each extent would ever be used.
  for (int offset = 0; offset < EXTENT_SIZE; ++offset) {
    Page* pg = buffer_pool_.new_or_fetch_page(pid + offset);
    if (!pg) return INVALID_PAGE_ID;
    SlottedPage::init(pg->get_data());
    buffer_pool_.unpin_page(pid + offset, true);
  }
  return pid;
}

bool TableManager::try_insert_into_page(Page* page, const char* buf, uint32_t data_size) {
  SlottedPage sp(page->get_data());
  return sp.insert_tuple(buf, data_size).has_value();
}

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

  page_id_t target_page_id = acquire_page_for_insert(meta.iam_page_id, data_size);
  if (target_page_id == INVALID_PAGE_ID) {
    throw DbException(DbErrorCode::NoSpace, "failed to find or allocate a page for table");
  }

  Page* page = buffer_pool_.fetch_page(target_page_id);
  if (!page) {
    throw DbException(DbErrorCode::IOError, "failed to fetch target page for insert");
  }

  bool ok = try_insert_into_page(page, buf, data_size);
  buffer_pool_.unpin_page(target_page_id, ok);
  if (!ok) {
    throw DbException(DbErrorCode::NoSpace, "failed to insert tuple into page " + std::to_string(target_page_id));
  }
  return true;
}

uint32_t TableManager::insert_rows(const std::string& table_name, const std::vector<Tuple>& tuples) {
  auto* meta = catalog_manager_.get_table(table_name);
  if (!meta) {
    throw DbException(DbErrorCode::UndefinedTable, "table '" + table_name + "' does not exist");
  }

  const Schema& schema = meta->schema;
  page_id_t iam_page_id = meta->iam_page_id;

  uint32_t inserted = 0;
  page_id_t current_page_id = INVALID_PAGE_ID;
  Page* current_page = nullptr;

  for (const auto& tuple : tuples) {
    char buf[PAGE_SIZE];
    uint32_t data_size;
    try {
      data_size = tuple.serialize(schema, buf, PAGE_SIZE);
    } catch (...) {
      if (current_page) buffer_pool_.unpin_page(current_page_id, true);
      throw;
    }

    if (current_page && try_insert_into_page(current_page, buf, data_size)) {
      ++inserted;
      continue;
    }

    // Current page is full or not yet set — release it and find a new one
    if (current_page) {
      buffer_pool_.unpin_page(current_page_id, true);
      current_page = nullptr;
      current_page_id = INVALID_PAGE_ID;
    }

    current_page_id = acquire_page_for_insert(iam_page_id, data_size);
    if (current_page_id == INVALID_PAGE_ID) {
      throw DbException(DbErrorCode::NoSpace, "failed to find or allocate a page for table");
    }

    current_page = buffer_pool_.fetch_page(current_page_id);
    if (!current_page) {
      throw DbException(DbErrorCode::IOError, "failed to fetch target page for insert");
    }

    if (!try_insert_into_page(current_page, buf, data_size)) {
      page_id_t failed_page_id = current_page_id;
      buffer_pool_.unpin_page(current_page_id, false);
      current_page = nullptr;
      current_page_id = INVALID_PAGE_ID;
      throw DbException(DbErrorCode::NoSpace, "failed to insert tuple into page " + std::to_string(failed_page_id));
    }
    ++inserted;
  }

  if (current_page) buffer_pool_.unpin_page(current_page_id, true);
  return inserted;
}

TableScanner TableManager::scan_table(const std::string& table_name) {
  auto* table_metadata = catalog_manager_.get_table(table_name);
  if (!table_metadata) {
    throw DbException(DbErrorCode::UndefinedTable, "table '" + table_name + "' does not exist");
  }

  return {buffer_pool_, table_metadata->iam_page_id};
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

  SlottedPage sp(page->get_data());
  bool ok = sp.delete_tuple(slot_id);
  buffer_pool_.unpin_page(page_id, ok);
  return ok;
}

void TableManager::compact_table(const std::string& table_name) {
  auto* meta = catalog_manager_.get_table(table_name);
  if (!meta) {
    throw DbException(DbErrorCode::UndefinedTable, "table '" + table_name + "' does not exist");
  }
  compact_table(*meta);
}

void TableManager::compact_table(const TableMetadata& meta) {
  page_id_t iam_page_id = meta.iam_page_id;

  while (iam_page_id != INVALID_PAGE_ID) {
    auto iam_page = load_page_value<IAMPage>(buffer_pool_, iam_page_id);
    if (!iam_page) return;

    iam_page_id = iam_page->next_page_id;

    for (uint16_t idx = 0; idx < iam_page->extent_count; ++idx) {
      uint32_t extent_id = iam_page->extent_ids[idx];
      for (uint8_t offset = 0; offset < EXTENT_SIZE; ++offset) {
        page_id_t data_page_id = static_cast<page_id_t>(extent_id * EXTENT_SIZE + offset);
        Page* page = buffer_pool_.fetch_page(data_page_id);
        if (!page) continue;

        SlottedPage sp(page->get_data());
        if (sp.get_num_slots() > 0) {
          sp.compact();
        }
        buffer_pool_.unpin_page(data_page_id, true);
      }
    }
  }
}

}
