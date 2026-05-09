#pragma once

#include "storage_def.h"
#include "config.h"
#include <optional>


namespace letty {

/**
 * @struct SlottedPageHeader
 * @brief Fixed-size metadata stored at byte 0 of a slotted data page.
 *
 * The header tracks the two moving boundaries inside the page:
 * - the slot directory grows forward from the beginning of the page
 * - tuple bytes grow backward from the end of the page
 *
 * The space between those two regions is available for future inserts.
 */
#pragma pack(push, 1)
struct SlottedPageHeader {
  // Log Sequence Number. Reserved for WAL-based crash recovery.
  lsn_t lsn = 0;
  // Currently these fields are not being used. We have a back log item to use them during full page table scane
  page_id_t next_page_id = INVALID_PAGE_ID;
  page_id_t prev_page_id = INVALID_PAGE_ID;
  /// Number of slot entries allocated, including deleted (tombstone) slots.
  uint16_t num_slots = 0;
  /// Offset to the start of tuple data. Decreases as tuples are appended.
  uint16_t free_space_pointer = PAGE_SIZE;
};

/**
 * @struct Slot
 * @brief Directory entry that points to one tuple stored elsewhere in the page.
 *
 * A slot is stable even if tuple bytes live at the end of the page. Higher
 * layers can identify a tuple by slot id instead of by byte offset. A length of
 * 0 means the slot is deleted and can be reused by a future insert.
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
 * A table page cannot store rows at fixed offsets because tuples may have
 * variable-sized values such as VARCHAR. SlottedPage solves this by separating
 * tuple identity from tuple bytes:
 *
 * - the slot directory stores small fixed-size entries near the front
 * - the tuple payload bytes are copied near the end
 * - free space remains in the middle
 *
 * The slot id is the stable handle returned to callers. The tuple bytes can sit
 * anywhere in the tuple area as long as the slot records their offset and size.
 *
 * SlottedPage is a non-owning view over a PAGE_SIZE byte buffer. The caller owns
 * that buffer, usually through BufferPoolManager, and must keep it alive while
 * the SlottedPage object is used. Constructing SlottedPage around a buffer does
 * not initialize it; call init() when formatting a brand-new data page.
 *
 * Layout (PAGE_SIZE bytes):
 *
 *  byte offsets increase downward
 *
 *  +--------------------------------------------------------------+ 0
 *  | SlottedPageHeader (fixed-size)                               |
 *  +--------------------------------------------------------------+  sizeof(Header)
 *  | Slot directory, grows toward larger offsets                   |
 *  |  slot[0] = {offset, length}                                  |
 *  |  slot[1] = {offset, length}                                  |
 *  |  ...                                                        |
 *  |  slot[num_slots-1]                                           |
 *  +--------------------------------------------------------------+
 *  | Free space                                                    |
 *  +--------------------------------------------------------------+  free_space_pointer
 *  | Tuple payload bytes, grows toward smaller offsets             |
 *  +--------------------------------------------------------------+  PAGE_SIZE
 *
 * Invariants:
 * - num_slots counts all allocated slot entries, including deleted slots.
 * - free_space_pointer is the first byte of the tuple payload region.
 * - usable free space is between the end of the slot directory and
 *   free_space_pointer.
 * - slot.length == 0 marks a deleted/reusable slot.
 *
 * Implementation note:
 * SlottedPage treats the page as byte storage. Header and slot entries are
 * copied in and out with std::memcpy rather than accessed through pointers into
 * the buffer.
 */
class SlottedPage {
 public:
  /**
   * @brief Wrap an existing slotted page buffer without changing its bytes.
   *
   * Use this for pages that were already initialized and loaded through the
   * BufferPoolManager. Passing an uninitialized page will make the header fields
   * meaningless.
   *
   * @param buffer Pointer to a PAGE_SIZE page buffer.
   */
  explicit SlottedPage(char* buffer);

  /**
   * @brief Format a buffer as an empty slotted page and return a view over it.
   *
   * This zeroes the page, resets the page links, sets num_slots to 0, and moves
   * free_space_pointer to PAGE_SIZE so tuple bytes can start growing backward
   * from the end of the page.
   *
   * @param buffer Pointer to a PAGE_SIZE page buffer.
   */
  static SlottedPage init(char* buffer);

  /**
   * @brief Insert serialized tuple bytes into this page.
   *
   * The tuple payload is copied into the tuple area. If a deleted slot exists,
   * that slot id is reused; otherwise a new slot entry is appended to the slot
   * directory. The insert fails if there is not enough contiguous free space for
   * the tuple and, when needed, one new Slot entry.
   *
   * @param tuple_data Pointer to the serialized tuple bytes.
   * @param tuple_size Size of the tuple in bytes.
   * @return The slot ID where the tuple was placed, or std::nullopt if no space.
   */
  std::optional<uint16_t> insert_tuple(const char* tuple_data, uint32_t tuple_size);

  /**
   * @brief Return a read-only pointer to tuple bytes for a slot.
   *
   * The returned pointer points inside the page buffer owned by the caller. It
   * remains valid only while the underlying page remains pinned/live and
   * unchanged.
   *
   * @param slot_id The slot to retrieve.
   * @param[out] size Set to the tuple's byte length on success.
   * @return Pointer to the tuple data, or nullptr if slot is invalid/deleted.
   */
  const char* get_tuple(uint16_t slot_id, uint32_t* size) const;

  /**
   * @brief Return currently available contiguous free space in bytes.
   *
   * This is the gap between the end of the slot directory and
   * free_space_pointer. insert_tuple() may additionally need sizeof(Slot) bytes
   * if it cannot reuse a deleted slot.
   */
  size_t get_free_space() const;

  /**
   * @brief Returns the number of slot entries (active + deleted).
   */
  uint16_t get_num_slots() const;

 private:
  SlottedPageHeader load_header() const;
  void store_header(const SlottedPageHeader& header);
  Slot load_slot(uint16_t slot_id) const;
  void store_slot(uint16_t slot_id, const Slot& slot);
  static size_t slot_offset(uint16_t slot_id);

  char* data_;
};

}
