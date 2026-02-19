#pragma once

#include "storage_def.h"
#include "config.h"
#include <vector>
#include <string_view>

namespace letty {

/**
 * @struct SlottedPageHeader
 * @brief Header for slotted page.
 */
#pragma pack(1)
struct SlottedPageHeader {
  //Log Sequence Number. Needed for crash recovery when we implement WAL
  lsn_t lsn = 0;
  page_id_t next_page_id = INVALID_PAGE_ID;
  page_id_t prev_page_id = INVALID_PAGE_ID;
  uint16_t num_slots = 0;       // Number of slots currently allocated. Will also include deleted slots.
  uint16_t free_space_pointer = PAGE_SIZE; // Offset to the start of free space (grows downwards)
  uint16_t tuple_count = 0; // Only includes active slots.
};

/**
 * @struct Slot
 * @brief Represents a slot in the directory for SlottedPage
 */
struct Slot {
  uint16_t offset;
  uint16_t length;
};
#pragma pack()

/**
 * @class SlottedPage
 * @brief Wraps a raw page to provide Slotted Page functionality for variable-length tuple storage.
 *
 * Layout:
 * Slotted Page Layout (PAGE_SIZE bytes)
 *
 *  Offsets grow downward ↓
 *
 *  +--------------------------------------------------------------+ 0
 *  | SlottedPageHeader (fixed-size)                               |
 *  |  - lsn                                                       |
 *  |  - next_page_id / prev_page_id                               |
 *  |  - num_slots       (# slot entries allocated; includes holes)|
 *  |  - tuple_count     (# active tuples; excludes deleted slots) |
 *  |  - free_space_pointer (offset where free space begins)       |
 *  +--------------------------------------------------------------+  sizeof(Header)
 *  | Slot Directory (grows upward →)                              |
 *  |  slot[0] -> (tuple_offset, tuple_length [, flags])           |
 *  |  slot[1] -> (tuple_offset, tuple_length [, flags])           |
 *  |  ...                                                        |
 *  |  slot[num_slots-1]                                           |
 *  +------------------------------ free space ---------------------+
 *  | Free Space (shrinks as slots grow up and tuples grow down)   |
 *  +--------------------------------------------------------------+  free_space_pointer
 *  | Tuple / Record Data Area (grows downward ← from PAGE_SIZE)   |
 *  |  tuple bytes ...                                             |
 *  |  tuple bytes ...                                             |
 *  +--------------------------------------------------------------+  PAGE_SIZE
 *
 * Invariants:
 *  - Slot directory grows from the front of the page (upwards).
 *  - Tuple data grows from the end of the page (downwards).
 *  - free_space_pointer always points to the start of tuple data.
 *  - num_slots counts allocated slot entries (including deleted slots).
 *  - tuple_count counts only ACTIVE tuples.
 */
class SlottedPage {
 public:
  /**
   * @brief Constructs a SlottedPage view over a raw buffer.
   * @param buffer Pointer to the raw 4KB page buffer.
   * @param init If true, initializes the header (clears page).
   */
  explicit SlottedPage(char* buffer, bool init = false);

  /**
   * @brief Inserts a tuple into the page.
   * @param tuple_data Pointer to the data.
   * @param tuple_size Size of the data.
   * @return The slot ID where the tuple was inserted, or -1 if no space.
   */
  int32_t insert_tuple(const char* tuple_data, uint32_t tuple_size);

  /**
   * @brief Retrieves a pointer to the tuple data.
   * @param slot_id The slot ID to retrieve.
   * @param[out] size Output parameter for the size of the tuple.
   * @return Pointer to the data, or nullptr if slot is invalid/deleted.
   */
  char* get_tuple(uint16_t slot_id, uint32_t* size);

  /**
   * @brief Marks a tuple as deleted.
   * @param slot_id The slot ID to delete.
   * @return True if successful, false if slot_id is invalid.
   */
  bool delete_tuple(uint16_t slot_id);

  /**
   * @brief Returns the amount of free space remaining in bytes.
   */
  size_t get_free_space() const;

  /**
   * @brief Returns the number of slots (valid + invalid).
   */
  uint16_t get_num_slots() const;

  /**
   * @brief Returns the total number of active tuples in the page.
   */
  uint16_t get_tuple_count() const;

 private:
  char* data_; // holds the actual bytes
  SlottedPageHeader* header_;
  Slot* slots_; // Pointer to the start of the slot array
};

}
