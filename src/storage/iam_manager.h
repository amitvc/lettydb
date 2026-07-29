#pragma once

#include "buffer/buffer_pool_manager.h"
#include "storage_def.h"
#include "extent_manager.h"
#include <string>
#include <unordered_map>

namespace letty {

/**
 * @class IamManager
 * @brief Manages Index Allocation Maps (IAM) for tables using list-based extent tracking.
 *
 * Each table has an IAM page that stores a list of extent IDs belonging to that table.
 * IAM pages live in dedicated extents owned by their table: the chain head sits at
 * the first page of an extent and grows into the remaining pages before a new
 * extent is allocated. Usage within an IAM extent is append-only, so the tail
 * page's position encodes the allocation state — no directory or bitmap needed.
 *
 * All page access goes through BufferPoolManager. No direct DiskManager calls.
 */
class IamManager {
 public:
  IamManager(BufferPoolManager& buffer_pool, ExtentManager& extent_manager);

  /**
   * @brief Creates a new IAM chain for a table.
   * Allocates a dedicated extent and initializes its first page as an empty IAMPage.
   * The returned page ID is always extent-aligned.
   * @param table_name Table name for which the IAM chain is being created.
   * @return The Page ID of the new IAM head page.
   * @throws DbException if the extent cannot be allocated or the page initialized.
   */
  page_id_t create_iam_chain(const std::string &table_name);

  /**
   * @brief Allocates a data extent for a table and adds it to the IAM.
   * Gets a new extent from ExtentManager and appends its ID to the IAM list.
   *
   * @param iam_head_page_id The IAM page for the table.
   * @return The starting Page ID of the newly allocated extent.
   * @throws DbException if the extent cannot be allocated or recorded in the IAM chain.
   */
  page_id_t allocate_extent_for_table(page_id_t iam_head_page_id);

  /**
   * @brief Finds a data page with sufficient free space within a table's extents.
   *
   * Strategy (in order):
   *  1. Check the cached hint page for this table (O(1) common case).
   *  2. Scan forward within the hint's extent (up to 7 more pages).
   *  3. Full IAM chain scan across all extents (hint miss or no hint yet).
   *
   * Returns INVALID_PAGE_ID when all known pages are full — caller must allocate
   * a new extent via allocate_extent_for_table().
   *
   * @param iam_head_page_id The IAM head page for the table.
   * @param required_space Minimum free bytes needed (tuple size only — slot overhead added internally).
   * @return Page ID of a suitable page, or INVALID_PAGE_ID if none found.
   */
  page_id_t find_page_with_space(page_id_t iam_head_page_id, uint32_t required_space);

 private:
  BufferPoolManager& buffer_pool_;
  ExtentManager& extent_manager_;

  // Per-table hint: iam_head_page_id → last known data page with free space.
  // On INSERT, the hint is checked first (O(1)). On a hint miss, the hint's
  // extent is scanned forward, then the full IAM chain. Updated whenever a
  // page with space is found or a new extent is allocated.
  std::unordered_map<page_id_t, page_id_t> table_page_hints_;


  /** Check the cached hint page for space. Returns page ID or INVALID_PAGE_ID. */
  page_id_t find_hint_page(page_id_t iam_head_page_id, uint32_t total_needed);

  /** Scan forward within the hint's extent for a page with space. Returns page ID or INVALID_PAGE_ID. */
  page_id_t scan_extent_forward(page_id_t iam_head_page_id, page_id_t hint_page_id, uint32_t total_needed);

  /** Scans all extents in the IAM chain for a page with space. Used when hint misses
   *  and the hint's extent is exhausted. Returns page ID or INVALID_PAGE_ID. */
  page_id_t scan_iam_chain(page_id_t iam_head_page_id, uint32_t total_needed);

  // ——— shared helpers ———

  /**
   * @brief Fetches a page, checks if it has at least `required` free bytes, and unpins it.
   * @return true if the page has enough space.
   */
  bool page_has_space(page_id_t page_id, uint32_t required);

  /**
   * @brief Allocates a new IAM page, appends extent_id to it, and links it to
   *        prev_iam_page (if valid). Uses the next page of prev_iam_page's extent
   *        when one is free; otherwise allocates a fresh extent.
   * @return Page ID of the new IAM page.
   * @throws DbException if the new IAM page cannot be allocated, initialized, or linked.
   */
  page_id_t link_new_iam_page(page_id_t prev_iam_page, uint32_t extent_id);
};

}
