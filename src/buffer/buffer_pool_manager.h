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

class BufferPoolManager; // Forward declaration

/**
 * @class PageGuard
 * @brief RAII handle for a page in the buffer pool. 
 *        Automatically unpins the page when it goes out of scope.
 */
class PageGuard {
 public:
  PageGuard() = default;

  /** @brief Constructor used by BufferPoolManager. */
  PageGuard(BufferPoolManager *bpm, Page *page, page_id_t page_id)
      : bpm_(bpm), page_(page), page_id_(page_id) {}

  /** @brief Destructor automatically calls drop() to unpin the page. */
  ~PageGuard() { drop(); }

  // Non-copyable, prevents multiple unpins for the same page
  PageGuard(const PageGuard &) = delete;
  PageGuard &operator=(const PageGuard &) = delete;

  /** @brief Move constructor transfers ownership of the page pin. */
  PageGuard(PageGuard &&other) noexcept;
  /** @brief Move assignment transfers ownership of the page pin. */
  PageGuard& operator=(PageGuard &&other) noexcept;

  /** @brief Returns pointer to the underlying page. */
  Page *get() const { return page_; }
  Page &operator*() const { return *page_; }
  Page *operator->() const { return page_; }
  explicit operator bool() const { return page_ != nullptr; }

  /** @brief Marks the page as dirty so it is written back on eviction. */
  void mark_dirty() { dirty_ = true; }

  /** @brief Manually releases the page pin before the guard goes out of scope. */
  void drop();

 private:
  BufferPoolManager *bpm_ = nullptr;
  Page *page_ = nullptr;
  page_id_t page_id_ = INVALID_PAGE_ID;
  bool dirty_ = false;
};

/** @brief Snapshot of buffer pool cache performance counters. */
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
 * All storage components (ExtentManager, IamManager, CatalogManager, TableManager)
 * access pages through the BufferPoolManager instead of calling DiskManager directly.
 * Pages are cached in memory and written back to disk only when evicted or flushed.
 *
 * Thread safety: All public methods are protected by an internal mutex.
 */
class BufferPoolManager {
 public:
  BufferPoolManager(IDiskManager& disk_manager, size_t pool_size,
                    std::unique_ptr<PageReplacer> replacer); // We can inject different implementation of PageReplacer

  /**
   * @brief
   * Flush all dirty pages to disk.
   */
  ~BufferPoolManager();

  // Non-copyable, non-movable
  BufferPoolManager(const BufferPoolManager&) = delete;
  BufferPoolManager& operator=(const BufferPoolManager&) = delete;

  /**
   * @brief Fetch a page from the pool. Reads from disk on cache miss.
   * @param page_id The page to fetch.
   * @return Pointer to the Page, or nullptr if pool is full and all frames are pinned.
   *         Caller MUST call unpin_page when done.
   */
  Page* fetch_page(page_id_t page_id);

  /**
   * @brief Unpin a page, allowing it to be evicted.
   * @param page_id The page to unpin.
   * @param is_dirty If true, marks the page as dirty (modified).
   * @return false if the page is not in the pool.
   */
  bool unpin_page(page_id_t page_id, bool is_dirty);

  /**
   * @brief Register a new page in the pool that does not yet exist on disk.
   * @param page_id The page ID assigned by the caller (from ExtentManager).
   * @return Pointer to the Page (pin_count=1, dirty=true), or nullptr if pool is full.
   */
  Page* new_page(page_id_t page_id);
  
  /**
   * @brief Fetch a page from the pool and return an RAII guard.
   * @param page_id The page to fetch.
   * @return PageGuard holding the page, or empty guard if fetch fails.
   */
  PageGuard fetch_page_guard(page_id_t page_id);

  /**
   * @brief Register a new page and return an RAII guard.
   * @param page_id The page ID assigned by the caller.
   * @return PageGuard holding the new page, or empty guard if creation fails.
   */
  PageGuard new_page_guard(page_id_t page_id);

  /**
   * @brief Write a dirty page to disk without unpinning or evicting it.
   * @return false if page is not in the pool.
   */
  bool flush_page(page_id_t page_id);

  /**
   * @brief
   * Write all dirty pages in the pool to disk.
   * */
  void flush_all_pages();

  /**
   * @brief Flush all dirty pages and sync to stable storage.
   * */
  void checkpoint();

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
  std::vector<Page> pages_;
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
