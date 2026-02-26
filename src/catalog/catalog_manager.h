#pragma once

#include <string>
#include <optional>
#include <utility>
#include <vector>
#include <memory>
#include <unordered_map>

#include "catalog/catalog_defs.h"
#include "catalog/schema.h"
#include "buffer/buffer_pool_manager.h"
#include "storage/iam_manager.h"
#include "storage/tuple.h"

namespace letty {

/**
 * @struct TableMetadata
 * @brief In-memory representation of a table's metadata.
 * This structure describes the logical and physical properties of a table
 * known to the database catalog. It does NOT store any table data (tuples);
 * it only describes how and where that data is stored.
 */
struct TableMetadata {
  uint32_t oid;
  std::string name; // table name.
  Schema schema;

  // Head of the IAM (Index Allocation Map) chain for this table.
  // The storage layer follows this chain to find all extents (and their
  // data pages) that belong to the table.
  page_id_t iam_page_id;
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
  CatalogManager(BufferPoolManager& buffer_pool, IamManager& iam_manager);

  /**
   * @brief Initializes the catalog manager.
   * Checks if sys_tables and sys_columns exist. If not, creates them (Bootstraps).
   */
  void init();

  /**
   * @brief Creates a new table.
   * @param name The name of the table.
   * @param schema The schema of the table.
   * @return The OID of the newly created table, or error if failed.
   */
  bool create_table(const std::string& name, const Schema& schema);

  /**
   * @brief Retrieves metadata for a table by name.
   * @param name The name of the table.
   * @return Pointer to TableMetadata if found, nullptr otherwise.
   */
  const TableMetadata* get_table(const std::string& name);

  bool delete_table(const std::string &name);

  /** Schema for sys_tables: {oid INT, name VARCHAR(32), iam_page_id INT, column_count INT} */
  static Schema sys_tables_schema();

  /** Schema for sys_columns: {table_oid INT, name VARCHAR(32), type INT, length INT, offset INT} */
  static Schema sys_columns_schema();

 private:
  BufferPoolManager& buffer_pool_;
  IamManager& iam_manager_;

  /** In-memory cache of table metadata, keyed by table name.
   *  Populated on create_table() and first get_table() miss.
   *  Eliminates repeated full scans of sys_tables + sys_columns.
   **/
  std::unordered_map<std::string, TableMetadata> table_cache_;

  /**
   *  Will bootstrap the database by creating sys_tables and sys_columns
   */
  void bootstrap();

  /**
   * @brief Gets the next available OID and increments the counter in the database header.
   * @return The next available OID for a new table.
   */
  uint16_t get_next_oid();

  /**
   * @brief Inserts a tuple into a table, automatically finding a page with space.
   *
   * If no page has enough space, allocates a new extent for the table.
   *
   * @param iam_page_id The IAM page ID for the table.
   * @param data The tuple data to insert.
   * @param size The size of the tuple data.
   * @return true if successful, false otherwise.
   */
  bool insert_into_table(page_id_t iam_page_id, const char* data, uint32_t size);

  /**
   * @brief Reads the database header and returns the IAM page IDs for system tables.
   * @return (sys_tables_iam, sys_columns_iam), or (INVALID_PAGE_ID, INVALID_PAGE_ID) if uninitialized.
   */
  std::pair<page_id_t, page_id_t> get_system_iam_pages();

  /**
   * @brief Scans a system table via its IAM chain and returns all tuples.
   *
   * Walks the IAM chain starting at iam_head, reads every SlottedPage in
   * every extent, and deserializes each tuple using the given schema.
   * System tables are small (tens of rows), so materializing is fine.
   *
   * @param iam_head  IAM chain head page ID for the system table.
   * @param schema    Schema used for tuple deserialization.
   * @return All tuples found in the system table.
   */
  std::vector<Tuple> scan_system_table(page_id_t iam_head, const Schema& schema);
};

}
