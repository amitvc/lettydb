#pragma once

#include <functional>
#include <string>
#include "buffer/buffer_pool_manager.h"
#include "iam_manager.h"
#include "tuple.h"
#include "catalog/catalog_manager.h"

namespace letty {

/**
 * @class TableManager
 * @brief Manages data operations on user tables.
 *
 * TableManager provides a high-level interface for inserting and scanning
 * table data. It uses CatalogManager to get table metadata, IamManager
 * to find pages with space, and BufferPoolManager for page I/O.
 *
 * All page access goes through BufferPoolManager. No direct DiskManager calls.
 *
 * Responsibilities:
 * - Insert rows into tables
 * - Scan table data using callbacks
 * - Manage page allocation when tables grow
 *
 * Non-responsibilities:
 * - Schema management (CatalogManager)
 * - Transaction handling (future TransactionManager)
 * - Query execution (future Executor)
 */
class TableManager {
 public:
  TableManager(BufferPoolManager& buffer_pool, IamManager& iam_manager, CatalogManager& catalog_manager);

  /**
   * @brief Inserts a row into a table.
   *
   * @param table_name The name of the table.
   * @param tuple The tuple to insert.
   * @return true if successful, false otherwise.
   */
  bool insert_row(const std::string& table_name, const Tuple& tuple);

  /**
   * @brief Inserts a row using pre-resolved metadata (avoids catalog lookup).
   *
   * @param meta The table's metadata (schema + IAM page).
   * @param tuple The tuple to insert.
   * @return true if successful, false otherwise.
   */
  bool insert_row(const TableMetadata& meta, const Tuple& tuple);

  /**
   * @brief Inserts multiple rows into a table in a single batch.
   *
   * Performs a single catalog lookup and keeps pages pinned while filling
   * them with multiple tuples, reducing overhead significantly.
   *
   * @param table_name The name of the table.
   * @param tuples The tuples to insert.
   * @return Number of rows successfully inserted.
   */
  uint32_t insert_rows(const std::string& table_name, const std::vector<Tuple>& tuples);

  /**
   * @brief Scans all rows in a table.
   *
   * Traverses the table's IAM chain, reads each SlottedPage, and
   * invokes the callback for each tuple found.
   *
   * @param table_name The name of the table.
   * @param callback Function called for each tuple (raw data, size).
   * @return true if scan completed successfully, false on error.
   */
  bool scan_table(const std::string& table_name,
                  const std::function<void(const char* data, uint32_t size)>& callback);

  /**
   * @brief Scans all rows in a table, deserializing to Tuples.
   *
   * @param table_name The name of the table.
   * @param callback Function called for each deserialized tuple.
   * @return true if scan completed successfully, false on error.
   */
  bool scan_table_tuples(const std::string& table_name,
                         const std::function<void(const Tuple& tuple)>& callback);

 private:
  BufferPoolManager& buffer_pool_;
  IamManager& iam_manager_;
  CatalogManager& catalog_manager_;

  /**
   * @brief Returns a page that can accept `needed_space` bytes, allocating a
   *        new extent (and initializing it as a SlottedPage) if none exists.
   */
  page_id_t acquire_page_for_insert(page_id_t iam_head, uint32_t needed_space);

  /**
   * @brief Attempts to insert a serialized tuple into an already-pinned page.
   * @return true if insertion succeeded, false if the page has no room.
   */
  bool try_insert_into_page(Page* page, const char* buf, uint32_t data_size);

  /**
   * @brief Scans all live tuples across every page in one extent, invoking
   *        callback with raw (data, size) for each.
   */
  void scan_extent(uint32_t extent_id,
                   const std::function<void(const char*, uint32_t)>& callback);

  /**
   * @brief Scans all live tuples across every page in one extent, deserializing
   *        each into a Tuple before invoking callback.
   */
  void scan_extent_tuples(uint32_t extent_id, const Schema& schema,
                          const std::function<void(const Tuple&)>& callback);
};

}
