
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

inline PageHeader load_page_header_from_bytes(const Byte* data) {
  const Byte* cursor = data;
  PageHeader value{};
  value.page_lsn = static_cast<lsn_t>(read_le64(cursor));
  cursor += sizeof(uint64_t);

  value.next_page_id = static_cast<page_id_t>(read_le32(cursor));
  cursor += sizeof(uint32_t);

  value.prev_page_id = static_cast<page_id_t>(read_le32(cursor));
  cursor += sizeof(uint32_t);

  value.table_id = read_le16(cursor);
  cursor += sizeof(uint16_t);

  value.page_type = static_cast<PageType>(*cursor);
  cursor += sizeof(uint8_t);

  value.status = static_cast<PageStatus>(*cursor);
  return value;
}

inline void store_page_header_to_bytes(Byte* data, const PageHeader& value) {
  Byte* cursor = data;
  write_le64(cursor, static_cast<uint64_t>(value.page_lsn));
  cursor += sizeof(uint64_t);

  write_le32(cursor, static_cast<uint32_t>(value.next_page_id));
  cursor += sizeof(uint32_t);

  write_le32(cursor, static_cast<uint32_t>(value.prev_page_id));
  cursor += sizeof(uint32_t);

  write_le16(cursor, value.table_id);
  cursor += sizeof(uint16_t);

  *cursor = static_cast<Byte>(value.page_type);
  cursor += sizeof(uint8_t);

  *cursor = static_cast<Byte>(value.status);
}

inline SlotDirectoryHeader load_header_from_bytes(const Byte* data) {
  const Byte* cursor = data;
  SlotDirectoryHeader value{};
  value.num_slots = read_le16(cursor);
  cursor += sizeof(uint16_t);

  value.free_space_pointer = read_le16(cursor);
  return value;
}

inline void store_header_to_bytes(Byte* data, const SlotDirectoryHeader& value) {
  Byte* cursor = data;
  write_le16(cursor, value.num_slots);
  cursor += sizeof(uint16_t);

  write_le16(cursor, value.free_space_pointer);
}

inline Slot load_slot_from_bytes(const Byte* data) {
  const Byte* cursor = data;
  Slot value{};
  value.offset = read_le16(cursor);
  cursor += sizeof(uint16_t);

  value.length = read_le16(cursor);
  return value;
}

inline void store_slot_to_bytes(Byte* data, const Slot& value) {
  Byte* cursor = data;
  write_le16(cursor, value.offset);
  cursor += sizeof(uint16_t);

  write_le16(cursor, value.length);
}

/**
 * @brief Reads an on-disk struct from a page buffer at the given offset.
 *
 * Copies sizeof(T) bytes from the page into a local instance of T. This is the
 * safe way to read header-like structs (PageHeader, SlottedPageHeader, Slot,
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
  assert(offset + sizeof(T) <= DEFAULT_PAGE_SIZE);

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
  assert(offset + sizeof(T) <= DEFAULT_PAGE_SIZE);

  std::memcpy(page->get_data() + offset, &value, sizeof(T));
}

template <> inline PageHeader load_page_layout<PageHeader>(Page* page, size_t offset) {
  assert(page != nullptr);
  assert(offset + sizeof(PageHeader) <= DEFAULT_PAGE_SIZE);
  return load_page_header_from_bytes(reinterpret_cast<const Byte*>(page->get_data()) + offset);
}

template <> inline void store_page_layout<PageHeader>(Page* page, const PageHeader& value, size_t offset) {
  assert(page != nullptr);
  assert(offset + sizeof(PageHeader) <= DEFAULT_PAGE_SIZE);
  store_page_header_to_bytes(reinterpret_cast<Byte*>(page->get_data()) + offset, value);
}

template <> inline SlotDirectoryHeader load_page_layout<SlotDirectoryHeader>(Page* page, size_t offset) {
  assert(page != nullptr);
  assert(offset + sizeof(SlotDirectoryHeader) <= DEFAULT_PAGE_SIZE);
  return load_header_from_bytes(reinterpret_cast<const Byte*>(page->get_data()) + offset);
}

template <> inline void store_page_layout<SlotDirectoryHeader>(Page* page, const SlotDirectoryHeader& value, size_t offset) {
  assert(page != nullptr);
  assert(offset + sizeof(SlotDirectoryHeader) <= DEFAULT_PAGE_SIZE);
  store_header_to_bytes(reinterpret_cast<Byte*>(page->get_data()) + offset, value);
}

template <> inline Slot load_page_layout<Slot>(Page* page, size_t offset) {
  assert(page != nullptr);
  assert(offset + sizeof(Slot) <= DEFAULT_PAGE_SIZE);
  return load_slot_from_bytes(reinterpret_cast<const Byte*>(page->get_data()) + offset);
}

template <> inline void store_page_layout<Slot>(Page* page, const Slot& value, size_t offset) {
  assert(page != nullptr);
  assert(offset + sizeof(Slot) <= DEFAULT_PAGE_SIZE);
  store_slot_to_bytes(reinterpret_cast<Byte*>(page->get_data()) + offset, value);
}

/** @brief Reads a body payload struct starting at PAGE_HEADER_SIZE (Page::get_body()). */
template <typename T>
T load_page_body(Page* page) {
  return load_page_layout<T>(page, PAGE_HEADER_SIZE);
}

