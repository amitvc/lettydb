
#pragma once

#include "buffer/buffer_pool_manager.h"
#include "config.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <optional>
#include <type_traits>

namespace letty {

/**
 * @brief Reads an on-disk struct from a page buffer at the given offset.
 *
 * Copies sizeof(T) bytes from the page into a local instance of T. This is the
 * safe way to read header-like structs (SlottedPageHeader, IAMPage, GAMPage,
 * etc.) without relying on pointer casts into the buffer.
 *
 * @tparam T The on-disk struct type to read. Must be trivially copyable.
 * @param page The pinned page to read from.
 * @param offset Byte offset within the page (default 0).
 * @return A value-initialized T filled with the page's bytes.
 */
template <typename T>
T load_page_layout(Page* page, size_t offset = 0) {
  static_assert(std::is_trivially_copyable_v<T>);
  assert(page != nullptr);
  assert(offset + sizeof(T) <= PAGE_SIZE);

  T value{};
  std::memcpy(&value, page->get_data() + offset, sizeof(T));
  return value;
}

/**
 * @brief Writes an on-disk struct into a page buffer at the given offset.
 *
 * Copies the raw bytes of value into the page. The caller is responsible for
 * marking the page dirty in BufferPoolManager so the changes survive eviction.
 *
 * @tparam T The on-disk struct type to write. Must be trivially copyable.
 * @param page The pinned page to write to.
 * @param value The struct whose bytes will be copied into the page.
 * @param offset Byte offset within the page (default 0).
 */
template <typename T>
void store_page_layout(Page* page, const T& value, size_t offset = 0) {
  static_assert(std::is_trivially_copyable_v<T>);
  assert(page != nullptr);
  assert(offset + sizeof(T) <= PAGE_SIZE);

  std::memcpy(page->get_data() + offset, &value, sizeof(T));
}

/**
 * @brief Fetches a page, reads a trivially copyable struct from it, and unpins.
 *
 * Convenience helper that combines fetch_page + load_page_layout + unpin_page.
 * Returns nullopt if the page cannot be fetched (e.g., out of range or pool
 * exhaustion). The page is always unpinned with is_dirty=false because this
 * is a read-only operation.
 *
 * @tparam T The on-disk struct type to read. Must be trivially copyable.
 * @param buffer_pool The buffer pool to fetch the page from.
 * @param page_id The page to fetch.
 * @return The loaded struct, or nullopt if the page could not be fetched.
 */
template <typename T>
std::optional<T> load_page_value(BufferPoolManager& buffer_pool, page_id_t page_id) {
  Page* frame = buffer_pool.fetch_page(page_id);
  if (!frame) {
    return std::nullopt;
  }

  T value = load_page_layout<T>(frame);
  buffer_pool.unpin_page(page_id, false);
  return value;
}

}
