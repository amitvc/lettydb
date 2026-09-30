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
 * @brief A fixed-size cache of in-memory database pages. The single point of
 *        access for all page reads and writes — no storage component may call
 *        DiskManager directly.
 *
 * **The pin/unpin model**
 *
 * Every page in the buffer pool has a **pin count**. A page with pin count > 0
 * is *in use* by some caller and must not be evicted. A page with pin count 0
 * is *evictable* — the replacer may choose it as a candidate when a new page needs
 * a frame.
 *
 * The lifecycle is always the same:
 *
 * ```
 * fetch_page(X)    → pin_count becomes 1 (page is "in use")
 * fetch_page(X)    → pin_count becomes 2 (another caller also needs it)
 * unpin_page(X)    → pin_count becomes 1 (still in use by first caller)
 * unpin_page(X)    → pin_count becomes 0 (now evictable)
 * ```
 *
 * **When does a page become dirty?**
 *
 * After calling `fetch_page()`, the caller may modify the page's bytes. When
 * done, `unpin_page(page_id, is_dirty)` tells the buffer pool whether the page
 * was modified. If `is_dirty` is true, the page must be written to disk before
 * its frame can be reused. This happens in two ways:
 *
 * 1. **Lazy eviction:** The replacer picks the dirty page as a candidate. Before
 *    reassigning the frame, the BPM calls `disk_manager_.write_page()`.
 * 2. **Explicit flush:** The caller asks for a flush via `flush_page()`,
 *    `flush_all_pages()`, or `force_flush()` — all write dirty pages to disk
 *    immediately, without evicting them.
 *
 * **Responsibilities of the callers**
 *
 * Every `fetch_page()` or `new_page()` returns a `Page*` with pin_count = 1.
 * The caller is responsible for exactly one matching `unpin_page()`. Forgetting
 * to unpin leaks the frame (it can never be evicted). Calling unpin twice
 * underflows the pin count (caught by `assert()` in debug builds).
 *
 * **Eviction order**
 *
 * When all frames are pinned and a new page must be loaded, `acquire_frame()`
 * delegates to the `PageReplacer` (typically LRU) to pick a candidate among
 * evictable pages. If no page is evictable, `fetch_page()` returns `nullptr`.
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
   * @brief Fetch a page from the pool, reading from disk on cache miss.
   *
   * If page_id is already cached, the existing frame is pinned (pin_count++)
   * and returned immediately. Otherwise, a free frame is acquired and the page
   * is read from disk.
   *
   * @param page_id The page to fetch.
   * @return Pointer to the Page, or nullptr if all frames are pinned.
   *         Caller MUST call unpin_page() when done.
   */
  Page* fetch_page(page_id_t page_id);

  /**
   * @brief Pin an existing cached page, or allocate a zeroed dirty frame for
   *        a new page without reading from disk.
   *
   * If page_id is already cached, the existing frame is pinned and returned.
   * Otherwise a new frame is allocated, zeroed, and marked dirty (it has no
   * on-disk counterpart yet). Use this when formatting a page that may not
   * exist on disk.
   *
   * @param page_id The page to pin or allocate.
   * @return Pointer to the pinned Page, or nullptr if all frames are pinned.
   *         Caller MUST call unpin_page() when done.
   */
  Page* new_or_fetch_page(page_id_t page_id);

  /**
   * @brief Release a page, decrementing its pin count.
   *
   * When pin count reaches 0 the page becomes evictable. If `is_dirty` is true
   * the page is marked for disk write before its frame can be reused.
   *
   * @param page_id The page to release.
   * @param is_dirty True if the caller modified the page's bytes.
   * @return false if page_id is not in the pool.
   */
  bool unpin_page(page_id_t page_id, bool is_dirty);

  /**
   * @brief Allocate a brand-new page in the buffer pool.
   *
   * The page does not yet exist on disk. A free frame is acquired, zeroed,
   * pinned (pin_count = 1), and marked dirty so it will be written on flush
   * or eviction.
   *
   * If page_id is already cached, returns nullptr — callers that may be racing
   * with another allocator should use new_or_fetch_page() instead.
   *
   * @param page_id The page ID to allocate.
   * @return Pointer to the pinned Page, or nullptr if page is already cached
   *         or all frames are pinned. Caller MUST call unpin_page() when done.
   */
  Page* new_page(page_id_t page_id);

  /**
   * @brief Write every dirty page to disk immediately, without evicting.
   *
   * Iterates all frames, writes each dirty page to disk, and clears its dirty
   * flag. Pages remain pinned and in the pool. Use this after operations that
   * must survive a crash (e.g., compaction extent deallocation).
   */
  void force_flush();

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
