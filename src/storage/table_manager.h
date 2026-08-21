#pragma once

#include <string>
#include "buffer/buffer_pool_manager.h"
#include "iam_manager.h"
#include "table_scanner.h"
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
 * - Create scanners for table data
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
   * @return true if successful.
   * @throws DbException if the table is missing or the insert cannot complete.
   */
  bool insert_row(const std::string& table_name, const Tuple& tuple);

  /**
   * @brief Inserts a row using pre-resolved metadata (avoids catalog lookup).
   *
   * @param meta The table's metadata (schema + IAM page).
   * @param tuple The tuple to insert.
   * @return true if successful.
   * @throws DbException if the insert cannot complete.
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
   * @throws DbException if the table is missing or the batch insert cannot complete.
   */
  uint32_t insert_rows(const std::string& table_name, const std::vector<Tuple>& tuples);

  /**
   * @brief Creates a scanner for streaming raw tuple bytes from a table.
   *
   * @param table_name The name of the table.
   * @return Scanner positioned before the first tuple.
   * @throws DbException if the table is missing.
   */
  TableScanner scan_table(const std::string& table_name);

  /**
   * @brief Deletes a row from a table by page and slot.
   *
   * Fetches the page, marks the slot as a tombstone, marks the page dirty,
   * and unpins it.
   *
   * @param table_name The name of the table.
   * @param page_id The page ID containing the tuple.
   * @param slot_id The slot index of the tuple to delete.
   * @return true if the tuple was live and is now deleted.
   *         false if the slot was already deleted or the page failed to load.
   * @throws DbException if the table is missing.
   */
  bool delete_row(const std::string& table_name, page_id_t page_id, uint16_t slot_id);

  /**
   * @brief Deletes a row using pre-resolved metadata (avoids catalog lookup).
   *
   * @param meta The table's metadata (schema + IAM page).
   * @param page_id The page ID containing the tuple.
   * @param slot_id The slot index of the tuple to delete.
   * @return true if the tuple was live and is now deleted.
   *         false if the slot was already deleted or the page failed to load.
   */
  bool delete_row(const TableMetadata& meta, page_id_t page_id, uint16_t slot_id);

  /**
   * @brief Reclaims space from deleted tuples in all pages of a table.
   *
   * Walks every page in the table's IAM chain and calls SlottedPage::compact()
   * on each. Pages without tombstones are skipped. The caller should hold a
   * reference to the table's metadata.
   *
   * @param table_name The name of the table.
   * @throws DbException if the table is missing.
   */
  void compact_table(const std::string& table_name);

  /**
   * @brief Reclaims space using pre-resolved metadata (avoids catalog lookup).
   *
   * @param meta The table's metadata (schema + IAM page).
   */
  void compact_table(const TableMetadata& meta);

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

};

}
