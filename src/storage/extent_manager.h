#pragma once

#include "storage_def.h"
#include "buffer/buffer_pool_manager.h"
#include <mutex>

namespace letty {

// Forward declaration
struct DatabaseHeader;
/**
 * @class ExtentManager
 * @brief Manages allocation and deallocation of Extent's within the database file.
 * An *extent* is a contiguous group of pages treated as a single allocation unit. All pages within an Extent typically
 * belong to a single database entity (either table, index or system catalog). For letty the size of extent is 8 pages
 * (see EXTENT_SIZE in config.h)
 * The ExtentManager sits above the BufferPoolManager and below higher-level components.
 *
 * Thread-safety:
 *  - ExtentManager is thread-safe.
 *  - All public methods are internally synchronized.
 *  - A single mutex protects all extent allocation metadata
 *    (GAM pages, IAM pages, and related in-memory state).
 *
 * Locking model:
 *  - Coarse-grained locking is used to keep allocation logic simple
 *    and correct.
 *  - Only one extent allocation or deallocation may proceed at a time.
 *  - Lock ordering: ExtentManager::lock_ is acquired before any
 *    BufferPoolManager calls (which acquire their own internal latch).
 *
 * Responsibilities:
 *  - Allocate new extents.
 *  - Free extents when objects are dropped or truncated.
 *  - Track global extent usage via the Global Allocation Map (GAM)
 *
 * Reference:
 *  - GAM / IAM allocation strategy inspired by SQL Server internals
 *    https://tinyurl.com/32rhava7
 */
class ExtentManager {
 public:
  explicit ExtentManager(BufferPoolManager& buffer_pool);

  /**
   * @brief Allocates a new extent
   * @return PageId of the first page in the newly allocated extent.
   */
  page_id_t allocate_extent();

  /**
   * @brief Reclaims the extent
   * @param start_page_id The starting pageId of the extent that is being deallocated.
   * @return true if deallocation succeeded, false if the page ID was invalid or an error occurred.
   */
  bool deallocate_extent(page_id_t start_page_id);
 private:
  /**
   * @brief Finds a free extent in the GAM page, marks it allocated.
   * @param gam_page Pointer to the GAM page data.
   * @return The starting page ID of the allocated extent, or INVALID_PAGE_ID if no free extent found.
   */
  page_id_t allocate_extent_in_gam_page(GAMPage* gam_page);

  /**
   * @brief Helper to perform the actual allocation once a free bit is found.
   */
  page_id_t commit_extent_allocation(GAMPage* gam_page, size_t bit_index);
  /**
   * @brief Initializes a brand new database file.
   * This function is called by the constructor if it detects that the database
   * file is empty. It creates and writes the initial Header and GAM pages.
   */
  void initialize_new_db();

  BufferPoolManager& buffer_pool_;

  std::mutex lock_;
  // We keep track of the last known free GAM page so we don't need to scan each gam page to arrive at the free
  // gam page. This essentially removes the O(n) linear scan of all gam pages.
  page_id_t current_gam_page_id_ = FIRST_GAM_PAGE_ID;
  // The ordinal position (0-indexed) of current_gam_page_id_ in the GAM chain.
  // Note: This is very important as it is used for calculating page IDs when allocating extents.
  size_t current_gam_chain_index_ = 0;

  /**
   * @brief Advances the internal GAM cursor to the specified next page ID.
   * @param next_gam_id The ID of the next GAM page to inspect.
   */
  void advance_gam_cursor(page_id_t next_gam_id);

  /**
   * @brief Creates a new GAM page, links it to the current chain, and updates the cursor to point to it.
   * @return true if successful, false otherwise.
   */
  bool create_and_link_new_gam();

  static void print_database_header(const DatabaseHeader *header);
};
}
