#pragma once

#include "storage_def.h"
#include "config.h"
#include <optional>


namespace letty {

/**
 * @struct SlottedPageHeader
 * @brief Fixed-size header at the start of every slotted page.
 */
#pragma pack(push, 1)
struct SlottedPageHeader {
  /// Log Sequence Number. Reserved for WAL-based crash recovery.
  lsn_t lsn = 0;
  page_id_t next_page_id = INVALID_PAGE_ID;
  page_id_t prev_page_id = INVALID_PAGE_ID;
  /// Number of slot entries allocated, including deleted (tombstone) slots.
  uint16_t num_slots = 0;
  /// Offset to the start of tuple data. Decreases as tuples are appended.
  uint16_t free_space_pointer = PAGE_SIZE;
};

/**
 * @struct Slot
 * @brief A slot directory entry. Length of 0 indicates a deleted (reusable) slot.
 */
struct Slot {
  uint16_t offset;
  uint16_t length;
};
#pragma pack(pop)

/**
 * @class SlottedPage
 * @brief Wraps a raw page buffer to provide variable-length tuple storage.
 *
 * SlottedPage is a non-owning view. The caller owns the underlying buffer
 * (typically via BufferPoolManager) and is responsible for its lifetime.
 *
 * Layout (PAGE_SIZE bytes):
 *
 *  Offsets grow downward ↓
 *
 *  +--------------------------------------------------------------+ 0
 *  | SlottedPageHeader (fixed-size)                               |
 *  |  - lsn                                                       |
 *  |  - next_page_id / prev_page_id                               |
 *  |  - num_slots       (# slot entries allocated; includes holes)|
 *  |  - free_space_pointer (offset where free space begins)       |
 *  +--------------------------------------------------------------+  sizeof(Header)
 *  | Slot Directory (grows ↓ toward higher offsets)               |
 *  |  slot[0] -> (tuple_offset, tuple_length)                     |
 *  |  slot[1] -> (tuple_offset, tuple_length)                     |
 *  |  ...                                                        |
 *  |  slot[num_slots-1]                                           |
 *  +------------------------------ free space ---------------------+
 *  | Free Space (shrinks from both ends)                          |
 *  +--------------------------------------------------------------+  free_space_pointer
 *  | Tuple / Record Data Area (grows ↑ toward lower offsets)      |
 *  |  tuple bytes ...                                             |
 *  |  tuple bytes ...                                             |
 *  +--------------------------------------------------------------+  PAGE_SIZE
 *
 * Invariants:
 *  - Slot directory grows toward higher offsets (top down).
 *  - Tuple data grows toward lower offsets (bottom up).
 *  - free_space_pointer marks the boundary between free space and tuple data.
 *  - num_slots counts allocated slot entries (including deleted slots).
 */
class SlottedPage {
 public:
  /**
   * @brief Wraps an existing page buffer without initialization.
   * @param buffer Pointer to the raw 4KB page buffer.
   */
  explicit SlottedPage(char* buffer);

  /**
   * @brief Initializes a buffer as an empty slotted page and returns a view over it.
   * @param buffer Pointer to the raw 4KB page buffer (will be zeroed).
   */
  static SlottedPage init(char* buffer);

  /**
   * @brief Inserts a tuple into the page.
   * @param tuple_data Pointer to the serialized tuple bytes.
   * @param tuple_size Size of the tuple in bytes.
   * @return The slot ID where the tuple was placed, or std::nullopt if no space.
   */
  std::optional<uint16_t> insert_tuple(const char* tuple_data, uint32_t tuple_size);

  /**
   * @brief Retrieves a read-only pointer to a tuple's data.
   * @param slot_id The slot to retrieve.
   * @param[out] size Set to the tuple's byte length on success.
   * @return Pointer to the tuple data, or nullptr if slot is invalid/deleted.
   */
  const char* get_tuple(uint16_t slot_id, uint32_t* size) const;

  /**
   * @brief Returns available space in bytes (accounting for a potential new slot entry).
   */
  size_t get_free_space() const;

  /**
   * @brief Returns the number of slot entries (active + deleted).
   */
  uint16_t get_num_slots() const;

 private:
  char* data_;
  SlottedPageHeader* header_;
  Slot* slots_;
};

}
