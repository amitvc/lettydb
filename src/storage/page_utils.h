
#pragma once

#include "buffer/buffer_pool_manager.h"
#include "config.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <optional>
#include <type_traits>

namespace letty {

template <typename T>
T load_page_layout(Page* page, size_t offset = 0) {
  static_assert(std::is_trivially_copyable_v<T>);
  assert(page != nullptr);
  assert(offset + sizeof(T) <= PAGE_SIZE);

  T value{};
  std::memcpy(&value, page->get_data() + offset, sizeof(T));
  return value;
}

template <typename T>
void store_page_layout(Page* page, const T& value, size_t offset = 0) {
  static_assert(std::is_trivially_copyable_v<T>);
  assert(page != nullptr);
  assert(offset + sizeof(T) <= PAGE_SIZE);

  std::memcpy(page->get_data() + offset, &value, sizeof(T));
}

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
