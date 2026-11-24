#pragma once

#include "storage_def.h"
#include "config.h"
#include "buffer/page_guard.h"
#include <optional>

namespace letty {
class Page;

/**
 * @struct SlotDirectoryHeader
 * @brief Fixed-size slotted-page metadata stored at byte 0 of a page's body
 *        (immediately after the 20-byte PageHeader).
 *
 * This struct is laid out directly on disk (4 bytes). The static_assert in
 * slotted_page.cpp enforces the size at compile time.
 *
 * The header tracks the two moving boundaries inside the 4076-byte body:
 * - the slot directory grows forward from byte 4 of the body
 * - tuple bytes grow backward from PAGE_BODY_SIZE (4076)
 *
 * The space between those two regions is available for future inserts.
 */
#pragma pack(push, 1)
struct SlotDirectoryHeader {
  // Number of slot entries in the directory. Shrinks when trailing slots are
  // deleted; grows when a new slot must be appended beyond the current count.
  uint16_t num_slots = 0;
  // First byte of the tuple payload region (relative to body start). Decreases as tuples are inserted.
  uint16_t free_space_pointer = PAGE_BODY_SIZE;
};

/**
 * @struct Slot
 * @brief One entry in the slot directory. Each slot represents one row and
 *        records where its bytes live inside the page.
 *
 * This struct is laid out on disk as part of every slotted page's slot
 * directory. Any change to its layout will corrupt existing database files.
 * The static_assert in slotted_page.cpp enforces the 4-byte size.
 *
 * Think of the slot directory as a numbered list. Slot 0 is row 0, slot 1 is
 * row 1, and so on. Each entry holds two numbers:
 * - `offset` — the byte position of the row's data inside the page buffer
 * - `length` — how many bytes the row occupies
 *
 * Note:
 * A row with length 0 is a tombstoned slot. Its entry still exists in the directory but
 * points to no live data. The next insert will reuse this slot number instead
 * of adding a new entry to the list.
 */
struct Slot {
  uint16_t offset;
  uint16_t length;
};
#pragma pack(pop)

/**
 * @class SlottedPage
 * @brief Non-owning view over a PAGE_BODY_SIZE buffer (Page::get_body()) that provides
 *        variable-length tuple storage via a slot directory.
 *
 * Tables store rows of different sizes. SlottedPage separates **what** a row is
 * (its bytes) from **where** a row is (its slot entry) so that neither changes
 * when the other moves.
 *
 * Layout (PAGE_BODY_SIZE = 4076 bytes, starting at Page::get_body()):
 *
 * @verbatim
 *  +--------------------------------------------------------------+ 0
 *  | SlotDirectoryHeader (4 bytes: num_slots, free_space_pointer)   |
 *  +--------------------------------------------------------------+ 4
 *  | Slot[0]  = {offset: 4071, length:  5}  ← "Alice"             |
 *  | Slot[1]  = {offset: 4068, length:  0}  ← tombstone (deleted) |
 *  | Slot[2]  = {offset: 4064, length:  4}  ← "Zara"              |
 *  +--------------------------------------------------------------+ ← end of slot directory (16)
 *  | Free space (contiguous gap)                                   |
 *  +--------------------------------------------------------------+ ← free_space_pointer (4064)
 *  | Tuple bytes, grow toward smaller offsets                      |
 *  |   ... bytes for Slot[2] (4 bytes) ...                         |
 *  |   ... bytes for Slot[0] (5 bytes) ...                         |
 *  +--------------------------------------------------------------+ 4076 (PAGE_BODY_SIZE)
 *  @endverbatim
 *
 * Growth rules:
 * - Slots are appended at the front of the free gap (grow forward).
 * - Tuple bytes are appended at the end of the page (grow backward).
 * - The free gap shrinks from both sides until it closes.
 *
 * Example (insert, insert, insert, delete):
 *
 *   insert "Alice" (5 bytes) → slot[0] = {4071, 5},    free_space_pointer = 4071
 *   insert "Bob"   (3 bytes) → slot[1] = {4068, 3},    free_space_pointer = 4068
 *   insert "Zara"  (4 bytes) → slot[2] = {4064, 4},    free_space_pointer = 4064
 *   delete slot 1             → slot[1] = {4068, 0},   num_slots still 3
 *
 *   A subsequent insert reuses slot 1 before growing the directory:
 *   insert "Eve"   (3 bytes) → slot[1] = {4061, 3},    free_space_pointer = 4061
 *   (Slot 1 pointed at "Bob"'s orphaned bytes which are now overwritten.)
 *
 * Invariants:
 * - num_slots is the directory size; it shrinks when trailing slots are
 *   tombstoned and grows when a new slot is appended past the current end.
 * - free_space_pointer marks the boundary between free space and tuple bytes.
 * - slot.length == 0 marks a deleted (tombstone) slot eligible for reuse.
 * - Usable free space = free_space_pointer − (sizeof(Header) + num_slots * sizeof(Slot)).
 *
 * Ownership: SlottedPage is a non-owning view. The caller (usually
 * BufferPoolManager) owns the underlying buffer and must keep it alive and
 * pinned while SlottedPage methods are called. Constructing SlottedPage does
 * not initialize the buffer; call init() to format a fresh page.
 *
 * Implementation note: all on-disk reads and writes go through little-endian
 * helpers. The header is cached in memory and persisted explicitly after
 * mutations. Slot access uses load/store functions rather than pointer casts.
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
   * @param buffer Pointer to a PAGE_BODY_SIZE body buffer (e.g., Page::get_body()).
   */
  explicit SlottedPage(Byte* buffer);

