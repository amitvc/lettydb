#include "table_manager.h"
#include "slotted_page.h"
#include "storage_def.h"
#include "page_utils.h"
#include <iostream>

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
    Page* pg = buffer_pool_.new_page(pid + offset);
    if (!pg) {
      pg = buffer_pool_.fetch_page(pid + offset);
      if (pg && (pg->get_pin_count() != 1 || pg->is_dirty())) {
        buffer_pool_.unpin_page(pid + offset, false);
        pg = nullptr;
      }
    }
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

void TableManager::scan_extent(uint32_t extent_id,
                               const std::function<void(const char*, uint32_t)>& callback) {
  page_id_t extent_start = static_cast<page_id_t>(extent_id * EXTENT_SIZE);
  for (int offset = 0; offset < EXTENT_SIZE; ++offset) {
    page_id_t data_page_id = extent_start + offset;
    Page* data_pg = buffer_pool_.fetch_page(data_page_id);
    if (!data_pg) continue;

    SlottedPage sp(data_pg->get_data());
    for (uint16_t slot = 0; slot < sp.get_num_slots(); ++slot) {
      uint32_t size;
      const char* data = sp.get_tuple(slot, &size);
      if (data) callback(data, size);
    }
    buffer_pool_.unpin_page(data_page_id, false);
  }
}

void TableManager::scan_extent_tuples(uint32_t extent_id, const Schema& schema,
                                      const std::function<void(const Tuple&)>& callback) {
  page_id_t extent_start = static_cast<page_id_t>(extent_id * EXTENT_SIZE);
  for (int offset = 0; offset < EXTENT_SIZE; ++offset) {
    page_id_t data_page_id = extent_start + offset;
    Page* data_pg = buffer_pool_.fetch_page(data_page_id);
    if (!data_pg) continue;

    SlottedPage sp(data_pg->get_data());
    for (uint16_t slot = 0; slot < sp.get_num_slots(); ++slot) {
      uint32_t size;
      const char* data = sp.get_tuple(slot, &size);
      if (data) callback(Tuple::deserialize(schema, data, size));
    }
    buffer_pool_.unpin_page(data_page_id, false);
  }
}

bool TableManager::insert_row(const std::string& table_name, const Tuple& tuple) {
  auto* meta = catalog_manager_.get_table(table_name);
  if (!meta) {
    std::cerr << "Table not found: " << table_name << std::endl;
    return false;
  }
  return insert_row(*meta, tuple);
}

bool TableManager::insert_row(const TableMetadata& meta, const Tuple& tuple) {
  char buf[PAGE_SIZE];
  uint32_t data_size;
  if (!tuple.serialize(meta.schema, buf, PAGE_SIZE, &data_size)) {
    std::cerr << "Tuple too large for page" << std::endl;
    return false;
  }

  page_id_t target_page_id = acquire_page_for_insert(meta.iam_page_id, data_size);
  if (target_page_id == INVALID_PAGE_ID) {
    std::cerr << "Failed to find or allocate a page for table" << std::endl;
    return false;
  }

  Page* page = buffer_pool_.fetch_page(target_page_id);
  if (!page) return false;

  bool ok = try_insert_into_page(page, buf, data_size);
  if (!ok) std::cerr << "Failed to insert tuple into page " << target_page_id << std::endl;
  buffer_pool_.unpin_page(target_page_id, ok);
  return ok;
}

uint32_t TableManager::insert_rows(const std::string& table_name, const std::vector<Tuple>& tuples) {
  auto* meta = catalog_manager_.get_table(table_name);
  if (!meta) {
    std::cerr << "Table not found: " << table_name << std::endl;
    return 0;
  }

  const Schema& schema = meta->schema;
  page_id_t iam_page_id = meta->iam_page_id;

  uint32_t inserted = 0;
  page_id_t current_page_id = INVALID_PAGE_ID;
  Page* current_page = nullptr;

  for (const auto& tuple : tuples) {
    char buf[PAGE_SIZE];
    uint32_t data_size;
    if (!tuple.serialize(schema, buf, PAGE_SIZE, &data_size)) {
      std::cerr << "Tuple too large for page" << std::endl;
      break;
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
      std::cerr << "Failed to find or allocate a page for table" << std::endl;
      break;
    }

    current_page = buffer_pool_.fetch_page(current_page_id);
    if (!current_page) break;

    if (!try_insert_into_page(current_page, buf, data_size)) {
      std::cerr << "Failed to insert tuple into page " << current_page_id << std::endl;
      buffer_pool_.unpin_page(current_page_id, false);
      current_page = nullptr;
      current_page_id = INVALID_PAGE_ID;
      break;
    }
    ++inserted;
  }

  if (current_page) buffer_pool_.unpin_page(current_page_id, true);
  return inserted;
}

bool TableManager::scan_table(const std::string& table_name,
                              const std::function<void(const char* data, uint32_t size)>& callback) {
  auto* meta = catalog_manager_.get_table(table_name);
  if (!meta) {
    std::cerr << "Table not found: " << table_name << std::endl;
    return false;
  }

  page_id_t current_iam_page_id = meta->iam_page_id;
  while (current_iam_page_id != INVALID_PAGE_ID) {
    auto iam_page = load_page_value<IAMPage>(buffer_pool_, current_iam_page_id);
    if (!iam_page) {
      std::cerr << "Failed to fetch IAM page " << current_iam_page_id << std::endl;
      return false;
    }

    for (uint16_t i = 0; i < iam_page->extent_count; ++i) {
      scan_extent(iam_page->extent_ids[i], callback);
    }
    current_iam_page_id = iam_page->next_page_id;
  }

  return true;
}

bool TableManager::scan_table_tuples(const std::string& table_name,
                                     const std::function<void(const Tuple& tuple)>& callback) {
  auto* meta = catalog_manager_.get_table(table_name);
  if (!meta) {
    std::cerr << "Table not found: " << table_name << std::endl;
    return false;
  }

  const Schema& schema = meta->schema;
  page_id_t current_iam_page_id = meta->iam_page_id;

  while (current_iam_page_id != INVALID_PAGE_ID) {
    auto iam_page = load_page_value<IAMPage>(buffer_pool_, current_iam_page_id);
    if (!iam_page) {
      std::cerr << "Failed to fetch IAM page " << current_iam_page_id << std::endl;
      return false;
    }

    for (uint16_t i = 0; i < iam_page->extent_count; ++i) {
      scan_extent_tuples(iam_page->extent_ids[i], schema, callback);
    }
    current_iam_page_id = iam_page->next_page_id;
  }
  return true;
}

}
