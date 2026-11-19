
#pragma once

#include "buffer/buffer_pool_manager.h"
#include "buffer/page_guard.h"
#include "config.h"
#include "slotted_page.h"
#include "storage_def.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <optional>
#include <type_traits>

namespace letty {

inline void write_le16(Byte* dest, uint16_t value) {
  dest[0] = static_cast<Byte>(value & 0xFF);
  dest[1] = static_cast<Byte>((value >> 8) & 0xFF);
}

inline uint16_t read_le16(const Byte* src) {
  return static_cast<uint16_t>(src[0]) |
      (static_cast<uint16_t>(src[1]) << 8);
}

inline void write_le32(Byte* dest, uint32_t value) {
  dest[0] = static_cast<Byte>(value & 0xFF);
  dest[1] = static_cast<Byte>((value >> 8) & 0xFF);
  dest[2] = static_cast<Byte>((value >> 16) & 0xFF);
  dest[3] = static_cast<Byte>((value >> 24) & 0xFF);
}

inline uint32_t read_le32(const Byte* src) {
  return static_cast<uint32_t>(src[0]) |
      (static_cast<uint32_t>(src[1]) << 8) |
      (static_cast<uint32_t>(src[2]) << 16) |
      (static_cast<uint32_t>(src[3]) << 24);
}

inline void write_le64(Byte* dest, uint64_t value) {
  dest[0] = static_cast<Byte>(value & 0xFF);
  dest[1] = static_cast<Byte>((value >> 8) & 0xFF);
  dest[2] = static_cast<Byte>((value >> 16) & 0xFF);
  dest[3] = static_cast<Byte>((value >> 24) & 0xFF);
  dest[4] = static_cast<Byte>((value >> 32) & 0xFF);
  dest[5] = static_cast<Byte>((value >> 40) & 0xFF);
  dest[6] = static_cast<Byte>((value >> 48) & 0xFF);
  dest[7] = static_cast<Byte>((value >> 56) & 0xFF);
}

inline uint64_t read_le64(const Byte* src) {
  return static_cast<uint64_t>(src[0]) |
      (static_cast<uint64_t>(src[1]) << 8) |
      (static_cast<uint64_t>(src[2]) << 16) |
      (static_cast<uint64_t>(src[3]) << 24) |
      (static_cast<uint64_t>(src[4]) << 32) |
      (static_cast<uint64_t>(src[5]) << 40) |
      (static_cast<uint64_t>(src[6]) << 48) |
      (static_cast<uint64_t>(src[7]) << 56);
}

/**
 * @brief Computes standard IEEE 802.3 CRC32 checksum over a byte buffer.
 * @param data Pointer to the input bytes.
 * @param length Number of bytes to checksum.
 * @return 32-bit CRC checksum.
 */
inline uint32_t compute_crc32(const Byte* data, size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

inline SlottedPageHeader load_header_from_bytes(const Byte* data) {
  SlottedPageHeader value{};
  value.page_lsn = static_cast<lsn_t>(read_le64(data));
  value.next_page_id = static_cast<page_id_t>(read_le32(data + 8));
  value.prev_page_id = static_cast<page_id_t>(read_le32(data + 12));
  value.num_slots = read_le16(data + 16);
  value.free_space_pointer = read_le16(data + 18);
  return value;
}

inline void store_header_to_bytes(Byte* data, const SlottedPageHeader& value) {
  write_le64(data, static_cast<uint64_t>(value.page_lsn));
  write_le32(data + 8, static_cast<uint32_t>(value.next_page_id));
  write_le32(data + 12, static_cast<uint32_t>(value.prev_page_id));
  write_le16(data + 16, value.num_slots);
  write_le16(data + 18, value.free_space_pointer);
}

inline Slot load_slot_from_bytes(const Byte* data) {
  Slot value{};
  value.offset = read_le16(data);
  value.length = read_le16(data + 2);
  return value;
}

inline void store_slot_to_bytes(Byte* data, const Slot& value) {
  write_le16(data, value.offset);
  write_le16(data + 2, value.length);
}

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

template <> inline GAMPage load_page_layout<GAMPage>(Page* page, size_t offset) {
  assert(page != nullptr);
  assert(offset + sizeof(GAMPage) <= PAGE_SIZE);

  const Byte* data = reinterpret_cast<const Byte*>(page->get_data()) + offset;

  GAMPage value{};
  value.page_lsn = static_cast<lsn_t>(read_le64(data));
  value.next_page_id = static_cast<page_id_t>(read_le32(data + 8));
  value.first_free_bit_hint = read_le16(data + 12);
  std::memcpy(value.bitmap, data + 14, GAM_BITMAP_ARRAY_SIZE);
  return value;
}

template <> inline void store_page_layout<GAMPage>(Page* page, const GAMPage& value, size_t offset) {
  assert(page != nullptr);
  assert(offset + sizeof(GAMPage) <= PAGE_SIZE);

  Byte* data = reinterpret_cast<Byte*>(page->get_data()) + offset;

  write_le64(data, static_cast<uint64_t>(value.page_lsn));
  write_le32(data + 8, static_cast<uint32_t>(value.next_page_id));
  write_le16(data + 12, value.first_free_bit_hint);
  std::memcpy(data + 14, value.bitmap, GAM_BITMAP_ARRAY_SIZE);
}


template <> inline IAMPage load_page_layout<IAMPage>(Page* page, size_t offset) {
  assert(page != nullptr);
  assert(offset + sizeof(IAMPage) <= PAGE_SIZE);

  const Byte* data = reinterpret_cast<const Byte*>(page->get_data()) + offset;

  IAMPage value{};
  value.page_lsn = static_cast<lsn_t>(read_le64(data));
  value.next_page_id = static_cast<page_id_t>(read_le32(data + 8));
  value.extent_count = read_le16(data + 12);
  value.reserved = read_le16(data + 14);
  for (size_t i = 0; i < value.extent_count; ++i) {
    value.extent_ids[i] = read_le32(data + 16 + i * sizeof(uint32_t));
  }
  return value;
}

template <> inline void store_page_layout<IAMPage>(Page* page, const IAMPage& value, size_t offset) {
  assert(page != nullptr);
  assert(offset + sizeof(IAMPage) <= PAGE_SIZE);

  Byte* data = reinterpret_cast<Byte*>(page->get_data()) + offset;

  write_le64(data, static_cast<uint64_t>(value.page_lsn));
  write_le32(data + 8, static_cast<uint32_t>(value.next_page_id));
  write_le16(data + 12, value.extent_count);
  write_le16(data + 14, value.reserved);
  for (size_t i = 0; i < value.extent_count; ++i) {
    write_le32(data + 16 + i * sizeof(uint32_t), value.extent_ids[i]);
  }
}

template <> inline DatabaseHeader load_page_layout<DatabaseHeader>(Page* page, size_t offset) {
  assert(page != nullptr);
  assert(offset + sizeof(DatabaseHeader) <= PAGE_SIZE);

  const Byte* data = reinterpret_cast<const Byte*>(page->get_data()) + offset;

  DatabaseHeader value{};
  std::memcpy(value.signature, data, sizeof(value.signature));
  value.version = read_le32(data + 8);
  value.page_size = read_le32(data + 12);
  value.gam_page_id = static_cast<page_id_t>(read_le32(data + 16));
  value.page_lsn = static_cast<lsn_t>(read_le64(data + 20));
  value.sys_tables_iam_page = static_cast<page_id_t>(read_le32(data + 28));
  value.sys_columns_iam_page = static_cast<page_id_t>(read_le32(data + 32));
  value.next_table_oid = read_le16(data + 36);
  std::memcpy(value.padding, data + 38, sizeof(value.padding));
  return value;
}

template <> inline void store_page_layout<DatabaseHeader>(Page* page, const DatabaseHeader& value, size_t offset) {
  assert(page != nullptr);
  assert(offset + sizeof(DatabaseHeader) <= PAGE_SIZE);

  Byte* data = reinterpret_cast<Byte*>(page->get_data()) + offset;

  std::memcpy(data, value.signature, sizeof(value.signature));
  write_le32(data + 8, value.version);
  write_le32(data + 12, value.page_size);
  write_le32(data + 16, static_cast<uint32_t>(value.gam_page_id));
  write_le64(data + 20, static_cast<uint64_t>(value.page_lsn));
  write_le32(data + 28, static_cast<uint32_t>(value.sys_tables_iam_page));
  write_le32(data + 32, static_cast<uint32_t>(value.sys_columns_iam_page));
  write_le16(data + 36, value.next_table_oid);
  std::memcpy(data + 38, value.padding, sizeof(value.padding));
}


template <> inline SlottedPageHeader load_page_layout<SlottedPageHeader>(Page* page, size_t offset) {
  assert(page != nullptr);
  assert(offset + sizeof(SlottedPageHeader) <= PAGE_SIZE);
  return load_header_from_bytes(reinterpret_cast<const Byte*>(page->get_data()) + offset);
}

template <> inline void store_page_layout<SlottedPageHeader>(Page* page, const SlottedPageHeader& value, size_t offset) {
  assert(page != nullptr);
  assert(offset + sizeof(SlottedPageHeader) <= PAGE_SIZE);
  store_header_to_bytes(reinterpret_cast<Byte*>(page->get_data()) + offset, value);
}

template <> inline Slot load_page_layout<Slot>(Page* page, size_t offset) {
  assert(page != nullptr);
  assert(offset + sizeof(Slot) <= PAGE_SIZE);
  return load_slot_from_bytes(reinterpret_cast<const Byte*>(page->get_data()) + offset);
}

template <> inline void store_page_layout<Slot>(Page* page, const Slot& value, size_t offset) {
  assert(page != nullptr);
  assert(offset + sizeof(Slot) <= PAGE_SIZE);
  store_slot_to_bytes(reinterpret_cast<Byte*>(page->get_data()) + offset, value);
}

/**
 * @brief Reads an on-disk struct from a PageGuard buffer at the given offset.
 *
 * @tparam T The on-disk struct type to read.
 * @param guard The PageGuard wrapping the page.
 * @param offset Byte offset within the page (default 0).
 * @return A value-initialized T filled with the page's bytes.
 */
template <typename T>
T load_page_layout(const PageGuard& guard, size_t offset = 0) {
  assert(guard.is_valid());
  return load_page_layout<T>(const_cast<Page*>(guard.get_page()), offset);
}

/**
 * @brief Writes an on-disk struct into a PageGuard buffer at the given offset.
 *
 * Automatically marks the PageGuard dirty.
 *
 * @tparam T The on-disk struct type to write.
 * @param guard The PageGuard wrapping the page.
 * @param value The struct whose bytes will be copied into the page.
 * @param offset Byte offset within the page (default 0).
 */
template <typename T>
void store_page_layout(PageGuard& guard, const T& value, size_t offset = 0) {
  assert(guard.is_valid());
  store_page_layout<T>(guard.get_page(), value, offset);
  guard.mark_dirty();
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
  PageGuard guard = buffer_pool.fetch_page_guard(page_id);
  if (!guard) {
    return std::nullopt;
  }

  return load_page_layout<T>(guard);
}

}
