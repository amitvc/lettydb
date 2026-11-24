#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "catalog/catalog_defs.h"
#include "catalog/schema.h"
#include "buffer/buffer_pool_manager.h"
#include "storage/page_allocator.h"
#include "storage/tuple.h"
#include "transaction/transaction_manager.h"

namespace letty {

/**
 * @struct TableMetadata
 * @brief In-memory representation of a table's metadata.
 * This structure describes the logical and physical properties of a table
 * known to the database catalog. It does NOT store any table data (tuples);
 * it only describes how and where that data is stored.
 */
struct TableMetadata {
  /** @brief Unique identifier assigned to the table by the catalog. */
  uint32_t table_id;

  /** @brief Table name used by catalog lookups. */
  std::string name;

  /** @brief Column definitions and serialized layout for the table's tuples. */
  Schema schema;

  /**
   * @brief Table's first heap page ID.
   *
   * The storage layer follows the doubly-linked next_page_id chain to traverse
   * data pages belonging to the table.
   */
  page_id_t first_page_id{INVALID_PAGE_ID};

  /**
   * @brief Table's last heap page ID.
   *
   * Reduces the O(N) scanning of entire chain of pages to O(1) when a new page is allocated for the table.
   */
  page_id_t last_page_id{INVALID_PAGE_ID};
};

/**
 * @struct SysTableRecord
 * @brief Represents a single row in the sys_tables system catalog table.
 * Used to insert records into sys_tables without passing individual columns.
 */
struct SysTableRecord {
  uint32_t table_id;
  std::string table_name;
  page_id_t first_page_id{INVALID_PAGE_ID};
  page_id_t last_page_id{INVALID_PAGE_ID};
  int8_t status{0}; // 0 = ACTIVE, 1 = DELETED
};

/**
 * @class CatalogManager
 * @brief The CatalogManager is the authoritative owner of database metadata.
 * It manages and serves information about:
 * - Stores Table metadata when a table is created
 * - Given table name returns Table metadata
 *
 * All page access goes through BufferPoolManager. No direct DiskManager calls.
 */
class CatalogManager {
 public:
  CatalogManager(BufferPoolManager& buffer_pool, PageAllocator& page_allocator, TransactionManager& txn_manager);

  /**
   * @brief Initializes the catalog manager.
   * Checks if sys_tables and sys_columns exist. If not, creates them (Bootstraps).
   */
  void init();

  /**
   * @brief Creates a new table.
   * @param name The name of the table.
   * @param schema The schema of the table.
   * @return true if the table was created.
   * @throws DbException if the table already exists or catalog metadata cannot be written.
   */
  bool create_table(const std::string& name, const Schema& schema);

  /**
   * @brief Retrieves metadata for a table by name.
   * @param name The name of the table.
   * @return Pointer to TableMetadata if found, nullptr otherwise.
   */
  const std::optional<std::reference_wrapper<TableMetadata>> get_table(const std::string& name);

  /**
   * @brief Checks if a table with the given name exists.
   * @param name The name of the table.
   * @return true if the table exists, false otherwise.
   */
  bool table_exists(const std::string& name);

  bool delete_table(const std::string &name);

  /**
   * @brief Updates cached and persisted boundary page IDs when table grows or compacts.
   * @param name The name of the table.
   * @param first_page_id New first page ID of the table chain.
   * @param last_page_id New last page ID of the table chain.
   * @param txn_id Transaction id for logging.
   * @return true if updated successfully.
   */
  bool update_table_page_bounds(const std::string& name, page_id_t first_page_id, page_id_t last_page_id, uint64_t txn_id);

  /** Schema for sys_tables: {oid INT, name VARCHAR(32), first_page_id INT, last_page_id INT, status INT} */
  static Schema sys_tables_schema();

  /** Schema for sys_columns: {table_oid INT, name VARCHAR(32), type INT, length INT, offset INT} */
  static Schema sys_columns_schema();

 private:
  BufferPoolManager& buffer_pool_;
  PageAllocator& page_allocator_;
  TransactionManager& txn_manager_;

  /** In-memory cache of table metadata, keyed by table name.
   *  Populated on create_table() and first get_table() miss.
   *  Eliminates repeated full scans of sys_tables + sys_columns.
   **/
  std::unordered_map<std::string, TableMetadata> table_cache_;
  mutable std::mutex latch_;

  /**
   * We follow a convention that first system's table and system's column will go into page 1 and 2 respectively
   */
  page_id_t sys_tables_first_page_ = 1;
  page_id_t sys_tables_last_page_ = 1;
  page_id_t sys_columns_first_page_ = 2;
  page_id_t sys_columns_last_page_ = 2;

  /**
   *  Will bootstrap the database by creating sys_tables and sys_columns
   */
  void bootstrap();

  /**
   * @brief Eagerly loads all table metadata from sys_tables and sys_columns into the cache.
   */
  void load_all_tables();

  /**
   * @brief Gets the next available table_id and increments the counter in the database header.
   * @return The next available table_id for a new table.
   */
  uint32_t get_next_table_id();

  /**
   * @brief Inserts a tuple into a table, automatically allocating a new page if full.
   *
   * @param first_page_id The first page ID of the table chain.
   * @param last_page_id The last page ID of the table chain (in/out, updated if new page linked).
   * @param data The tuple data to insert.
   * @param size The size of the tuple data.
   * @param txn_id Transaction id.
   * @return true if successful.
   * @throws DbException if the insert cannot complete.
   */
  bool insert_into_table(page_id_t first_page_id, page_id_t& last_page_id, const Byte* data, uint32_t size, uint64_t txn_id);

  /** @brief Helper to insert a record into sys_tables. */
  bool insert_sys_table_record(const SysTableRecord& record, uint64_t txn_id);

  /** @brief Helper to insert a record into sys_columns. */
  bool insert_sys_column_record(uint32_t table_oid, const Column& col, uint64_t txn_id);

  /**
   * @brief Scans a system table via its doubly-linked chain and returns all tuples.
   *
   * Walks the page chain starting at first_page_id, reads every SlottedPage,
   * and deserializes each tuple using the given schema.
   * System tables are small (tens of rows), so materializing is fine.
   *
   * @param first_page_id First page ID of the system table.
   * @param schema Schema used for tuple deserialization.
   * @return All tuples found in the system table.
   */
  std::vector<Tuple> scan_system_table(page_id_t first_page_id, const Schema& schema);
};

}
