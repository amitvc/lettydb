#include "catalog/catalog_manager.h"

#include <cstring>
#include <string>
#include <vector>

#include "catalog/catalog_defs.h"
#include "common/db_exception.h"
#include "storage/page_utils.h"
#include "storage/slotted_page.h"
#include "storage/storage_def.h"
#include "storage/tuple.h"

namespace letty {

CatalogManager::CatalogManager(BufferPoolManager &buffer_pool,
							   PageAllocator &page_allocator,
							   TransactionManager &txn_manager)
	: buffer_pool_(buffer_pool), page_allocator_(page_allocator), txn_manager_(txn_manager) {}

void CatalogManager::init() {
  auto db_metadata = load_page_body_value<DatabaseMetadata>(buffer_pool_, DATABASE_HEADER_PAGE_ID);
  if (!db_metadata) {
	throw DbException(DbErrorCode::IOError, "failed to fetch database header page during catalog initialization");
  }

  sys_tables_first_page_ = db_metadata->sys_tables_first_page;
  sys_columns_first_page_ = db_metadata->sys_columns_first_page;
  sys_tables_last_page_ = sys_tables_first_page_;
  sys_columns_last_page_ = sys_columns_first_page_;

  if (db_metadata->table_count == 0) {
	bootstrap();
  }

  load_all_tables();
}

void CatalogManager::load_all_tables() {
  std::unordered_map<uint32_t, std::vector<Column>> columns_by_oid;
  if (sys_columns_first_page_ != INVALID_PAGE_ID) {
	Schema sc_schema = sys_columns_schema();
	for (const auto &col_row : scan_system_table(sys_columns_first_page_, sc_schema)) {
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
  for (const auto &row : scan_system_table(sys_tables_first_page_, st_schema)) {
	int8_t status = std::get<int8_t>(row.get_value(4));
	if (status != 0) {
	  continue;
	}

	uint32_t table_id = static_cast<uint32_t>(std::get<int32_t>(row.get_value(0)));
	std::string name = std::get<std::string>(row.get_value(1));
	page_id_t first_page_id = static_cast<page_id_t>(std::get<int32_t>(row.get_value(2)));
	page_id_t last_page_id = static_cast<page_id_t>(std::get<int32_t>(row.get_value(3)));

	auto it = columns_by_oid.find(table_id);
	std::vector<Column> cols = (it != columns_by_oid.end()) ? std::move(it->second) : std::vector<Column>{};

	table_cache_.emplace(name, TableMetadata{table_id, name, Schema(std::move(cols)), first_page_id, last_page_id});
  }

  auto st_it = table_cache_.find(SYS_TABLES_NAME);
  if (st_it != table_cache_.end()) {
	sys_tables_last_page_ = st_it->second.last_page_id;
  }
  auto sc_it = table_cache_.find(SYS_COLUMNS_NAME);
  if (sc_it != table_cache_.end()) {
	sys_columns_last_page_ = sc_it->second.last_page_id;
  }
}

void CatalogManager::bootstrap() {
  uint64_t txn_id = txn_manager_.begin();

  page_id_t sys_tables_page = page_allocator_.allocate_page(SYS_TABLES_TABLE_OID, PageType::DATA, txn_id);
  page_id_t sys_columns_page = page_allocator_.allocate_page(SYS_COLUMNS_TABLE_OID, PageType::DATA, txn_id);

  if (sys_tables_page == INVALID_PAGE_ID || sys_columns_page == INVALID_PAGE_ID) {
	throw DbException(DbErrorCode::NoSpace, "failed to allocate system catalog pages");
  }

  sys_tables_first_page_ = sys_tables_page;
  sys_tables_last_page_ = sys_tables_page;
  sys_columns_first_page_ = sys_columns_page;
  sys_columns_last_page_ = sys_columns_page;

  PageGuard db_header_guard = buffer_pool_.fetch_page_guard(DATABASE_HEADER_PAGE_ID);
  if (!db_header_guard) {
	throw DbException(DbErrorCode::IOError, "failed to fetch header page during catalog bootstrap");
  }
  auto db_metadata = load_page_body<DatabaseMetadata>(db_header_guard);
  db_metadata.sys_tables_first_page = sys_tables_first_page_;
  db_metadata.sys_columns_first_page = sys_columns_first_page_;
  db_metadata.table_count = 2;
  store_page_body(db_header_guard, db_metadata);
  db_header_guard.drop();

  insert_sys_table_record({SYS_TABLES_TABLE_OID, SYS_TABLES_NAME, sys_tables_first_page_, sys_tables_last_page_, 0},
						  txn_id);
  insert_sys_table_record({SYS_COLUMNS_TABLE_OID, SYS_COLUMNS_NAME, sys_columns_first_page_, sys_columns_last_page_, 0},
						  txn_id);

  Schema st_schema = sys_tables_schema();
  for (const auto &col : st_schema.get_columns()) {
	insert_sys_column_record(SYS_TABLES_TABLE_OID, col, txn_id);
  }

  Schema sc_schema = sys_columns_schema();
  for (const auto &col : sc_schema.get_columns()) {
	insert_sys_column_record(SYS_COLUMNS_TABLE_OID, col, txn_id);
  }

  if (!txn_manager_.commit(txn_id)) {
	throw DbException(DbErrorCode::Internal, "failed to commit catalog bootstrap transaction");
  }
  buffer_pool_.flush_all_pages();
}

uint32_t CatalogManager::get_next_table_id() {
  PageGuard raw_header_guard = buffer_pool_.fetch_page_guard(DATABASE_HEADER_PAGE_ID);
  if (!raw_header_guard) {
	throw DbException(DbErrorCode::IOError, "failed to fetch header page for table_id allocation");
  }
  auto db_metadata = load_page_body<DatabaseMetadata>(raw_header_guard);

  uint16_t table_id = db_metadata.next_table_id;
  db_metadata.next_table_id++;
  db_metadata.table_count++;

  store_page_body(raw_header_guard, db_metadata);
  return table_id;
}

bool CatalogManager::insert_into_table(page_id_t first_page_id,
									   page_id_t &last_page_id,
									   const Byte *data,
									   uint32_t size,
									   uint64_t txn_id) {
  if (last_page_id == INVALID_PAGE_ID) {
	return false;
  }

  PageGuard guard = buffer_pool_.fetch_page_guard(last_page_id);
  if (!guard) {
	throw DbException(DbErrorCode::IOError, "failed to fetch catalog data page " + std::to_string(last_page_id));
  }

  SlottedPage sp = SlottedPage::from_page(guard);
  auto slot_id = sp.insert_tuple(data, size);
  if (slot_id.has_value()) {
	guard.mark_dirty();
	return true;
  }

  guard.drop();

  PageGuard tail_guard = buffer_pool_.fetch_page_guard(last_page_id);
  if (!tail_guard) {
	throw DbException(DbErrorCode::IOError, "failed to fetch tail page " + std::to_string(last_page_id));
  }
  PageHeader tail_header = load_page_layout<PageHeader>(tail_guard);
  tail_guard.drop();

  page_id_t new_page_id = page_allocator_.allocate_page(tail_header.table_id, PageType::DATA, txn_id);
  if (new_page_id == INVALID_PAGE_ID) {
	throw DbException(DbErrorCode::NoSpace, "failed to allocate new page for catalog table");
  }

  if (!page_allocator_.link_page(last_page_id, new_page_id, txn_id)) {
	throw DbException(DbErrorCode::Internal, "failed to link newly allocated catalog page");
  }

  PageGuard new_guard = buffer_pool_.fetch_page_guard(new_page_id);
  if (!new_guard) {
	throw DbException(DbErrorCode::IOError, "failed to fetch newly allocated catalog page");
  }

  SlottedPage new_sp = SlottedPage::from_page(new_guard);
  auto new_slot_id = new_sp.insert_tuple(data, size);
  if (!new_slot_id.has_value()) {
	throw DbException(DbErrorCode::NoSpace,
					  "failed to insert catalog tuple into new page " + std::to_string(new_page_id));
  }

  new_guard.mark_dirty();
  last_page_id = new_page_id;
  return true;
}

std::vector<Tuple> CatalogManager::scan_system_table(page_id_t first_page_id, const Schema &schema) {
  std::vector<Tuple> results;
  page_id_t current_page_id = first_page_id;

  while (current_page_id != INVALID_PAGE_ID) {
	PageGuard guard = buffer_pool_.fetch_page_guard(current_page_id);
	if (!guard) {
	  throw DbException(DbErrorCode::IOError, "failed to fetch system table page " + std::to_string(current_page_id));
	}

	SlottedPage sp = SlottedPage::from_page(guard);
	for (uint16_t slot = 0; slot < sp.get_num_slots(); ++slot) {
	  uint32_t sz = 0;
	  const Byte *data = sp.get_tuple(slot, &sz);
	  if (!data) {
		continue;
	  }
	  results.push_back(Tuple::deserialize(schema, data, sz));
	}

	PageHeader header = load_page_layout<PageHeader>(guard);
	current_page_id = header.next_page_id;
  }

  return results;
}

bool CatalogManager::insert_sys_table_record(const SysTableRecord &record, uint64_t txn_id) {
  Schema st_schema = sys_tables_schema();
  Byte buf[PAGE_SIZE];

  Tuple table_tuple({static_cast<int32_t>(record.table_id),
					 record.table_name,
					 static_cast<int32_t>(record.first_page_id),
					 static_cast<int32_t>(record.last_page_id),
					 record.status});
  uint32_t data_size = table_tuple.serialize(st_schema, buf, PAGE_SIZE);

  return insert_into_table(sys_tables_first_page_, sys_tables_last_page_, buf, data_size, txn_id);
}

bool CatalogManager::insert_sys_column_record(uint32_t table_oid, const Column &col, uint64_t txn_id) {
  Schema sc_schema = sys_columns_schema();
  Byte buf[PAGE_SIZE];

  Tuple col_tuple({static_cast<int32_t>(table_oid),
				   col.get_name(),
				   static_cast<int32_t>(col.get_type()),
				   static_cast<int32_t>(col.get_length()),
				   static_cast<int32_t>(col.get_offset())});
  uint32_t data_size = col_tuple.serialize(sc_schema, buf, PAGE_SIZE);

  return insert_into_table(sys_columns_first_page_, sys_columns_last_page_, buf, data_size, txn_id);
}

bool CatalogManager::table_exists(const std::string &name) {
  std::lock_guard<std::mutex> lock(latch_);
  return table_cache_.find(name) != table_cache_.end();
}

const std::optional<std::reference_wrapper<TableMetadata>> CatalogManager::get_table(const std::string &name) {
  std::lock_guard<std::mutex> lock(latch_);
  auto it = table_cache_.find(name);
  if (it != table_cache_.end()) {
	return std::ref(it->second);
  }
  return std::nullopt;
}

bool CatalogManager::create_table(const std::string &name, const Schema &schema) {
  std::lock_guard<std::mutex> lock(latch_);
  if (table_cache_.find(name) != table_cache_.end()) {
	throw DbException(DbErrorCode::DuplicateTable, "table '" + name + "' already exists");
  }

  if (schema.get_columns().size() > MAX_COLUMNS) {
	throw DbException(DbErrorCode::InvalidArgument,
					  "table '" + name + "' exceeds maximum column count of " + std::to_string(MAX_COLUMNS));
  }

  uint32_t next_table_id = get_next_table_id();
  uint64_t txn_id = txn_manager_.begin();

  page_id_t
	  initial_page_id = page_allocator_.allocate_page(static_cast<uint16_t>(next_table_id), PageType::DATA, txn_id);
  if (initial_page_id == INVALID_PAGE_ID) {
	throw DbException(DbErrorCode::NoSpace, "failed to allocate initial data page for table '" + name + "'");
  }

  insert_sys_table_record({next_table_id, name, initial_page_id, initial_page_id, 0}, txn_id);

  for (const auto &col : schema.get_columns()) {
	insert_sys_column_record(next_table_id, col, txn_id);
  }

  if (!txn_manager_.commit(txn_id)) {
	throw DbException(DbErrorCode::Internal, "failed to commit CREATE TABLE transaction for '" + name + "'");
  }
  buffer_pool_.flush_all_pages();

  table_cache_.emplace(name, TableMetadata{
	  next_table_id,
	  name,
	  schema,
	  initial_page_id,
	  initial_page_id
  });

  return true;
}

bool CatalogManager::delete_table(const std::string &name) {
  std::lock_guard<std::mutex> lock(latch_);
  auto it = table_cache_.find(name);
  if (it == table_cache_.end()) {
	return false;
  }

  uint64_t txn_id = txn_manager_.begin();
  if (!page_allocator_.free_page_chain(it->second.first_page_id, txn_id)) {
	txn_manager_.abort(txn_id);
	return false;
  }

  page_id_t current_page_id = sys_tables_first_page_;
  Schema st_schema = sys_tables_schema();
  bool updated = false;

  while (current_page_id != INVALID_PAGE_ID && !updated) {
	PageGuard guard = buffer_pool_.fetch_page_guard(current_page_id);
	if (!guard) {
	  break;
	}
	SlottedPage sp = SlottedPage::from_page(guard);
	for (uint16_t slot = 0; slot < sp.get_num_slots(); ++slot) {
	  uint32_t sz = 0;
	  const Byte *data = sp.get_tuple(slot, &sz);
	  if (!data) {
		continue;
	  }
	  Tuple t = Tuple::deserialize(st_schema, data, sz);
	  uint32_t table_id = static_cast<uint32_t>(std::get<int32_t>(t.get_value(0)));
	  if (table_id == it->second.table_id) {
		Tuple updated_tuple({static_cast<int32_t>(table_id),
							 it->second.name,
							 static_cast<int32_t>(it->second.first_page_id),
							 static_cast<int32_t>(it->second.last_page_id),
							 static_cast<int8_t>(1)});
		Byte buf[PAGE_SIZE];
		uint32_t new_size = updated_tuple.serialize(st_schema, buf, PAGE_SIZE);
		sp.apply_slot_payload(slot, buf, new_size);
		guard.mark_dirty();
		updated = true;
		break;
	  }
	}
	PageHeader header = load_page_layout<PageHeader>(guard);
	current_page_id = header.next_page_id;
  }

  if (!txn_manager_.commit(txn_id)) {
	return false;
  }

  table_cache_.erase(it);
  return true;
}

bool CatalogManager::update_table_page_bounds(const std::string &name,
											  page_id_t first_page_id,
											  page_id_t last_page_id,
											  uint64_t txn_id) {
  std::lock_guard<std::mutex> lock(latch_);
  auto it = table_cache_.find(name);
  if (it == table_cache_.end()) {
	return false;
  }

  it->second.first_page_id = first_page_id;
  it->second.last_page_id = last_page_id;

  if (name == SYS_TABLES_NAME) {
	sys_tables_first_page_ = first_page_id;
	sys_tables_last_page_ = last_page_id;
  } else if (name == SYS_COLUMNS_NAME) {
	sys_columns_first_page_ = first_page_id;
	sys_columns_last_page_ = last_page_id;
  }

  page_id_t current_page_id = sys_tables_first_page_;
  Schema st_schema = sys_tables_schema();
  bool updated = false;

  while (current_page_id != INVALID_PAGE_ID && !updated) {
	PageGuard guard = buffer_pool_.fetch_page_guard(current_page_id);
	if (!guard) {
	  break;
	}
	SlottedPage sp = SlottedPage::from_page(guard);
	for (uint16_t slot = 0; slot < sp.get_num_slots(); ++slot) {
	  uint32_t sz = 0;
	  const Byte *data = sp.get_tuple(slot, &sz);
	  if (!data) {
		continue;
	  }
	  Tuple t = Tuple::deserialize(st_schema, data, sz);
	  uint32_t table_id = static_cast<uint32_t>(std::get<int32_t>(t.get_value(0)));
	  if (table_id == it->second.table_id) {
		Tuple updated_tuple({static_cast<int32_t>(table_id),
							 it->second.name,
							 static_cast<int32_t>(first_page_id),
							 static_cast<int32_t>(last_page_id),
							 static_cast<int8_t>(0)});
		Byte buf[PAGE_SIZE];
		uint32_t new_size = updated_tuple.serialize(st_schema, buf, PAGE_SIZE);
		sp.apply_slot_payload(slot, buf, new_size);
		guard.mark_dirty();
		updated = true;
		break;
	  }
	}
	PageHeader header = load_page_layout<PageHeader>(guard);
	current_page_id = header.next_page_id;
  }

  return updated;
}

Schema CatalogManager::sys_tables_schema() {
  return Schema({
					Column("oid", DataType::INTEGER, 4, 0),
					Column("name", DataType::VARCHAR, MAX_NAME_LENGTH, 4),
					Column("first_page_id", DataType::INTEGER, 4, 36),
					Column("last_page_id", DataType::INTEGER, 4, 40),
					Column("status", DataType::TINYINT, 1, 44)
				});
}

Schema CatalogManager::sys_columns_schema() {
  return Schema({
					Column("table_oid", DataType::INTEGER, 4, 0),
					Column("name", DataType::VARCHAR, MAX_NAME_LENGTH, 4),
					Column("type", DataType::INTEGER, 4, 36),
					Column("length", DataType::INTEGER, 4, 40),
					Column("offset", DataType::INTEGER, 4, 44)
				});
}

}
