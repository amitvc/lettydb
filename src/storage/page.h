#pragma once

#include <cassert>
#include <cstring>
#include "storage/config.h"

namespace letty {

class BufferPoolManager;

/**
 * @class Page
 * @brief Represents a single frame(page) in the buffer pool.
 *
 * A Page holds a PAGE_SIZE buffer of raw data along with metadata that the
 * BufferPoolManager uses to manage caching, eviction, and dirty-page tracking.
 *
 */
class Page {
 public:
  Page() { std::memset(data_, 0, PAGE_SIZE); }

  /** @brief Returns a mutable pointer to the raw page data. */
  char* get_data() { return data_; }

  /** @brief Returns the page ID loaded in this frame, or INVALID_PAGE_ID if empty. */
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
    assert(pin_count_ > 0 && "decrement_pin called on page with pin_count <= 0"); // This is a bug if assert is true.
    --pin_count_;
  }

  // Raw page data buffer, aligned to PAGE_SIZE (4096 bytes).
  //
  // Why alignas(PAGE_SIZE)?
  //
  // We overlay on-disk structs onto this buffer via reinterpret_cast:
  //
  //   auto* header = reinterpret_cast<DatabaseHeader*>(page->get_data());
  //   page_id_t gam = header->gam_page_id;  // must read bytes 16-19
  //
  // Those structs use #pragma pack(1), so their fields sit at exact byte
  // offsets with no padding. For this cast to be correct and efficient, the
  // buffer must start at a well-known aligned address. alignas(PAGE_SIZE)
  // guarantees data_ begins at a multiple of 4096 (e.g., 0x7F00001000),
  // which means packed fields like a uint32_t at offset 16 land at address
  // 0x7F00001010 — naturally aligned for the CPU despite #pragma pack(1).
  //
  // Additional benefits:
  //  - O_DIRECT readiness: direct I/O requires buffers aligned to the
  //    filesystem block size (typically 4096). We don't use O_DIRECT today
  //    (we use fstream), but this makes the switch zero-effort later.
  //  - Cache-line / TLB efficiency: a 4096-aligned buffer maps cleanly to
  //    hardware page boundaries, so no cache line or TLB entry is split
  //    across two page buffers.
  //
  // Tradeoff: alignas(PAGE_SIZE) inflates sizeof(Page) from ~4105 to 8192
  // because the compiler must pad each Page so the next one in a
  // vector<Page> also starts at a 4096 boundary. For a 64-frame pool this
  // wastes ~256 KB — negligible for the safety guarantees it provides.
  //
  // See book/memory_layout.md for a detailed write-up with examples.
  alignas(PAGE_SIZE) char data_[PAGE_SIZE];
  page_id_t page_id_ = INVALID_PAGE_ID;
  int pin_count_ = 0; // When this > 0 then this page is being requested/used. When > 0 page cannot be evicted from bpm.
  bool is_dirty_ = false; // Page has been modified and is a candidate to be written to disk as some point
};

}
