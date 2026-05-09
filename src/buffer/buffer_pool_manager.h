#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <list>
#include <unordered_map>
#include <memory>
#include <optional>
#include "storage/config.h"
#include "storage/page.h"
#include "buffer/page_replacer.h"
#include "storage/disk_manager.h"

namespace letty {

/** @brief
 * Snapshot of buffer pool cache performance counters.
 * */
struct CacheStats {
  uint64_t hits             = 0;
  uint64_t misses           = 0;
  uint64_t evictions        = 0;
  uint64_t dirty_evictions  = 0;
  uint64_t flushes          = 0;

  double hit_ratio() const {
    uint64_t total = hits + misses;
    return total == 0 ? 0.0 : static_cast<double>(hits) / total * 100.0;
  }
};

/**
 * @class BufferPoolManager
 * @brief Manages a fixed-size pool of in-memory pages, serving as the single
 *        point of access for all page reads and writes.
 *
 * BufferPoolManager is the storage layer's access point for database page. It
 * fetches pages into memory, pins frames while callers use them, tracks dirty
 * pages, and writes dirty frames back on eviction, flush, checkpoint, or
 * destruction.
 * Callers must unpin every page returned by fetch_page() or new_page(). A page
 * whose pin count is nonzero is not evictable. When a caller modifies page
 * bytes, it must pass is_dirty=true to unpin_page().
 * All storage components (ExtentManager, IamManager, CatalogManager, TableManager)
 * access pages through the BufferPoolManager instead of calling DiskManager directly.
 *
 * Thread safety: All public methods are protected by an internal mutex.
 */
class BufferPoolManager {
 public:
  BufferPoolManager(IDiskManager& disk_manager, size_t pool_size,
                    std::unique_ptr<PageReplacer> replacer); // We can inject different implementation of PageReplacer

  /**
   * Flush all dirty pages to disk.
   */
  ~BufferPoolManager();

  BufferPoolManager(const BufferPoolManager&) = delete;
  BufferPoolManager& operator=(const BufferPoolManager&) = delete;

  /**
   * @brief Fetch a page from the pool. Reads from disk on cache miss.
   * @param page_id id of page to be fetched. If the page is not in the cache it is read from disk using DiskManager
   * @return Pointer to the Page, or nullptr if pool is full and all frames are pinned.
   *         Caller MUST call unpin_page when done.
   */
  Page* fetch_page(page_id_t page_id);

  /**
   * @brief Unpin a page allowing it to be evicted if the page cache is full.
   * @param page_id The page to unpin.
   * @param is_dirty If true, marks the page as dirty (modified).
   * @return false if the page is not in the pool.
   * Note: If the page is marked as dirty when it is being evicted it is first written to the disk
   */
  bool unpin_page(page_id_t page_id, bool is_dirty);

  /**
   * @brief Register a new page in the pool that does not yet exist on disk.
   *
   * The page ID is assigned by the caller. The returned frame is pinned
   * (pin_count=1), zeroed, and marked dirty immediately so the page will be
   * written to disk on flush or eviction.
   *
   * This function is strict: if page_id is already present in the buffer pool,
   * it returns nullptr rather than reusing or overwriting the existing frame.
   * Callers that are formatting a newly allocated logical page which may already
   * be cached because of file pre-extension should handle that case explicitly,
   * usually by falling back to fetch_page(page_id) and then writing the page
   * layout.
   *
   * @return Pointer to the Page, or nullptr if the page is already cached or no
   *         frame is available. Caller must unpin the page when done.
   */
  Page* new_page(page_id_t page_id);

  /**
   * @brief Write a dirty page to disk without unpinning or evicting it.
   * @return false if page is not in the pool.
   */
  bool flush_page(page_id_t page_id);

  /**
   * @brief Write all dirty pages in the pool to disk.
   * @return true if every dirty page was flushed successfully.
   *         false if one or more dirty pages failed to flush.
   */
  bool flush_all_pages();

  /**
   * @brief Flush all dirty pages and sync to stable storage.
   * @return true if all dirty pages were flushed and the disk sync succeeded.
   *         false if one or more dirty pages failed to flush, or sync failed.
   */
  bool checkpoint();

  /**
   * @brief Remove a page from the pool entirely.
   * @return false if page is not found or is currently pinned.
   */
  bool delete_page(page_id_t page_id);

  /** @brief Returns the current database file size in pages.
   *
   **/
  inline page_id_t get_file_size_in_pages() {
	return disk_manager_.get_file_size_in_pages();
  }

  /** @brief Returns the configured pool size. */
  size_t get_pool_size() const { return pool_size_; }

  /** @brief Returns a snapshot of cache performance stats. */
  CacheStats get_cache_stats() const;

  /** @brief Resets cache stat counters to zero. */
  void reset_cache_stats();

 private:
  IDiskManager& disk_manager_;
  size_t pool_size_;

  // PAGE_SIZE-aligned slab. Frame i occupies page_data_buffer_ + (i * PAGE_SIZE).
  // Each frame is exactly one database page and starts at a page-aligned address,
  // which keeps the in-memory layout consistent with the disk page size.
  // Freed in destructor via std::free.
  char* page_data_buffer_ = nullptr;
  std::vector<Page> pages_;

  // Runtime mapping of page_id → frame_id. Tells the BPM which frame currently
  // holds a given page. A page can land in any frame — there is no fixed formula.
  // On eviction the entry is removed; on fetch/new_page it is inserted.
  std::unordered_map<page_id_t, frame_id_t> page_table_;
  std::list<frame_id_t> free_list_;
  std::unique_ptr<PageReplacer> replacer_;
  std::mutex latch_;

  /**
   * @brief Acquires a free frame, either from the free list or by evicting.
   * @pre Caller holds latch_.
   * @return A free frame ID, or std::nullopt if all frames are pinned.
   */
  std::optional<frame_id_t> acquire_frame();

  // Cache performance counters
  std::atomic<uint64_t> cache_hits_{0};
  std::atomic<uint64_t> cache_misses_{0};
  std::atomic<uint64_t> evictions_{0};
  std::atomic<uint64_t> dirty_evictions_{0};
  std::atomic<uint64_t> flushes_{0};
};

}
