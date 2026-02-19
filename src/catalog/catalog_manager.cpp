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
    // 1. Fetch header page to read metadata_pool_page_id
    Page* header_page = buffer_pool_.fetch_page(HEADER_PAGE_ID);
    if (!header_page) {
        throw std::runtime_error("Failed to fetch header page during bootstrap");
    }
    auto* header = reinterpret_cast<DatabaseHeader*>(header_page->get_data());

    // Initialize metadata pool at the designated page
    iam_manager_.init_metadata_pool(header->metadata_pool_page_id);

    // 2. Create IAM chains for sys_tables and sys_columns
    page_id_t sys_tables_iam = iam_manager_.create_iam_chain();
    page_id_t sys_columns_iam = iam_manager_.create_iam_chain();

    // Update header in-place
    header->sys_tables_iam_page = sys_tables_iam;
    header->sys_columns_iam_page = sys_columns_iam;
    buffer_pool_.unpin_page(HEADER_PAGE_ID, true);

    // 3. Allocate first extent for sys_tables and sys_columns
    page_id_t sys_tables_first_page = iam_manager_.allocate_extent_for_table(sys_tables_iam);
    page_id_t sys_columns_first_page = iam_manager_.allocate_extent_for_table(sys_columns_iam);

    // Initialize these new data pages as SlottedPages via BPM
    Page* st_page = buffer_pool_.fetch_page(sys_tables_first_page);
    if (!st_page) {
        throw std::runtime_error("Failed to fetch sys_tables first page during bootstrap");
    }
    SlottedPage sys_tables_sp(st_page->get_data(), true);
    buffer_pool_.unpin_page(sys_tables_first_page, true);

    Page* sc_page = buffer_pool_.fetch_page(sys_columns_first_page);
    if (!sc_page) {
        throw std::runtime_error("Failed to fetch sys_columns first page during bootstrap");
    }
    SlottedPage sys_columns_sp(sc_page->get_data(), true);
    buffer_pool_.unpin_page(sys_columns_first_page, true);

    // 4. Insert the metadata for sys_tables AND sys_columns INTO sys_tables
    //    using Tuple serialization (same format as user tables)
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

    // 5. Insert column definitions into sys_columns table
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
    insert_col(SYS_TABLES_TABLE_OID, "first_page_id", static_cast<int32_t>(DataType::INTEGER), 4, 36);
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
    // 1. Try to find an existing page with space
    page_id_t target_page_id = iam_manager_.find_page_with_space(iam_page_id, size);

    // 2. If no page has space, allocate a new extent
    if (target_page_id == INVALID_PAGE_ID) {
        target_page_id = iam_manager_.allocate_extent_for_table(iam_page_id);
        if (target_page_id == INVALID_PAGE_ID) {
            std::cerr << "Failed to allocate new extent for table" << std::endl;
            return false;
        }

        // Initialize the new page as a SlottedPage
        Page* new_pg = buffer_pool_.fetch_page(target_page_id);
        if (!new_pg) return false;
        SlottedPage new_sp(new_pg->get_data(), true);
        buffer_pool_.unpin_page(target_page_id, true);
    }

    // 3. Fetch the target page, insert the tuple, unpin dirty
    Page* page = buffer_pool_.fetch_page(target_page_id);
    if (!page) return false;

    SlottedPage sp(page->get_data());
    int32_t slot_id = sp.insert_tuple(data, size);
    if (slot_id < 0) {
        std::cerr << "Failed to insert tuple into page " << target_page_id << std::endl;
        buffer_pool_.unpin_page(target_page_id, false);
        return false;
    }

    buffer_pool_.unpin_page(target_page_id, true);
    return true;
}

