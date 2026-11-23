#pragma once

#include "storage/config.h"

namespace letty {

class Page;
class BufferPoolManager;

/**
 * @class PageGuard
 * @brief Move-only RAII guard for pages pinned in the BufferPoolManager.
 *
 * Ensures that a pinned Page is automatically unpinned via
 * `BufferPoolManager::unpin_page()` when the guard goes out of scope or when
 * `drop()` is called explicitly.
 *
 * Starts with `is_dirty = false`. Accessing page data via `get_data()` does not
 * mark the page dirty; callers that modify the page must call `mark_dirty()`
 * after the modification succeeds.
 */
class PageGuard {
 public:
  /** @brief Constructs an empty, invalid guard that holds no page. */
  PageGuard();

  /**
   * @brief Constructs a guard managing an already-pinned page.
   * @param bpm The BufferPoolManager that owns the frame.
   * @param page The pinned Page pointer (or nullptr for an invalid guard).
   */
  PageGuard(BufferPoolManager* bpm, Page* page);

  /** @brief Unpins the managed page (if valid) with its current dirty status. */
  ~PageGuard();

  PageGuard(const PageGuard&) = delete;
  PageGuard& operator=(const PageGuard&) = delete;

  /** @brief Transfers ownership of the pinned page from `other` to this guard. */
  PageGuard(PageGuard&& other) noexcept;

  /**
   * @brief Unpins any currently held page and transfers ownership from `other`.
   * @param other The guard to move from (left in an invalid state).
   * @return Reference to this guard.
   */
  PageGuard& operator=(PageGuard&& other) noexcept;

  /**
   * @brief Explicitly unpins the managed page and resets the guard to invalid.
   *
   * Safe to call multiple times (idempotent).
   */
  void drop();

  /** @brief Returns true if this guard holds a valid pinned page. */
  bool is_valid() const;

  /** @brief Boolean conversion shorthand for `is_valid()`. */
  explicit operator bool() const;

  /** @brief Returns the page ID of the managed page. */
  page_id_t get_page_id() const;

  /** @brief Returns a mutable pointer to the 4 KB page buffer without marking dirty. */
  char* get_data();

  /** @brief Returns a read-only pointer to the 4 KB page buffer. */
  const char* get_data() const;

  /** @brief Returns a mutable pointer to the page body (after PageHeader). */
  char* get_body();

  /** @brief Returns a read-only pointer to the page body (after PageHeader). */
  const char* get_body() const;

  /** @brief Marks the managed page as dirty in the BufferPoolManager immediately. */
  void mark_dirty();

  /** @brief Returns true if the managed page is currently marked dirty in the BufferPoolManager. */
  bool is_dirty() const;

  /** @brief Returns the underlying mutable Page pointer. */
  Page* get_page();

  /** @brief Returns the underlying read-only Page pointer. */
  const Page* get_page() const;

 private:
  BufferPoolManager* bpm_{nullptr};
  Page* page_{nullptr};
};

}
