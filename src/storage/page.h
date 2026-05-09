#pragma once

#include <cassert>
#include <cstring>
#include <iostream>
#include "storage/config.h"

namespace letty {

class BufferPoolManager;

/**
 * @class Page
 * @brief Represents a single page in the buffer pool.
 *
 * A Page holds metadata plus a pointer to a PAGE_SIZE buffer managed by the
 * BufferPoolManager. The data buffer is allocated separately as one large
 * page-aligned slab — this avoids the sizeof(Page) inflation that occurs when
 * alignas(PAGE_SIZE) is embedded in the struct (which would pad each Page to
 * 8192 bytes instead of ~24 bytes, wasting ~256 KB for a 64-frame pool).
 *
 * The BufferPoolManager allocates a contiguous, PAGE_SIZE-aligned buffer of
 * pool_size * PAGE_SIZE bytes and assigns each frame's data_ pointer into it.
 * Each frame starts on a database page boundary and is used as byte-oriented
 * storage for page layouts loaded from or stored to disk.
 */
class Page {
 public:
  Page() = default;

  /** @brief Returns a mutable pointer to the raw page data. */
  char* get_data() {
    assert(data_ != nullptr && "Page data_ is null — Page must be used through BufferPoolManager");
    return data_;
  }

  /** @brief Returns the page ID of this page, or INVALID_PAGE_ID if empty. */
  page_id_t get_page_id() const { return page_id_; }

  /** @brief Returns the current pin count (number of active users). */
  int get_pin_count() const { return pin_count_; }

  /** @brief Returns true if the in-memory content differs from what is on disk. */
  bool is_dirty() const { return is_dirty_; }

 private:
  // Only BPM can modify the Page metadata since all modifiers functions are kept private.
  friend class BufferPoolManager;

  /** @brief Resets the page to its initial empty state. Zeroes all data. */
  void reset() {
    std::memset(data_, 0, PAGE_SIZE);
    page_id_ = INVALID_PAGE_ID;
    pin_count_ = 0;
    is_dirty_ = false;
  }

  void set_page_id(page_id_t page_id) { page_id_ = page_id; }
  void set_dirty(bool dirty) { is_dirty_ = dirty; }
  void increment_pin() { ++pin_count_; }

  void decrement_pin() {
    assert(pin_count_ > 0 && "decrement_pin called on page with pin_count <= 0");
    --pin_count_;
  }

  char* data_ = nullptr;
  page_id_t page_id_ = INVALID_PAGE_ID;
  int pin_count_ = 0;
  bool is_dirty_ = false;
};

}
