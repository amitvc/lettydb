#pragma once

#include "storage_def.h"
#include "config.h"
#include <optional>


namespace letty {

/**
 * @struct SlottedPageHeader
 * @brief Fixed-size metadata stored at byte 0 of every slotted data page.
 *
 * This struct is laid out directly on disk. Any change to field order, types,
 * or total size (currently 16 bytes) will corrupt existing database files.
 * The static_assert in slotted_page.cpp enforces the size at compile time.
 *
 * Fields are naturally aligned without #pragma pack — the first three int32_t
 * fields sit on 4-byte boundaries and the two uint16_t fields fill the
 * remaining space exactly to 16 bytes.
 *
 * The header tracks the two moving boundaries inside the page:
 * - the slot directory grows forward from the beginning of the page
 * - tuple bytes grow backward from the end of the page
 *
 * The space between those two regions is available for future inserts.
 */
struct SlottedPageHeader {
  // Log Sequence Number. Reserved for WAL-based crash recovery.
  lsn_t lsn = 0;
  // Logical page linkage. Reserved for future full-table sequential scans that
  // bypass the IAM chain (e.g., when the IAM is known to be stale).
  page_id_t next_page_id = INVALID_PAGE_ID;
  page_id_t prev_page_id = INVALID_PAGE_ID;
  // Number of slot entries in the directory. Shrinks when trailing slots are
  // deleted; grows when a new slot must be appended beyond the current count.
  uint16_t num_slots = 0;
  // First byte of the tuple payload region. Decreases as tuples are inserted.
  uint16_t free_space_pointer = PAGE_SIZE;
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

/**
 * @class SlottedPage
 * @brief Non-owning view over a PAGE_SIZE buffer that provides variable-length
 *        tuple storage via a slot directory.
 *
 * Tables store rows of different sizes. SlottedPage separates **what** a row is
 * (its bytes) from **where** a row is (its slot entry) so that neither changes
 * when the other moves.
 *
 * Layout (PAGE_SIZE bytes):
 *
 *  +--------------------------------------------------------------+ 0
 *  | SlottedPageHeader (32 bytes)                                 |
 *  +--------------------------------------------------------------+ sizeof(Header)
 *  | Slot[0]  = {offset: 3800, length: 50}                        |
 *  | Slot[1]  = {offset: 2100, length:  0}  ← tombstone (deleted) |
 *  | Slot[2]  = {offset: 3850, length: 100}                       |
 *  | ...                                                          |
 *  +--------------------------------------------------------------+ ← end of slot directory
 *  | Free space (contiguous gap)                                   |
 *  +--------------------------------------------------------------+ free_space_pointer
 *  | Tuple bytes, grow toward smaller offsets                      |
 *  |   ... bytes for Slot[2] (100 bytes) ...                       |
 *  |   ... bytes for Slot[0] (50 bytes)  ...                       |
 *  +--------------------------------------------------------------+ PAGE_SIZE
 *
 * Growth rules:
 * - Slots are appended at the front of the free gap (grow forward).
 * - Tuple bytes are appended at the end of the page (grow backward).
 * - The free gap shrinks from both sides until it closes.
 *
 * Example (insert, insert, insert, delete):
 *
 *   insert "Alice" (5 bytes) → slot[0] = {3891, 5},    free_space_pointer = 3891
 *   insert "Bob"   (3 bytes) → slot[1] = {3888, 3},    free_space_pointer = 3888
 *   insert "Zara"  (4 bytes) → slot[2] = {3884, 4},    free_space_pointer = 3884
 *   delete slot 1             → slot[1] = {3888, 0},   num_slots still 3
 *
 *   A subsequent insert reuses slot 1 before growing the directory:
 *   insert "Eve"   (3 bytes) → slot[1] = {3881, 3},    free_space_pointer = 3881
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
 * Implementation note: header and slot entries are loaded and stored via
 * std::memcpy rather than by reinterpret-casting pointers into the buffer.
 * This avoids alignment issues on platforms that require strict alignment.
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
  SlottedPageHeader* header_;
};

}