const TableMetadata* CatalogManager::get_table(const std::string& name) {
    // Fast path: check in-memory cache first
    auto cache_it = table_cache_.find(name);
    if (cache_it != table_cache_.end()) {
        return &cache_it->second;
    }

    Page* header_page = buffer_pool_.fetch_page(HEADER_PAGE_ID);
    if (!header_page) return nullptr;

    auto* db_header = reinterpret_cast<const DatabaseHeader*>(header_page->get_data());

    if (db_header->sys_tables_iam_page == INVALID_PAGE_ID) {
        buffer_pool_.unpin_page(HEADER_PAGE_ID, false);
        return nullptr;
    }

    page_id_t sys_tables_iam = db_header->sys_tables_iam_page;
    page_id_t sys_columns_iam = db_header->sys_columns_iam_page;
    buffer_pool_.unpin_page(HEADER_PAGE_ID, false);

    // 2. Scan sys_tables to find the table by name (using Tuple deserialization)
    Schema st_schema = sys_tables_schema();
    uint32_t found_oid = 0;
    page_id_t found_first_page_id = INVALID_PAGE_ID;
    bool found = false;

    page_id_t current_iam = sys_tables_iam;
    while (current_iam != INVALID_PAGE_ID && !found) {
        Page* iam_pg = buffer_pool_.fetch_page(current_iam);
        if (!iam_pg) break;

        auto* iam_page = reinterpret_cast<const IAMPage*>(iam_pg->get_data());

        for (uint16_t i = 0; i < iam_page->extent_count && !found; ++i) {
            uint32_t extent_id = iam_page->extent_ids[i];
            page_id_t extent_start = static_cast<page_id_t>(extent_id * EXTENT_SIZE);

            for (int offset = 0; offset < EXTENT_SIZE && !found; ++offset) {
                page_id_t data_page_id = extent_start + offset;

                Page* data_pg = buffer_pool_.fetch_page(data_page_id);
                if (!data_pg) continue;

                SlottedPage sp(data_pg->get_data());
                for (uint16_t slot = 0; slot < sp.get_num_slots(); ++slot) {
                    uint32_t size;
                    char* tuple_data = sp.get_tuple(slot, &size);
                    if (tuple_data) {
                        Tuple t = Tuple::deserialize(st_schema, tuple_data, size);
                        std::string table_name = std::get<std::string>(t.get_value(1));
                        if (name == table_name) {
                            found_oid = static_cast<uint32_t>(std::get<int32_t>(t.get_value(0)));
                            found_first_page_id = static_cast<page_id_t>(std::get<int32_t>(t.get_value(2)));
                            found = true;
                            break;
                        }
                    }
                }
                buffer_pool_.unpin_page(data_page_id, false);
            }
        }

        page_id_t next_iam = iam_page->next_page_id;
        buffer_pool_.unpin_page(current_iam, false);
        current_iam = next_iam;
    }

    if (!found) return nullptr;

    // 3. Build metadata
    TableMetadata metadata;
    metadata.oid = found_oid;
    metadata.name = name;
    metadata.first_page_id = found_first_page_id;

    // 4. Load columns from sys_columns (using Tuple deserialization)
    Schema sc_schema = sys_columns_schema();
    std::vector<Column> column_list;
    page_id_t col_iam = sys_columns_iam;

    while (col_iam != INVALID_PAGE_ID) {
        Page* col_iam_pg = buffer_pool_.fetch_page(col_iam);
        if (!col_iam_pg) break;

        auto* col_iam_page = reinterpret_cast<const IAMPage*>(col_iam_pg->get_data());

        for (uint16_t ci = 0; ci < col_iam_page->extent_count; ++ci) {
            uint32_t col_extent_id = col_iam_page->extent_ids[ci];
            page_id_t col_extent_start = static_cast<page_id_t>(col_extent_id * EXTENT_SIZE);

            for (int cp = 0; cp < EXTENT_SIZE; ++cp) {
                page_id_t col_page_id = col_extent_start + cp;

                Page* col_data_pg = buffer_pool_.fetch_page(col_page_id);
                if (!col_data_pg) continue;

                SlottedPage col_sp(col_data_pg->get_data());
                for (uint16_t cs = 0; cs < col_sp.get_num_slots(); ++cs) {
                    uint32_t c_size;
                    char* c_data = col_sp.get_tuple(cs, &c_size);
                    if (c_data) {
                        Tuple ct = Tuple::deserialize(sc_schema, c_data, c_size);
                        auto col_table_oid = static_cast<uint32_t>(std::get<int32_t>(ct.get_value(0)));
                        if (col_table_oid == found_oid) {
                            std::string col_name = std::get<std::string>(ct.get_value(1));
                            auto col_type = static_cast<DataType>(std::get<int32_t>(ct.get_value(2)));
                            auto col_length = static_cast<uint16_t>(std::get<int32_t>(ct.get_value(3)));
                            auto col_offset = static_cast<uint16_t>(std::get<int32_t>(ct.get_value(4)));
                            column_list.emplace_back(col_name, col_type, col_length, col_offset);
                        }
                    }
                }
                buffer_pool_.unpin_page(col_page_id, false);
            }
        }

        page_id_t next_col_iam = col_iam_page->next_page_id;
        buffer_pool_.unpin_page(col_iam, false);
        col_iam = next_col_iam;
    }

    metadata.schema = Schema(column_list);

    // Populate cache and return pointer to cached entry
    auto [it, _] = table_cache_.emplace(name, std::move(metadata));
    return &it->second;
}

bool CatalogManager::create_table(const std::string& name, const Schema& schema) {
    // 1. Check if table already exists
    if (get_table(name) != nullptr) {
        return false;
    }

    // 2. Get next OID from the database header
    uint16_t next_oid = get_next_oid();

    // 3. Allocate IAM chain for the new table
    page_id_t new_iam = iam_manager_.create_iam_chain();
    if (new_iam == INVALID_PAGE_ID) {
        return false;
    }

    // 4. Get IAM pages for system tables from header
    Page* header_page = buffer_pool_.fetch_page(HEADER_PAGE_ID);
    if (!header_page) return false;

    auto* db_header = reinterpret_cast<const DatabaseHeader*>(header_page->get_data());
    page_id_t sys_tables_iam = db_header->sys_tables_iam_page;
    page_id_t sys_columns_iam = db_header->sys_columns_iam_page;
    buffer_pool_.unpin_page(HEADER_PAGE_ID, false);

    // 5. Insert into sys_tables using Tuple serialization
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

    // 6. Insert columns into sys_columns using Tuple serialization
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

    // 7. Populate the cache so subsequent lookups skip the disk scan
    TableMetadata cached_meta;
    cached_meta.oid = next_oid;
    cached_meta.name = name;
    cached_meta.schema = schema;
    cached_meta.first_page_id = new_iam;
    table_cache_[name] = std::move(cached_meta);

    return true;
}

bool CatalogManager::delete_table(const std::string &name) {
  	// no-op
	  return false;
}

Schema CatalogManager::sys_tables_schema() {
    // {oid INT, name VARCHAR(32), first_page_id INT, column_count INT}
    // All INTEGER columns use length=4 to match Tuple::serialize_into format
    return Schema({
        Column("oid",           DataType::INTEGER, 4, 0),
        Column("name",          DataType::VARCHAR, MAX_NAME_LENGTH, 4),
        Column("first_page_id", DataType::INTEGER, 4, 36),
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
