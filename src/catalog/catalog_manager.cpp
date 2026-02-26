#include "catalog/catalog_manager.h"
#include "storage/slotted_page.h"
#include "storage/tuple.h"
#include "catalog/catalog_defs.h"
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace letty {

CatalogManager::CatalogManager(BufferPoolManager& buffer_pool, IamManager& iam_manager)
    : buffer_pool_(buffer_pool), iam_manager_(iam_manager) {}

void CatalogManager::init() {
  Page* header_page = buffer_pool_.fetch_page(HEADER_PAGE_ID);
  if (!header_page) return;

  auto* header = reinterpret_cast<const DatabaseHeader*>(header_page->get_data());
  bool needs_bootstrap = (header->sys_tables_iam_page == INVALID_PAGE_ID);
  buffer_pool_.unpin_page(HEADER_PAGE_ID, false);

  if (needs_bootstrap) {
    bootstrap();
  }
}

void CatalogManager::bootstrap() {
    Page* header_page = buffer_pool_.fetch_page(HEADER_PAGE_ID);
    if (!header_page) {
        throw std::runtime_error("Failed to fetch header page during bootstrap");
    }
    auto* header = reinterpret_cast<DatabaseHeader*>(header_page->get_data());

    // Initialize metadata pool at the designated page
    iam_manager_.init_metadata_pool(header->metadata_pool_page_id);

    page_id_t sys_tables_iam = iam_manager_.create_iam_chain();
    page_id_t sys_columns_iam = iam_manager_.create_iam_chain();

    // Update header in-place
    header->sys_tables_iam_page = sys_tables_iam;
    header->sys_columns_iam_page = sys_columns_iam;
    buffer_pool_.unpin_page(HEADER_PAGE_ID, true);

    page_id_t sys_tables_first_page = iam_manager_.allocate_extent_for_table(sys_tables_iam);
    page_id_t sys_columns_first_page = iam_manager_.allocate_extent_for_table(sys_columns_iam);

    // Initialize these new data pages as SlottedPages via BPM
    Page* st_page = buffer_pool_.fetch_page(sys_tables_first_page);
    if (!st_page) {
        throw std::runtime_error("Failed to fetch sys_tables first page during bootstrap");
    }
    SlottedPage::init(st_page->get_data());
    buffer_pool_.unpin_page(sys_tables_first_page, true);

    Page* sc_page = buffer_pool_.fetch_page(sys_columns_first_page);
    if (!sc_page) {
        throw std::runtime_error("Failed to fetch sys_columns first page during bootstrap");
    }
    SlottedPage::init(sc_page->get_data());
    buffer_pool_.unpin_page(sys_columns_first_page, true);

    // Insert the metadata for sys_tables AND sys_columns INTO sys_tables
    Schema st_schema = sys_tables_schema();
    char buf[PAGE_SIZE];
    uint32_t data_size;

    // sys_tables entry for itself
    Tuple st_tuple({static_cast<int32_t>(SYS_TABLES_TABLE_OID),
                    std::string("sys_tables"),
                    static_cast<int32_t>(sys_tables_iam),
                    static_cast<int32_t>(4)});
  st_tuple.serialize(st_schema, buf, PAGE_SIZE, &data_size);
    insert_into_table(sys_tables_iam, buf, data_size);

    // sys_tables entry for sys_columns
    Tuple sc_tuple({static_cast<int32_t>(SYS_COLUMNS_TABLE_OID),
                    std::string("sys_columns"),
                    static_cast<int32_t>(sys_columns_iam),
                    static_cast<int32_t>(5)});
  sc_tuple.serialize(st_schema, buf, PAGE_SIZE, &data_size);
    insert_into_table(sys_tables_iam, buf, data_size);

    // Insert column definitions into sys_columns
    Schema sc_schema = sys_columns_schema();

    // Helper lambda to serialize and insert a sys_columns row
    auto insert_col = [&](int32_t table_oid, const std::string& col_name,
                          int32_t type, int32_t length, int32_t offset) {
        Tuple col_tuple({table_oid, col_name, type, length, offset});
	  col_tuple.serialize(sc_schema, buf, PAGE_SIZE, &data_size);
        insert_into_table(sys_columns_iam, buf, data_size);
    };

    // Columns for sys_tables (offsets are sequential for Tuple format)
    insert_col(SYS_TABLES_TABLE_OID, "oid",           static_cast<int32_t>(DataType::INTEGER), 4, 0);
    insert_col(SYS_TABLES_TABLE_OID, "name",          static_cast<int32_t>(DataType::VARCHAR), MAX_NAME_LENGTH, 4);
    insert_col(SYS_TABLES_TABLE_OID, "iam_page_id", static_cast<int32_t>(DataType::INTEGER), 4, 36);
    insert_col(SYS_TABLES_TABLE_OID, "column_count",  static_cast<int32_t>(DataType::INTEGER), 4, 40);

    // Columns for sys_columns
    insert_col(SYS_COLUMNS_TABLE_OID, "table_oid", static_cast<int32_t>(DataType::INTEGER), 4, 0);
    insert_col(SYS_COLUMNS_TABLE_OID, "name",      static_cast<int32_t>(DataType::VARCHAR), MAX_NAME_LENGTH, 4);
    insert_col(SYS_COLUMNS_TABLE_OID, "type",      static_cast<int32_t>(DataType::INTEGER), 4, 36);
    insert_col(SYS_COLUMNS_TABLE_OID, "length",    static_cast<int32_t>(DataType::INTEGER), 4, 40);
    insert_col(SYS_COLUMNS_TABLE_OID, "offset",    static_cast<int32_t>(DataType::INTEGER), 4, 44);
}

uint16_t CatalogManager::get_next_oid() {
    Page* header_page = buffer_pool_.fetch_page(HEADER_PAGE_ID);
    if (!header_page) {
        throw std::runtime_error("Failed to fetch header page for OID allocation");
    }
    auto* header = reinterpret_cast<DatabaseHeader*>(header_page->get_data());

    uint16_t oid = header->next_table_oid;
    header->next_table_oid++;

    buffer_pool_.unpin_page(HEADER_PAGE_ID, true);
    return oid;
}

bool CatalogManager::insert_into_table(page_id_t iam_page_id, const char* data, uint32_t size) {
    page_id_t target_page_id = iam_manager_.find_page_with_space(iam_page_id, size);

    // No existing page has room — grow the table
    if (target_page_id == INVALID_PAGE_ID) {
        target_page_id = iam_manager_.allocate_extent_for_table(iam_page_id);
        if (target_page_id == INVALID_PAGE_ID) {
            std::cerr << "Failed to allocate new extent for table" << std::endl;
            return false;
        }

        // Initialize the new page as a SlottedPage
        Page* new_pg = buffer_pool_.fetch_page(target_page_id);
        if (!new_pg) return false;
        SlottedPage::init(new_pg->get_data());
        buffer_pool_.unpin_page(target_page_id, true);
    }

    Page* page = buffer_pool_.fetch_page(target_page_id);
    if (!page) return false;

    SlottedPage sp(page->get_data());
    auto slot_id = sp.insert_tuple(data, size);
    if (!slot_id) {
        std::cerr << "Failed to insert tuple into page " << target_page_id << std::endl;
        buffer_pool_.unpin_page(target_page_id, false);
        return false;
    }

    buffer_pool_.unpin_page(target_page_id, true);
    return true;
}

std::pair<page_id_t, page_id_t> CatalogManager::get_system_iam_pages() {
    Page* header_page = buffer_pool_.fetch_page(HEADER_PAGE_ID);
    if (!header_page) {
        return {INVALID_PAGE_ID, INVALID_PAGE_ID};
    }

    auto* db_header = reinterpret_cast<const DatabaseHeader*>(header_page->get_data());
    page_id_t sys_tables_iam = db_header->sys_tables_iam_page;
    page_id_t sys_columns_iam = db_header->sys_columns_iam_page;
    buffer_pool_.unpin_page(HEADER_PAGE_ID, false);

    return {sys_tables_iam, sys_columns_iam};
}

std::vector<Tuple> CatalogManager::scan_system_table(page_id_t iam_head, const Schema& schema) {
    std::vector<Tuple> results;

    page_id_t current_iam = iam_head;
    while (current_iam != INVALID_PAGE_ID) {
        Page* iam_pg = buffer_pool_.fetch_page(current_iam);
        if (!iam_pg) break;

        auto* iam_page = reinterpret_cast<const IAMPage*>(iam_pg->get_data());

        for (uint16_t i = 0; i < iam_page->extent_count; ++i) {
            uint32_t extent_id = iam_page->extent_ids[i];
            page_id_t extent_start = static_cast<page_id_t>(extent_id * EXTENT_SIZE);

            for (int offset = 0; offset < EXTENT_SIZE; ++offset) {
                page_id_t data_page_id = extent_start + offset;

                Page* data_pg = buffer_pool_.fetch_page(data_page_id);
                if (!data_pg) continue;

                SlottedPage sp(data_pg->get_data());
                for (uint16_t slot = 0; slot < sp.get_num_slots(); ++slot) {
                    uint32_t sz;
                    const char* data = sp.get_tuple(slot, &sz);
                    if (!data) continue;
                    results.push_back(Tuple::deserialize(schema, data, sz));
                }
                buffer_pool_.unpin_page(data_page_id, false);
            }
        }

        page_id_t next_iam = iam_page->next_page_id;
        buffer_pool_.unpin_page(current_iam, false);
        current_iam = next_iam;
    }

    return results;
}

const TableMetadata* CatalogManager::get_table(const std::string& name) {
    auto cache_it = table_cache_.find(name);
    if (cache_it != table_cache_.end()) {
        return &cache_it->second;
    }

    auto [sys_tables_iam, sys_columns_iam] = get_system_iam_pages();
    if (sys_tables_iam == INVALID_PAGE_ID) {
        return nullptr;
    }

    // Find matching table in sys_tables
    Schema st_schema = sys_tables_schema();
    auto table_rows = scan_system_table(sys_tables_iam, st_schema);

    uint32_t found_oid = 0;
    page_id_t found_iam_page_id = INVALID_PAGE_ID;
    bool found = false;

    for (const auto& row : table_rows) {
        std::string table_name = std::get<std::string>(row.get_value(1));
        if (name == table_name) {
            found_oid = static_cast<uint32_t>(std::get<int32_t>(row.get_value(0)));
            found_iam_page_id = static_cast<page_id_t>(std::get<int32_t>(row.get_value(2)));
            found = true;
            break;
        }
    }

    if (!found) return nullptr;

    // Collect columns for this table from sys_columns
    Schema sc_schema = sys_columns_schema();
    auto col_rows = scan_system_table(sys_columns_iam, sc_schema);

    std::vector<Column> column_list;
    for (const auto& col_row : col_rows) {
        auto col_table_oid = static_cast<uint32_t>(std::get<int32_t>(col_row.get_value(0)));
        if (col_table_oid == found_oid) {
            std::string col_name = std::get<std::string>(col_row.get_value(1));
            auto col_type = static_cast<DataType>(std::get<int32_t>(col_row.get_value(2)));
            auto col_length = static_cast<uint16_t>(std::get<int32_t>(col_row.get_value(3)));
            auto col_offset = static_cast<uint16_t>(std::get<int32_t>(col_row.get_value(4)));
            column_list.emplace_back(col_name, col_type, col_length, col_offset);
        }
    }

    // Build metadata, cache, and return
    TableMetadata metadata;
    metadata.oid = found_oid;
    metadata.name = name;
    metadata.iam_page_id = found_iam_page_id;
    metadata.schema = Schema(column_list);

    auto [it, _] = table_cache_.emplace(name, std::move(metadata));
    return &it->second;
}

bool CatalogManager::create_table(const std::string& name, const Schema& schema) {
    if (get_table(name) != nullptr) {
        return false;
    }

    uint16_t next_oid = get_next_oid();

    page_id_t new_iam = iam_manager_.create_iam_chain();
    if (new_iam == INVALID_PAGE_ID) {
        return false;
    }

    auto [sys_tables_iam, sys_columns_iam] = get_system_iam_pages();
    if (sys_tables_iam == INVALID_PAGE_ID) return false;

    // Insert into sys_tables
    char buf[PAGE_SIZE];
    uint32_t data_size;
    Schema st_schema = sys_tables_schema();

    Tuple table_tuple({static_cast<int32_t>(next_oid),
                       name,
                       static_cast<int32_t>(new_iam),
                       static_cast<int32_t>(schema.get_columns().size())});
  table_tuple.serialize(st_schema, buf, PAGE_SIZE, &data_size);

    if (!insert_into_table(sys_tables_iam, buf, data_size)) {
        std::cerr << "Failed to insert table record into sys_tables" << std::endl;
        return false;
    }

    // Insert columns into sys_columns
    Schema sc_schema = sys_columns_schema();
    const auto& columns = schema.get_columns();
    for (const auto& col : columns) {
        Tuple col_tuple({static_cast<int32_t>(next_oid),
                         col.get_name(),
                         static_cast<int32_t>(col.get_type()),
                         static_cast<int32_t>(col.get_length()),
                         static_cast<int32_t>(col.get_offset())});
	  col_tuple.serialize(sc_schema, buf, PAGE_SIZE, &data_size);

        if (!insert_into_table(sys_columns_iam, buf, data_size)) {
            std::cerr << "Failed to insert column record into sys_columns" << std::endl;
            return false;
        }
    }

    // Cache so subsequent lookups skip the disk scan
    TableMetadata cached_meta;
    cached_meta.oid = next_oid;
    cached_meta.name = name;
    cached_meta.schema = schema;
    cached_meta.iam_page_id = new_iam;
    table_cache_[name] = std::move(cached_meta);

    return true;
}

Schema CatalogManager::sys_tables_schema() {
    // {oid INT, name VARCHAR(32), iam_page_id INT, column_count INT}
    // All INTEGER columns use length=4 to match Tuple::serialize_into format
    return Schema({
        Column("oid",           DataType::INTEGER, 4, 0),
        Column("name",          DataType::VARCHAR, MAX_NAME_LENGTH, 4),
        Column("iam_page_id", DataType::INTEGER, 4, 36),
        Column("column_count",  DataType::INTEGER, 4, 40)
    });
}

Schema CatalogManager::sys_columns_schema() {
    // {table_oid INT, name VARCHAR(32), type INT, length INT, offset INT}
    return Schema({
        Column("table_oid", DataType::INTEGER, 4, 0),
        Column("name",      DataType::VARCHAR, MAX_NAME_LENGTH, 4),
        Column("type",      DataType::INTEGER, 4, 36),
        Column("length",    DataType::INTEGER, 4, 40),
        Column("offset",    DataType::INTEGER, 4, 44)
    });
}

}