  /**
   * @brief Format a page body buffer as an empty slotted page and return a view over it.
   *
   * This zeroes the PAGE_BODY_SIZE body, sets num_slots to 0, and moves
   * free_space_pointer to PAGE_BODY_SIZE so tuple bytes can grow backward.
   *
   * @param buffer Pointer to a PAGE_BODY_SIZE buffer (e.g., Page::get_body()).
   */
  static SlottedPage init(Byte* buffer);

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
  std::optional<uint16_t> insert_tuple(const Byte* tuple_data, uint32_t tuple_size);

  /**
   * @brief Marks a slot as deleted (tombstone) by setting its length to zero.
   *
   * The tuple payload bytes are left in place. The slot becomes eligible for
   * reuse by a future insert_tuple() call. Free space increases by the original
   * tuple size.
   *
   * The caller is responsible for marking the page dirty in BufferPoolManager.
   *
   * @param slot_id The slot to delete.
   * @return true if the slot was live and is now deleted.
   *         false if the slot_id is out of range or already deleted.
   */
  bool delete_tuple(uint16_t slot_id);

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
  const Byte* get_tuple(uint16_t slot_id, uint32_t* size) const;

  /**
   * @brief Return currently available contiguous free space in bytes.
   *
   * This is the gap between the end of the slot directory and
   * free_space_pointer. insert_tuple() may additionally need sizeof(Slot) bytes
   * if it cannot reuse a deleted slot.
   */
  size_t get_free_space() const;

  /**
   * @brief Reclaims space from tombstoned slots by rebuilding the page.
   *
   * Copies all live tuple payloads to temporary buffers, re-initializes the
   * page with init(), and re-inserts every tuple. This eliminates all
   * orphaned bytes left behind by delete_tuple().
   *
   * @return true if the page had tombstones and was rebuilt.
   *         false if the page had no tombstones (no-op).
   */
  bool compact();

  /**
   * @brief Returns true if the page contains no live tuples.
   *
   * A page is empty when num_slots is 0 or every slot is a tombstone.
   */
  bool is_empty() const;

  /**
   * @brief Returns the number of slot entries (active + deleted).
   */
  uint16_t get_num_slots() const;

  /**
   * @brief Reads a slot entry from the page buffer in host byte order.
   * @return The slot entry, or nullopt if the slot_id is out of range.
   */
  std::optional<Slot> get_slot(uint16_t slot_id) const;

  /**
   * @brief Writes a slot entry to the page buffer in little-endian order.
   * @return true if the slot was written, false if slot_id is out of range.
   */
  bool set_slot(uint16_t slot_id, const Slot& slot);

  /**
   * @brief Applies a WAL slot payload during Redo or Undo.
   *
   * When tuple_size is 0, deletes the tuple at slot_id. When tuple_size > 0,
   * places the tuple bytes at the exact slot_id, growing the slot directory
   * if needed.
   *
   * @param slot_id Target slot index.
   * @param tuple_data Pointer to tuple bytes (may be nullptr when tuple_size is 0).
   * @param tuple_size Size of tuple in bytes (0 to delete the slot).
   * @return true if applied, false if the tuple does not fit in the page.
   */
  bool apply_slot_payload(uint16_t slot_id, const Byte* tuple_data, uint32_t tuple_size);

  /**
   * @brief Convenience helper to create a SlottedPage view over a Page's body.
   */
  static SlottedPage from_page(Page* page);

  /**
   * @brief Convenience helper to create a SlottedPage view over a PageGuard's body.
   */
  static SlottedPage from_page(PageGuard& guard) {
    return SlottedPage(reinterpret_cast<Byte*>(guard.get_body()));
  }

 private:
  void store_header();
  void repack_tuples_preserving_slots(uint16_t excluded_slot_id);
  static size_t slot_data_offset(uint16_t slot_id);

  Byte* data_;
  SlotDirectoryHeader header_;
};

}
