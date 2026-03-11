#pragma once

#include "buffer/buffer_pool_manager.h"
#include "storage_def.h"
#include "extent_manager.h"
#include <unordered_map>

namespace letty {

/**
 * @class IamManager
 * @brief Manages Index Allocation Maps (IAM) for tables using list-based extent tracking.
 *
 * Each table has an IAM page that stores a list of extent IDs belonging to that table.
 * IAM pages are allocated from a shared extent to minimize space waste.
 *
 * All page access goes through BufferPoolManager. No direct DiskManager calls.
 */
class IamManager {
 public:
  IamManager(BufferPoolManager& buffer_pool, ExtentManager& extent_manager);

  /**
   * @brief Creates a new IAM chain for a table.
   * Allocates a page from the shared extent and initializes it as an empty IAMPage.
   * @param table_name Table name for which the IAM chain is being created.
   * @return The Page ID of the new IAM page, or INVALID_PAGE_ID if failed.
   */
  page_id_t create_iam_chain(const std::string &table_name);

  /**
   * @brief Allocates a data extent for a table and adds it to the IAM.
   * Gets a new extent from ExtentManager and appends its ID to the IAM list.
   *
   * @param iam_head_page_id The IAM page for the table.
   * @return The starting Page ID of the newly allocated extent, or INVALID_PAGE_ID if failed.
   */
  page_id_t allocate_extent_for_table(page_id_t iam_head_page_id);

  /**
   * @brief Finds a data page with sufficient free space within a table's extents.
   *
   * Strategy (in order):
   *  1. Check the cached hint page for this table.
   *  2. Scan forward within the hint's extent.
   *  3. Full IAM chain scan (first-insert path only).
   *
   * @param iam_head_page_id The IAM page for the table.
   * @param required_space The minimum free space needed (tuple size + slot overhead).
   * @return Page ID of a suitable page, or INVALID_PAGE_ID if none exists.
   *         INVALID_PAGE_ID means the caller must allocate a new extent.
   */
  page_id_t find_page_with_space(page_id_t iam_head_page_id, uint32_t required_space);

  /**
   * @brief Initializes the first shared extent.
   * Called during database bootstrap.
   * @param pool_page_id The page ID where the shared extent should start.
   */
  void init_shared_extent(page_id_t pool_page_id);

 private:
  BufferPoolManager& buffer_pool_;
  ExtentManager& extent_manager_;

  /** Cached from header page on first access. Never changes after init. */
  page_id_t shared_extent_page_id_ = INVALID_PAGE_ID;

  /** Per-table hint: iam_head_page_id → last known page with free space.
   *  Avoids O(n) linear scan on every INSERT in the common (append) case. */
  std::unordered_map<page_id_t, page_id_t> table_page_hints_;

  /** Loads shared_extent_page_id_ from header if not yet cached. */
  page_id_t get_shared_extent_page_id();

  // ——— find_page_with_space decomposition ———

  /** Check the cached hint page for space. Returns page ID or INVALID_PAGE_ID. */
  page_id_t find_hint_page(page_id_t iam_head_page_id, uint32_t total_needed);

  /** Scan forward within the hint's extent for a page with space. Returns page ID or INVALID_PAGE_ID. */
  page_id_t scan_extent_forward(page_id_t iam_head_page_id, page_id_t hint_page_id, uint32_t total_needed);

  /** Full IAM chain scan for a page with space (first-insert path). Returns page ID or INVALID_PAGE_ID. */
  page_id_t scan_iam_chain(page_id_t iam_head_page_id, uint32_t total_needed);

  // ——— allocate_metadata_page decomposition ———

  /**
   * @brief Allocates a page from the shared extent.
   * Used internally by create_iam_chain.
   * @return The Page ID of the allocated metadata page, or INVALID_PAGE_ID if failed.
   */
  page_id_t allocate_shared_page();

  /** Allocates a new pool extent and links it to the current one. Returns new pool page ID or INVALID_PAGE_ID. */
  page_id_t extend_shared_extent(SharedExtentDirectoryPage* current_header, page_id_t current_pool_page);

  // ——— shared helpers ———

  /**
   * @brief Fetches a page, checks if it has at least `required` free bytes, and unpins it.
   * @return true if the page has enough space.
   */
  bool page_has_space(page_id_t page_id, uint32_t required);

  /**
   * @brief Allocates a new IAM page from the shared extent, appends extent_id to it,
   *        and links it to prev_iam_page (if valid).
   * @return Page ID of the new IAM page, or INVALID_PAGE_ID on failure.
   */
  page_id_t link_new_iam_page(page_id_t prev_iam_page, uint32_t extent_id);
};

}
