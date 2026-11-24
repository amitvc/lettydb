#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include "buffer/buffer_pool_manager.h"
#include "log_manager.h"
#include "table_scanner.h"
#include "transaction/transaction_manager.h"
#include "tuple.h"
#include "catalog/catalog_manager.h"
#include "page_allocator.h"

namespace letty {

/**
 * @struct CompactionStats
 * @brief Statistics returned by TableManager::compact_table().
 */
struct CompactionStats {
  uint32_t tuples_migrated = 0;
  uint32_t pages_freed = 0;
};

/**
 * @class TableManager
 * @brief Manages data operations on user tables.
 *
 * TableManager provides a high-level interface for inserting and scanning
 * table data. It uses CatalogManager to get table metadata, PageAllocator
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
  TableManager(BufferPoolManager& buffer_pool, CatalogManager& catalog_manager,
               LogManager& log_manager, TransactionManager& txn_manager,
			   PageAllocator& page_allocator);

  /**
   * @brief Inserts a row into a table.
   *
   * @param table_name The name of the table.
   * @param tuple The tuple to insert.
   * @param txn_id The active transaction ID for WAL logging.
   * @return the slot ID where the tuple was placed, or std::nullopt on failure.
   * @throws DbException if the table is missing or the insert cannot complete.
   */
  std::optional<uint16_t> insert_row(const std::string& table_name, const Tuple& tuple, uint64_t txn_id);

  /**
   * @brief Inserts a row using pre-resolved metadata (avoids catalog lookup).
   *
   * @param meta The table's metadata (schema + IAM page).
   * @param tuple The tuple to insert.
   * @param txn_id The active transaction ID for WAL logging.
   * @return the slot ID where the tuple was placed, or std::nullopt on failure.
   * @throws DbException if the insert cannot complete.
   */
  std::optional<uint16_t> insert_row(const TableMetadata& meta, const Tuple& tuple, uint64_t txn_id);

  /**
   * @brief Inserts multiple rows into a table in a single batch.
   *
   * Performs a single catalog lookup and keeps pages pinned while filling
   * them with multiple tuples, reducing overhead significantly.
   *
   * @param table_name The name of the table.
   * @param tuples The tuples to insert.
   * @param txn_id The active transaction ID for WAL logging.
   * @return Number of rows successfully inserted.
   * @throws DbException if the table is missing or the batch insert cannot complete.
   */
  uint32_t insert_rows(const std::string& table_name, const std::vector<Tuple>& tuples, uint64_t txn_id);

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
   * @param txn_id The active transaction ID for WAL logging.
   * @return true if the tuple was live and is now deleted.
   *         false if the slot was already deleted or the page failed to load.
   * @throws DbException if the table is missing.
   */
  bool delete_row(const std::string& table_name, page_id_t page_id, uint16_t slot_id, uint64_t txn_id);

  /**
   * @brief Deletes a row using pre-resolved metadata (avoids catalog lookup).
   *
   * @param meta The table's metadata (schema + IAM page).
   * @param page_id The page ID containing the tuple.
   * @param slot_id The slot index of the tuple to delete.
   * @param txn_id The active transaction ID for WAL logging.
   * @return true if the tuple was live and is now deleted.
   *         false if the slot was already deleted or the page failed to load.
   */
  bool delete_row(const TableMetadata& meta, page_id_t page_id, uint16_t slot_id, uint64_t txn_id);


  /**
   * @brief Starts the process of table compaction.
   * @param tbl_meta_data
   * @return Stats about the compaction (tuples migrated, extents freed).
   */
  CompactionStats compact_table(const TableMetadata& tbl_meta_data);

 private:
  /**
   * In memory structure that tracks
   * how many slots are occupied for a given page.
   */
  struct PageStats {
    page_id_t page_id;
    uint16_t live_count;
    uint32_t free_space;
  };

  /**
   * @brief Collects per-page live tuple count and free space across the
   *        table's page chain without keeping any data page pinned.
   *
   * @param first_page_id The first data page ID for the table.
   * @param table_name The table name (for log messages).
   * @param[out] pages Appended with one PageStats entry per data page.
   */
  void survey_pages(page_id_t first_page_id, const std::string& table_name, std::vector<PageStats>& pages);

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
   * @brief Walks the table's page chain, unlinking and freeing empty pages
   *        while retaining at least one page for the table.
   *
   * @param tbl_meta_data The table's metadata.
   * @param txn_id Transaction the underlying page deallocations are logged under.
   * @return Number of pages freed.
   */
  int free_empty_pages(const TableMetadata& tbl_meta_data, uint64_t txn_id);

  BufferPoolManager& buffer_pool_;
  CatalogManager& catalog_manager_;
  LogManager& log_manager_;
  TransactionManager& txn_manager_;
  PageAllocator& page_allocator_;

  mutable std::mutex latches_map_mutex_;
  mutable std::unordered_map<uint32_t, std::mutex> table_latches_;

  std::mutex& get_table_latch(uint32_t table_id) const {
    std::lock_guard<std::mutex> map_lock(latches_map_mutex_);
    return table_latches_[table_id];
  }

  /**
   * @brief Returns a page that can accept `needed_space` bytes, allocating and
   *        linking a new page if the current tail page is full.
   */
  page_id_t acquire_page_for_insert(const TableMetadata& meta, uint32_t needed_space, uint64_t txn_id);

};

}
