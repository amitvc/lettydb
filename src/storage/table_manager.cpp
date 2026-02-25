//
// Created by Antigravity on 1/27/26.
//

#include "table_manager.h"
#include "slotted_page.h"
#include "storage_def.h"
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

  // Format the first page of the new extent as an empty SlottedPage
  Page* pg = buffer_pool_.fetch_page(pid);
  if (!pg) return INVALID_PAGE_ID;
  SlottedPage::init(pg->get_data());
  buffer_pool_.unpin_page(pid, true);

  return pid;
}

bool TableManager::insert_row(const std::string& table_name, const Tuple& tuple) {
  auto* table_meta = catalog_manager_.get_table(table_name);
  if (!table_meta) {
    std::cerr << "Table not found: " << table_name << std::endl;
    return false;
  }
  return insert_row(*table_meta, tuple);
}

bool TableManager::insert_row(const TableMetadata& meta, const Tuple& tuple) {
  // 1. Serialize the tuple into a stack buffer (no heap allocation)
  char buf[PAGE_SIZE];
  uint32_t data_size;
  if (!tuple.serialize(meta.schema, buf, PAGE_SIZE, &data_size)) {
    std::cerr << "Tuple too large for page" << std::endl;
    return false;
  }

  page_id_t iam_page_id = meta.first_page_id;

  // 2. Get a page with room, allocating a new extent if needed
  page_id_t target_page_id = acquire_page_for_insert(iam_page_id, data_size);
  if (target_page_id == INVALID_PAGE_ID) {
    std::cerr << "Failed to find or allocate a page for table" << std::endl;
    return false;
  }

  // 3. Fetch the target page, insert the tuple, unpin dirty
  Page* page = buffer_pool_.fetch_page(target_page_id);
  if (!page) return false;

  SlottedPage sp(page->get_data());
  auto slot_id = sp.insert_tuple(buf, data_size);
  if (!slot_id) {
    std::cerr << "Failed to insert tuple into page " << target_page_id << std::endl;
    buffer_pool_.unpin_page(target_page_id, false);
    return false;
  }

  buffer_pool_.unpin_page(target_page_id, true);
  return true;
}

uint32_t TableManager::insert_rows(const std::string& table_name, const std::vector<Tuple>& tuples) {
  // Single catalog lookup
  auto* table_meta = catalog_manager_.get_table(table_name);
  if (!table_meta) {
    std::cerr << "Table not found: " << table_name << std::endl;
    return 0;
  }

  const Schema& schema = table_meta->schema;
  page_id_t iam_page_id = table_meta->first_page_id;

  uint32_t inserted = 0;
  page_id_t current_page_id = INVALID_PAGE_ID;
  Page* current_page = nullptr;

  for (const auto& tuple : tuples) {
    // Serialize tuple
    char buf[PAGE_SIZE];
    uint32_t data_size;
    if (!tuple.serialize(schema, buf, PAGE_SIZE, &data_size)) {
      std::cerr << "Tuple too large for page" << std::endl;
      break;
    }

    // Try inserting into the currently pinned page
    if (current_page) {
      SlottedPage sp(current_page->get_data());
      if (sp.get_free_space() >= data_size + sizeof(Slot)) {
        auto slot_id = sp.insert_tuple(buf, data_size);
        if (slot_id) {
          ++inserted;
          continue;
        }
      }
      // Current page is full — unpin it and find a new one
      buffer_pool_.unpin_page(current_page_id, true);
      current_page = nullptr;
      current_page_id = INVALID_PAGE_ID;
    }

    // Get a page with room, allocating a new extent if needed
    page_id_t target_page_id = acquire_page_for_insert(iam_page_id, data_size);
    if (target_page_id == INVALID_PAGE_ID) {
      std::cerr << "Failed to find or allocate a page for table" << std::endl;
      break;
    }

    // Fetch and pin the target page — keep it pinned for subsequent tuples
    current_page = buffer_pool_.fetch_page(target_page_id);
    if (!current_page) break;
    current_page_id = target_page_id;

    SlottedPage sp(current_page->get_data());
    auto slot_id = sp.insert_tuple(buf, data_size);
    if (!slot_id) {
      buffer_pool_.unpin_page(current_page_id, false);
      current_page = nullptr;
      current_page_id = INVALID_PAGE_ID;
      std::cerr << "Failed to insert tuple into page " << target_page_id << std::endl;
      break;
    }
    ++inserted;
  }

  // Unpin the last page if still pinned
  if (current_page) {
    buffer_pool_.unpin_page(current_page_id, true);
  }

  return inserted;
}

bool TableManager::scan_table(const std::string& table_name,
                              const std::function<void(const char* data, uint32_t size)>& callback) {
  // 1. Get table metadata
  auto* table_meta = catalog_manager_.get_table(table_name);
  if (!table_meta) {
    std::cerr << "Table not found: " << table_name << std::endl;
    return false;
  }

  page_id_t current_iam_page_id = table_meta->first_page_id;

  // 2. Traverse the IAM chain
  while (current_iam_page_id != INVALID_PAGE_ID) {
    Page* iam_pg = buffer_pool_.fetch_page(current_iam_page_id);
    if (!iam_pg) {
      std::cerr << "Failed to fetch IAM page " << current_iam_page_id << std::endl;
      return false;
    }

    auto* iam_page = reinterpret_cast<const IAMPage*>(iam_pg->get_data());

    // 3. Iterate through extent IDs in this IAM page
    for (uint16_t i = 0; i < iam_page->extent_count; ++i) {
      uint32_t extent_id = iam_page->extent_ids[i];
      page_id_t extent_start_page = static_cast<page_id_t>(extent_id * EXTENT_SIZE);

      // 4. Check each page in this extent
      for (int page_offset = 0; page_offset < EXTENT_SIZE; ++page_offset) {
        page_id_t data_page_id = extent_start_page + page_offset;

        Page* data_pg = buffer_pool_.fetch_page(data_page_id);
        if (!data_pg) continue;

        // 5. Scan tuples in the SlottedPage
        SlottedPage sp(data_pg->get_data());
        for (uint16_t slot = 0; slot < sp.get_num_slots(); ++slot) {
          uint32_t size;
          const char* data = sp.get_tuple(slot, &size);
          if (data) {
            callback(data, size);
          }
        }
        buffer_pool_.unpin_page(data_page_id, false);
      }
    }

    // Move to next IAM page in chain
    page_id_t next_iam = iam_page->next_page_id;
    buffer_pool_.unpin_page(current_iam_page_id, false);
    current_iam_page_id = next_iam;
  }

  return true;
}

bool TableManager::scan_table_tuples(const std::string& table_name,
                                     const std::function<void(const Tuple& tuple)>& callback) {
  // Get table metadata for schema
  auto* table_meta = catalog_manager_.get_table(table_name);
  if (!table_meta) {
    std::cerr << "Table not found: " << table_name << std::endl;
    return false;
  }

  const Schema& schema = table_meta->schema;

  // Use scan_table with a wrapper callback that deserializes
  return scan_table(table_name, [&schema, &callback](const char* data, uint32_t size) {
    Tuple tuple = Tuple::deserialize(schema, data, size);
    callback(tuple);
  });
}

}
