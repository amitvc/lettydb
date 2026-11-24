#pragma once

#include <cassert>
#include <cstring>
#include <iostream>
#include "storage/config.h"
#include "storage/storage_def.h"

namespace letty {

class BufferPoolManager;

/**
 * @class Page
 * @brief Represents a single 4 KB page frame in the buffer pool.
 *
 * Every on-disk page in LettyDB has a fixed 20-byte PageHeader at byte 0
 * followed by a 4076-byte body (PAGE_BODY_SIZE) whose format depends on
 * PageHeader::page_type.
 *
 * @verbatim
 * Uniform 4 KB Page Layout (DEFAULT_PAGE_SIZE = 4096 bytes)
 *
 *  +-------------------------------------------------------------------+ 0
 *  | PageHeader (20 bytes — owned by Page)                             |
 *  |   page_lsn (8) | next_page_id (4) | prev_page_id (4)              |
 *  |   table_id (2) | page_type (1)    | reserved (1)                  |
 *  +-------------------------------------------------------------------+ 20  ← get_body()
 *  | Page Body (PAGE_BODY_SIZE = 4076 bytes)                           |
 *  |                                                                   |
 *  |   PageType::DATABASE_HEADER (Page 0):                             |
 *  |     DatabaseMetadata (signature, version, free_list_head, ...)    |
 *  |                                                                   |
 *  |   PageType::DATA (Heap / Catalog Tables):                         |
 *  |     SlottedPage { SlottedPageHeader (4B) -> Slots -> Tuples }     |
 *  |                                                                   |
 *  |   PageType::INDEX (B+Tree Nodes):                                 |
 *  |     B+Tree internal / leaf node payload                           |
 *  |                                                                   |
 *  |   PageType::FREE (Freelist):                                      |
 *  |     Unused; linked via PageHeader::next_page_id                   |
 *  +-------------------------------------------------------------------+ 4096
 * @endverbatim
 *
 * `Page` exposes `get_data()` (full 4 KB buffer for DiskManager I/O),
 * `get_body()` (the 4076-byte payload for `DatabaseMetadata`, `SlottedPage`,
 * etc.), and direct accessors for all `PageHeader` fields. Payload classes
 * never read or modify `PageHeader` directly.
 *
 * Memory model: A `Page` stores only frame metadata and a pointer into
 * BufferPoolManager's contiguous DEFAULT_PAGE_SIZE-aligned slab. Keeping the
 * 4 KB buffer outside `Page` avoids inflating `sizeof(Page)` to 8192 bytes due
 * to alignment rounding.
 */
class Page {
 public:
  Page() = default;

  /** @brief Returns a mutable pointer to the raw 4 KB page buffer (header + body). */
  char* get_data() {
    assert(data_ != nullptr && "Page data_ is null — Page must be used through BufferPoolManager");
    return data_;
  }

  /** @brief Returns a mutable pointer to the page body (after PageHeader, PAGE_BODY_SIZE bytes). */
  char* get_body() {
    assert(data_ != nullptr && "Page data_ is null — Page must be used through BufferPoolManager");
    return data_ + PAGE_HEADER_SIZE;
  }

  /** @brief Returns a read-only pointer to the page body (after PageHeader, PAGE_BODY_SIZE bytes). */
  const char* get_body() const {
    assert(data_ != nullptr && "Page data_ is null — Page must be used through BufferPoolManager");
    return data_ + PAGE_HEADER_SIZE;
  }

  /** @brief Returns the page ID of this page, or INVALID_PAGE_ID if empty. */
  page_id_t get_page_id() const { return page_id_; }

  /** @brief Returns the current pin count (number of active users). */
  int get_pin_count() const { return pin_count_; }

  /** @brief Returns true if the in-memory content differs from what is on disk. */
  bool is_dirty() const { return is_dirty_; }

  PageHeader get_header() const {
    assert(data_ != nullptr);
    PageHeader header{};
    std::memcpy(&header, data_, PAGE_HEADER_SIZE);
    return header;
  }

  void set_header(const PageHeader& header) {
    assert(data_ != nullptr);
    std::memcpy(data_, &header, PAGE_HEADER_SIZE);
  }

  lsn_t get_page_lsn() const {
    return get_header().page_lsn;
  }

  void set_page_lsn(lsn_t lsn) {
    PageHeader header = get_header();
    header.page_lsn = lsn;
    set_header(header);
  }

  page_id_t get_next_page_id() const {
    return get_header().next_page_id;
  }

  void set_next_page_id(page_id_t next_page_id) {
    PageHeader header = get_header();
    header.next_page_id = next_page_id;
    set_header(header);
  }

  page_id_t get_prev_page_id() const {
    return get_header().prev_page_id;
  }

  void set_prev_page_id(page_id_t prev_page_id) {
    PageHeader header = get_header();
    header.prev_page_id = prev_page_id;
    set_header(header);
  }

  uint16_t get_table_id() const {
    return get_header().table_id;
  }

  void set_table_id(uint16_t table_id) {
    PageHeader header = get_header();
    header.table_id = table_id;
    set_header(header);
  }

  PageType get_page_type() const {
    return get_header().page_type;
  }

  void set_page_type(PageType type) {
    PageHeader header = get_header();
    header.page_type = type;
    set_header(header);
  }

 private:
  // Only BPM can modify the Page metadata since all modifiers functions are kept private.
  friend class BufferPoolManager;

  /** @brief Resets the page to its initial empty state. Zeroes all data. */
  void reset() {
    std::memset(data_, 0, DEFAULT_PAGE_SIZE);
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