/** @brief Writes a body payload struct starting at PAGE_HEADER_SIZE (Page::get_body()). */
template <typename T>
void store_page_body(Page* page, const T& value) {
  store_page_layout<T>(page, value, PAGE_HEADER_SIZE);
}

template <> inline DatabaseMetadata load_page_body<DatabaseMetadata>(Page* page) {
  assert(page != nullptr);
  const Byte* cursor = reinterpret_cast<const Byte*>(page->get_body());

  DatabaseMetadata value{};
  std::memcpy(value.signature, cursor, sizeof(value.signature));
  cursor += sizeof(value.signature);

  value.version = *cursor;
  cursor += sizeof(uint8_t);

  value.page_size = read_le32(cursor);
  cursor += sizeof(uint32_t);

  value.table_count = read_le16(cursor);
  cursor += sizeof(uint16_t);

  value.next_table_id = read_le16(cursor);
  cursor += sizeof(uint16_t);

  value.total_pages = read_le32(cursor);
  cursor += sizeof(uint32_t);

  value.free_page_list_head_page = static_cast<page_id_t>(read_le32(cursor));
  cursor += sizeof(uint32_t);

  value.sys_tables_first_page = static_cast<page_id_t>(read_le32(cursor));
  cursor += sizeof(uint32_t);

  value.sys_columns_first_page = static_cast<page_id_t>(read_le32(cursor));
  return value;
}

template <> inline void store_page_body<DatabaseMetadata>(Page* page, const DatabaseMetadata& value) {
  assert(page != nullptr);
  Byte* cursor = reinterpret_cast<Byte*>(page->get_body());

  std::memcpy(cursor, value.signature, sizeof(value.signature));
  cursor += sizeof(value.signature);

  *cursor = value.version;
  cursor += sizeof(uint8_t);

  write_le32(cursor, value.page_size);
  cursor += sizeof(uint32_t);

  write_le16(cursor, value.table_count);
  cursor += sizeof(uint16_t);

  write_le16(cursor, value.next_table_id);
  cursor += sizeof(uint16_t);

  write_le32(cursor, value.total_pages);
  cursor += sizeof(uint32_t);

  write_le32(cursor, static_cast<uint32_t>(value.free_page_list_head_page));
  cursor += sizeof(uint32_t);

  write_le32(cursor, static_cast<uint32_t>(value.sys_tables_first_page));
  cursor += sizeof(uint32_t);

  write_le32(cursor, static_cast<uint32_t>(value.sys_columns_first_page));
}

/** @brief Reads a body payload struct from a PageGuard starting at PAGE_HEADER_SIZE. */
template <typename T>
T load_page_body(const PageGuard& guard) {
  assert(guard.is_valid());
  return load_page_body<T>(const_cast<Page*>(guard.get_page()));
}

/** @brief Writes a body payload struct to a PageGuard starting at PAGE_HEADER_SIZE and marks dirty. */
template <typename T>
void store_page_body(PageGuard& guard, const T& value) {
  assert(guard.is_valid());
  store_page_body<T>(guard.get_page(), value);
  guard.mark_dirty();
}

/** @brief Fetches a page, reads its body payload struct, and unpins. */
template <typename T>
std::optional<T> load_page_body_value(BufferPoolManager& buffer_pool, page_id_t page_id) {
  PageGuard guard = buffer_pool.fetch_page_guard(page_id);
  if (!guard) {
    return std::nullopt;
  }
  return load_page_body<T>(guard);
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
