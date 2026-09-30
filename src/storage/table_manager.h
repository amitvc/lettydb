#pragma once

#include <string>
#include "buffer/buffer_pool_manager.h"
#include "iam_manager.h"
#include "table_scanner.h"
#include "tuple.h"
#include "catalog/catalog_manager.h"

namespace letty {

/**
 * @struct CompactionStats
 * @brief Statistics returned by TableManager::compact_extents().
 */
struct CompactionStats {
  uint32_t tuples_migrated = 0;
  uint32_t extents_freed = 0;
  uint32_t pages_freed = 0;
};

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
   * @brief Moves live tuples between pages to empty entire extents, then
   *        deallocates them back to the GAM.
   *
   * Walks the IAM chain and collects per-page statistics (live tuple count,
   * free space), then migrates tuples from sparse pages into denser ones.
   * Surviving pages are defragmented in-place. When all 8 pages in an extent
   * become empty, the extent is removed from the IAM chain and deallocated.
   *
   * @param meta The table's metadata (schema + IAM page).
   * @return Stats about the compaction (tuples migrated, extents freed).
   */
  CompactionStats compact_extents(const TableMetadata& meta);

  /**
   * @brief Starts the process of table compaction.
   * @param meta
   * @return Stats about the compaction (tuples migrated, extents freed).
   */
  CompactionStats compact_table(const TableMetadata& meta);

 private:
  struct PageStats {
    page_id_t id;
    uint16_t live_count;
    uint32_t free_space;
  };

  /**
   * @brief Collects per-page live tuple count and free space across the
   *        table's IAM chain without keeping any data page pinned.
   *
   * Walks the IAM chain, fetches each data page, counts slots with
   * length > 0, and records free_space. The caller sorts the resulting
   * vector before calling migrate_tuples().
   *
   * @param iam_head The IAM head page for the table.
   * @param table_name The table name (for log messages).
   * @param[out] pages Appended with one PageStats entry per data page.
   */
  void survey_pages(page_id_t iam_head, const std::string& table_name, std::vector<PageStats>& pages);

  /**
   * @brief Moves live tuples from sparse pages into denser pages.
   *
   * Iterates pages front-to-back (sparse first). For each live tuple,
   * searches backwards through the list for a destination with enough
   * free_space. Fetches both pages, inserts into destination, deletes
   * from source, unpins both dirty. Updates the destination's free_space
   * in the vector so subsequent searches use accurate data.
   *
   * @param[in,out] pages Sorted by live_count ascending; free_space fields
   *        are decremented as tuples are inserted.
   * @return Number of tuples successfully migrated.
   */
  int migrate_tuples(std::vector<PageStats>& pages);

  /**
   * @brief Walks the IAM chain and deallocates extents where all 8 pages
   *        are empty.
   *
   * For each extent, checks is_empty() on every page. When all 8 report
   * empty, calls IamManager::remove_extent_from_iam() followed by
   * ExtentManager::deallocate_extent().
   *
   * @param iam_head The IAM head page for the table.
   * @return Number of extents freed.
   */
  int free_empty_extents(page_id_t iam_head);

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
