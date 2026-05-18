#include "catalog/catalog_manager.h"
#include "storage/slotted_page.h"
#include "storage/tuple.h"
#include "storage/page_utils.h"
#include "catalog/catalog_defs.h"
#include "common/db_exception.h"
#include <cstring>

namespace letty {

CatalogManager::CatalogManager(BufferPoolManager& buffer_pool, IamManager& iam_manager)
    : buffer_pool_(buffer_pool), iam_manager_(iam_manager) {}

void CatalogManager::init() {
  auto db_header = load_page_value<DatabaseHeader>(buffer_pool_, HEADER_PAGE_ID);
  if (!db_header) {
    throw DbException(DbErrorCode::IOError, "failed to fetch database header page during catalog initialization");
  }

  sys_tables_iam_ = db_header->sys_tables_iam_page;
  sys_columns_iam_ = db_header->sys_columns_iam_page;

  if (sys_tables_iam_ == INVALID_PAGE_ID) {
    bootstrap();
  }

  // Eagerly load all tables into the cache
  load_all_tables();
}

void CatalogManager::load_all_tables() {
    // Scan sys_columns once and group by table_oid to avoid an O(N) scan per table.
    std::unordered_map<uint32_t, std::vector<Column>> columns_by_oid;
    if (sys_columns_iam_ != INVALID_PAGE_ID) {
        Schema sc_schema = sys_columns_schema();
        for (const auto& col_row : scan_system_table(sys_columns_iam_, sc_schema)) {
            uint32_t table_oid = static_cast<uint32_t>(std::get<int32_t>(col_row.get_value(0)));
            columns_by_oid[table_oid].emplace_back(
                std::get<std::string>(col_row.get_value(1)),
                static_cast<DataType>(std::get<int32_t>(col_row.get_value(2))),
                static_cast<uint16_t>(std::get<int32_t>(col_row.get_value(3))),
                static_cast<uint16_t>(std::get<int32_t>(col_row.get_value(4)))
            );
        }
    }

    Schema st_schema = sys_tables_schema();
    for (const auto& row : scan_system_table(sys_tables_iam_, st_schema)) {
        uint32_t oid = static_cast<uint32_t>(std::get<int32_t>(row.get_value(0)));
        std::string name = std::get<std::string>(row.get_value(1));
        page_id_t iam = static_cast<page_id_t>(std::get<int32_t>(row.get_value(2)));

        auto it = columns_by_oid.find(oid);
        std::vector<Column> cols = (it != columns_by_oid.end()) ? std::move(it->second) : std::vector<Column>{};

        table_cache_.emplace(name, TableMetadata{oid, name, Schema(std::move(cols)), iam});
    }
}

void CatalogManager::bootstrap() {
    Page* db_header_frame = buffer_pool_.fetch_page(HEADER_PAGE_ID);
    if (!db_header_frame) {
        throw DbException(DbErrorCode::IOError, "failed to fetch header page during catalog bootstrap");
    }
    auto db_header_page = load_page_layout<DatabaseHeader>(db_header_frame);

    // Initialize shared extent at the designated page
    iam_manager_.init_shared_extent(db_header_page.shared_extent_page_id);

    page_id_t sys_tables_iam = iam_manager_.create_iam_chain(SYS_TABLES_NAME);
    page_id_t sys_columns_iam = iam_manager_.create_iam_chain(SYS_COLUMNS_NAME);

    if (sys_tables_iam == INVALID_PAGE_ID || sys_columns_iam == INVALID_PAGE_ID) {
        buffer_pool_.unpin_page(HEADER_PAGE_ID, false);
        throw DbException(DbErrorCode::NoSpace, "failed to create system catalog IAM chains");
    }

    // Update header in-place and cache
    db_header_page.sys_tables_iam_page = sys_tables_iam;
    db_header_page.sys_columns_iam_page = sys_columns_iam;
    store_page_layout(db_header_frame, db_header_page);
    buffer_pool_.unpin_page(HEADER_PAGE_ID, true);

    sys_tables_iam_ = sys_tables_iam;
    sys_columns_iam_ = sys_columns_iam;

    page_id_t sys_tables_first_page = iam_manager_.allocate_extent_for_table(sys_tables_iam);
    page_id_t sys_columns_first_page = iam_manager_.allocate_extent_for_table(sys_columns_iam);
    if (sys_tables_first_page == INVALID_PAGE_ID || sys_columns_first_page == INVALID_PAGE_ID) {
        throw DbException(DbErrorCode::NoSpace, "failed to allocate system catalog data extents");
    }

    Page* sys_tables_page = buffer_pool_.new_page(sys_tables_first_page);
    if (!sys_tables_page) {
      sys_tables_page = buffer_pool_.fetch_page(sys_tables_first_page);
      if (sys_tables_page && (sys_tables_page->get_pin_count() != 1 || sys_tables_page->is_dirty())) {
        buffer_pool_.unpin_page(sys_tables_first_page, false);
        sys_tables_page = nullptr;
      }
    }

    if (!sys_tables_page) {
        throw DbException(DbErrorCode::IOError, "failed to create sys_tables first page during catalog bootstrap");
    }
    SlottedPage::init(sys_tables_page->get_data());
    buffer_pool_.unpin_page(sys_tables_first_page, true);

    Page* sys_columns_page = buffer_pool_.new_page(sys_columns_first_page);
    if (!sys_columns_page) {
      sys_columns_page = buffer_pool_.fetch_page(sys_columns_first_page);
      if (sys_columns_page && (sys_columns_page->get_pin_count() != 1 || sys_columns_page->is_dirty())) {
        buffer_pool_.unpin_page(sys_columns_first_page, false);
        sys_columns_page = nullptr;
      }
    }

    if (!sys_columns_page) {
        throw DbException(DbErrorCode::IOError, "failed to create sys_columns first page during catalog bootstrap");
    }
    SlottedPage::init(sys_columns_page->get_data());
    buffer_pool_.unpin_page(sys_columns_first_page, true);

    // Insert the metadata for sys_tables AND sys_columns INTO sys_tables
    insert_sys_table_record({SYS_TABLES_TABLE_OID, SYS_TABLES_NAME, sys_tables_iam, SYS_TABLES_COLUMN_COUNT});
    insert_sys_table_record({SYS_COLUMNS_TABLE_OID, SYS_COLUMNS_NAME, sys_columns_iam, SYS_COLUMNS_COLUMN_COUNT});

    // Insert column definitions into sys_columns
    Schema st_schema = sys_tables_schema();
    for (const auto& col : st_schema.get_columns()) {
        insert_sys_column_record(SYS_TABLES_TABLE_OID, col);
    }
    Schema sc_schema = sys_columns_schema();
    for (const auto& col : sc_schema.get_columns()) {
        insert_sys_column_record(SYS_COLUMNS_TABLE_OID, col);
    }
}

uint32_t CatalogManager::get_next_oid() {
    Page* db_header_frame = buffer_pool_.fetch_page(HEADER_PAGE_ID);
    if (!db_header_frame) {
        throw DbException(DbErrorCode::IOError, "failed to fetch header page for OID allocation");
    }
    auto db_header_page = load_page_layout<DatabaseHeader>(db_header_frame);

    uint16_t oid = db_header_page.next_table_oid;
    db_header_page.next_table_oid++;

    store_page_layout(db_header_frame, db_header_page);
    buffer_pool_.unpin_page(HEADER_PAGE_ID, true);
    return oid;
}

bool CatalogManager::insert_into_table(page_id_t iam_page_id, const char* data, uint32_t size) {
    page_id_t target_page_id = iam_manager_.find_page_with_space(iam_page_id, size);

    // No existing page has room — grow the table
    if (target_page_id == INVALID_PAGE_ID) {
        target_page_id = iam_manager_.allocate_extent_for_table(iam_page_id);
        if (target_page_id == INVALID_PAGE_ID) {
            throw DbException(DbErrorCode::NoSpace, "failed to allocate new extent for catalog table");
        }

        // Prefer new_page for a freshly allocated page; fetch only handles the
        // case where the frame was already cached by earlier file extension.
        Page* new_pg = buffer_pool_.new_page(target_page_id);
        if (!new_pg) {
          new_pg = buffer_pool_.fetch_page(target_page_id);
          if (new_pg && (new_pg->get_pin_count() != 1 || new_pg->is_dirty())) {
            buffer_pool_.unpin_page(target_page_id, false);
            new_pg = nullptr;
          }
        }

        if (!new_pg) {
            throw DbException(DbErrorCode::IOError, "failed to create catalog data page");
        }
        SlottedPage::init(new_pg->get_data());
        buffer_pool_.unpin_page(target_page_id, true);
    }

    Page* page = buffer_pool_.fetch_page(target_page_id);
    if (!page) {
        throw DbException(DbErrorCode::IOError, "failed to fetch catalog data page");
    }

    SlottedPage sp(page->get_data());
    auto slot_id = sp.insert_tuple(data, size);
    if (!slot_id) {
        buffer_pool_.unpin_page(target_page_id, false);
        throw DbException(DbErrorCode::NoSpace, "failed to insert catalog tuple into page " + std::to_string(target_page_id));
    }

    buffer_pool_.unpin_page(target_page_id, true);
    return true;
}

std::vector<Tuple> CatalogManager::scan_system_table(page_id_t iam_head, const Schema& schema) {
    std::vector<Tuple> results;

    page_id_t current_iam = iam_head;
    while (current_iam != INVALID_PAGE_ID) {
        auto iam_page = load_page_value<IAMPage>(buffer_pool_, current_iam);
        if (!iam_page) {
            throw DbException(DbErrorCode::IOError, "failed to fetch system table IAM page " + std::to_string(current_iam));
        }

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
        current_iam = next_iam;
    }

    return results;
}

bool CatalogManager::insert_sys_table_record(const SysTableRecord& record) {
    Schema st_schema = sys_tables_schema();
    char buf[PAGE_SIZE];
    uint32_t data_size;

    Tuple table_tuple({static_cast<int32_t>(record.oid),
                       record.table_name,
                       static_cast<int32_t>(record.iam_page_id),
                       static_cast<int32_t>(record.column_count)});
    if (!table_tuple.serialize(st_schema, buf, PAGE_SIZE, &data_size)) {
        throw DbException(DbErrorCode::InvalidArgument, "failed to serialize sys_tables record");
    }

    return insert_into_table(sys_tables_iam_, buf, data_size);
}

bool CatalogManager::insert_sys_column_record(uint32_t table_oid, const Column& col) {
    Schema sc_schema = sys_columns_schema();
    char buf[PAGE_SIZE];
    uint32_t data_size;

    Tuple col_tuple({static_cast<int32_t>(table_oid),
                     col.get_name(),
                     static_cast<int32_t>(col.get_type()),
                     static_cast<int32_t>(col.get_length()),
                     static_cast<int32_t>(col.get_offset())});
    if (!col_tuple.serialize(sc_schema, buf, PAGE_SIZE, &data_size)) {
        throw DbException(DbErrorCode::InvalidArgument, "failed to serialize sys_columns record");
    }
    return insert_into_table(sys_columns_iam_, buf, data_size);
}


bool CatalogManager::table_exists(const std::string& name) {
    return table_cache_.find(name) != table_cache_.end();
}

const TableMetadata* CatalogManager::get_table(const std::string& name) {
    auto it = table_cache_.find(name);
    if (it != table_cache_.end()) {
        return &it->second;
    }
    return nullptr;
}

bool CatalogManager::create_table(const std::string& name, const Schema& schema) {
    if (table_exists(name)) {
        throw DbException(DbErrorCode::DuplicateTable, "table '" + name + "' already exists");
    }

    if (schema.get_columns().size() > MAX_COLUMNS) {
        throw DbException(DbErrorCode::InvalidArgument,
            "table '" + name + "' exceeds maximum column count of " + std::to_string(MAX_COLUMNS));
    }

    if (sys_tables_iam_ == INVALID_PAGE_ID) {
        throw DbException(DbErrorCode::Corruption, "sys_tables IAM page is not initialized");
    }

    uint16_t next_oid = get_next_oid();

    page_id_t new_iam = iam_manager_.create_iam_chain(name);
    if (new_iam == INVALID_PAGE_ID) {
        throw DbException(DbErrorCode::NoSpace, "failed to create IAM chain for table '" + name + "'");
    }

    insert_sys_table_record({next_oid, name, new_iam, static_cast<int32_t>(schema.get_columns().size())});

    for (const auto& col : schema.get_columns()) {
        insert_sys_column_record(next_oid, col);
    }

    // Cache the newly created table directly
    table_cache_.emplace(name, TableMetadata{
        next_oid,
        name,
        schema,
        new_iam
    });

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
