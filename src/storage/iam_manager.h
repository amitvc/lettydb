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
 * IAM pages are allocated from a shared metadata pool to minimize space waste.
 *
 * All page access goes through BufferPoolManager. No direct DiskManager calls.
 */
class IamManager {
 public:
  IamManager(BufferPoolManager& buffer_pool, ExtentManager& extent_manager);

  /**
   * @brief Creates a new IAM chain for a table.
   * Allocates a page from the metadata pool and initializes it as an empty IAMPage.
   * @return The Page ID of the new IAM page, or INVALID_PAGE_ID if failed.
   */
  page_id_t create_iam_chain();

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
   * Iterates through the extent list in the IAM and checks each page for space.
   *
   * @param iam_head_page_id The IAM page for the table.
   * @param required_space The minimum free space needed (tuple size + slot overhead).
   * @return The Page ID of a page with enough space, or INVALID_PAGE_ID if none found.
   */
  page_id_t find_page_with_space(page_id_t iam_head_page_id, uint32_t required_space);

  /**
   * @brief Allocates a page from the metadata pool.
   * Used for IAM pages and other metadata.
   * @return The Page ID of the allocated metadata page, or INVALID_PAGE_ID if failed.
   */
  page_id_t allocate_metadata_page();

  /**
   * @brief Initializes the first metadata pool extent.
   * Called during database bootstrap.
   * @param pool_page_id The page ID where the metadata pool should start.
   */
  void init_metadata_pool(page_id_t pool_page_id);

 private:
  BufferPoolManager& buffer_pool_;
  ExtentManager& extent_manager_;

  /** Cached from header page on first access. Never changes after init. */
  page_id_t metadata_pool_page_id_ = INVALID_PAGE_ID;

  /** Per-table hint: iam_head_page_id → last known page with free space.
   *  Avoids O(n) linear scan on every INSERT in the common (append) case. */
  std::unordered_map<page_id_t, page_id_t> last_page_hint_;

  /** Loads metadata_pool_page_id_ from header if not yet cached. */
  page_id_t get_metadata_pool_page_id();

  // ——— find_page_with_space decomposition ———

  /** Check the cached hint page for space. Returns page ID or INVALID_PAGE_ID. */
  page_id_t find_hint_page(page_id_t iam_head_page_id, uint32_t total_needed);

  /** Scan forward within the hint's extent for a page with space. Returns page ID or INVALID_PAGE_ID. */
  page_id_t scan_extent_forward(page_id_t iam_head_page_id, page_id_t hint_page_id, uint32_t total_needed);

  /** Full IAM chain scan for a page with space (first-insert path). Returns page ID or INVALID_PAGE_ID. */
  page_id_t scan_iam_chain(page_id_t iam_head_page_id, uint32_t total_needed);

  // ——— allocate_metadata_page decomposition ———

  /** Allocates a new pool extent and links it to the current one. Returns new pool page ID or INVALID_PAGE_ID. */
  page_id_t extend_metadata_pool(MetadataPoolDirectoryPage* current_header, page_id_t current_pool_page);
};

}
