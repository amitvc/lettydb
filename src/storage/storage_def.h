//
// Created by Amit Chavan on 9/2/25.
//

#pragma once

#include "config.h"
#include "sql/utils.h"
#include <cstddef>
#include <cstring>


namespace letty {


// Disable compiler-inserted alignment padding so struct layouts match
// their on-disk byte offsets exactly. These structs are overlaid onto raw
// page buffers via reinterpret_cast, so any padding would misalign fields
// and corrupt reads/writes.
// See: https://learn.microsoft.com/en-us/cpp/preprocessor/pack
#pragma pack(push, 1)

/**
 * @struct DatabaseHeader
 * @brief Page 0 — the entry point for all database metadata.
 *
 * Always the first page in the database file. Has a fixed layout and
 * occupies exactly one page (PAGE_SIZE bytes). All other pages are
 * located through this page.
 *
 * @verbatim
 * Byte Offsets (PAGE_SIZE = 4096)
 *
 *  +--------------------------------------------------------------+ 0
 *  | signature[8]   ("LETTYDB\0")                                 |
 *  |   - Identifies this file as a Letty database                 |
 *  +--------------------------------------------------------------+ 8
 *  | version (uint32)                                             |
 *  |   - Database file format version (currently 2)               |
 *  +--------------------------------------------------------------+ 12
 *  | page_size (uint32)                                           |
 *  |   - Size of each page in bytes (e.g., 4096)                  |
 *  +--------------------------------------------------------------+ 16
 *  | gam_page_id (page_id_t)                                      |
 *  |   - Page ID of the Global Allocation Map (GAM)               |
 *  |   - Always page 1                                            |
 *  +--------------------------------------------------------------+ 20
 *  | metadata_pool_page_id (page_id_t)                            |
 *  |   - First page of the metadata pool extent                   |
 *  |   - Used to allocate IAM pages dynamically                   |
 *  +--------------------------------------------------------------+ 24
 *  | sys_tables_iam_page (page_id_t)                              |
 *  |   - IAM page for system catalog: sys_tables                  |
 *  |   - Dynamically allocated from metadata pool                 |
 *  +--------------------------------------------------------------+ 28
 *  | sys_columns_iam_page (page_id_t)                             |
 *  |   - IAM page for system catalog: sys_columns                 |
 *  |   - Dynamically allocated from metadata pool                 |
 *  +--------------------------------------------------------------+ 32
 *  | next_table_oid (uint16)                                      |
 *  |   - Next available OID for user tables                       |
 *  |   - System tables use OIDs 1-99, users start at 100          |
 *  +--------------------------------------------------------------+ 34
 *  | padding[4062]                                                |
 *  |   - Zero-filled; reserved for future metadata                |
 *  |   - Ensures the header occupies exactly one full page        |
 *  +--------------------------------------------------------------+ PAGE_SIZE
 * @endverbatim
 *
 * @par Invariants
 *  - page_id is always 0
 *  - Layout is fixed and backward-compatible via `version`
 *  - All other pages are bootstrapped through this page
 */
struct DatabaseHeader {
  char signature[8];
  uint32_t version = 1;
  // Page size stored for file validation and potential future support of variable page sizes.
  // Standalone tools can use this to read db pages.
  uint32_t page_size = PAGE_SIZE;
  // The GAM page is always the 2nd page in the file. Value is 1 since page 0 is db header page.
  page_id_t gam_page_id = FIRST_GAM_PAGE_ID;

  // Points to the first metadata pool extent (for IAM pages and other metadata)
  page_id_t metadata_pool_page_id = FIRST_METADATA_POOL_PAGE_ID;

  // Points to the IAM page for the 'sys_tables' table (dynamically assigned from metadata pool)
  page_id_t sys_tables_iam_page = INVALID_PAGE_ID;

  // Points to the IAM page for the 'sys_columns' table (dynamically assigned from metadata pool)
  page_id_t sys_columns_iam_page = INVALID_PAGE_ID;

  // Next available OID for user tables. System tables use OIDs 1-99.
  // User tables start at 100 and increment from there.
  uint16_t next_table_oid = 100;

  // 8 (sig) + 4 (ver) + 4 (page size) + 4 (gam) + 4 (meta_pool) + 4 (sys_tables) + 4 (sys_cols) + 2 (next_oid) = 34 bytes
  uint8_t padding[PAGE_SIZE - 34]; // Padding to ensure the header completely fills the page.

  DatabaseHeader() {
	  std::memset(signature, 0, sizeof(signature));  // zero pad the entire field
	  std::memcpy(signature, DB_SIGNATURE, sizeof(DB_SIGNATURE));
  }
};

static_assert(sizeof(DatabaseHeader) == PAGE_SIZE, 
              "DatabaseHeader must be exactly PAGE_SIZE bytes");

/**
 * @struct GAMPage
 * @brief Global Allocation Map — tracks which extents are allocated.
 *
 * Overlays a raw 4 KB page. Each bit represents one extent (8 pages).
 * A single GAM page tracks 32,720 extents (~1 GB). Additional GAM pages
 * are chained via `next_page_id` when the database grows beyond that.
 *
 * @verbatim
 *  +--------------------------------------------------------------+ 0
 *  | next_page_id (page_id_t)                                     |
 *  |   - Links to the next GAM page (INVALID_PAGE_ID if last)     |
 *  +--------------------------------------------------------------+ 4
 *  | first_free_bit_hint (uint16_t)                               |
 *  |   - Starting bit index for the next free-extent search       |
 *  |   - Avoids O(n) linear scan from bit 0 every time            |
 *  +--------------------------------------------------------------+ 6
 *  | bitmap[4090]                                                 |
 *  |   - 1 bit per extent: 0 = free, 1 = allocated                |
 *  |   - 4090 bytes * 8 bits = 32,720 extents                     |
 *  |   - 32,720 extents * 8 pages = 261,760 pages (~1 GB)         |
 *  +--------------------------------------------------------------+ 4096
 * @endverbatim
 */
struct GAMPage {
  // Links GAM pages together when database grows beyond single page capacity
  page_id_t next_page_id = INVALID_PAGE_ID;

  // Hint for where to start searching for free bits (optimization)
  // Updated on allocation (set to next bit) and deallocation (rewind if earlier)
  uint16_t first_free_bit_hint = 0;

  // Bitmap tracking extent allocation (1 = allocated, 0 = free)
  char bitmap[GAM_BITMAP_ARRAY_SIZE]; // 4090 = 4096 - (4 bytes PAGE_ID + 2 bytes hint)
};

/**
 * @struct MetadataPoolDirectoryPage
 * @brief Directory page for a metadata pool extent.
 *
 * A metadata pool extent holds up to 7 metadata pages (IAM pages, etc.).
 * Page 0 of the extent is this directory; pages 1–7 are allocatable slots.
 *
 * @verbatim
 *  +--------------------------------------------------------------+ 0
 *  | next_pool_page (page_id_t)                                   |
 *  |   - Links to the next pool extent when this one is full      |
 *  |   - INVALID_PAGE_ID if this is the last extent               |
 *  +--------------------------------------------------------------+ 4
 *  | slots_bitmap (uint8_t)                                       |
 *  |   - Bit i set means slot i is occupied (bits 1–7 only)       |
 *  |   - Bit 0 is unused (reserved for the directory page itself) |
 *  +--------------------------------------------------------------+ 5
 *  | padding[4091]                                                |
 *  |   - Zero-filled; ensures the struct fills exactly one page   |
 *  +--------------------------------------------------------------+ 4096
 * @endverbatim
 */
struct MetadataPoolDirectoryPage {
  page_id_t next_pool_page = INVALID_PAGE_ID;  // 4 bytes
  uint8_t slots_bitmap = 0;                     // 1 byte - bits 1-7 represent slots
  char padding[PAGE_SIZE - 5];                  // Fill the rest of the page
  
  static constexpr uint8_t NO_FREE_SLOT = 0;

  /**
   * @brief Find a free slot in this pool (1-7).
   * @return Slot number (1-7) or NO_FREE_SLOT if no free slot.
   */
  uint8_t find_free_slot() const {
    for (uint8_t i = 1; i <= METADATA_POOL_SLOTS; ++i) {
      if ((slots_bitmap & (1 << i)) == 0) {
        return i;
      }
    }
    return NO_FREE_SLOT;
  }
  
  /**
   * @brief Mark a slot as used.
   */
  void mark_slot_used(uint8_t slot) {
    slots_bitmap |= (1 << slot);
  }
  
  /**
   * @brief Mark a slot as free.
   */
  void mark_slot_free(uint8_t slot) {
    slots_bitmap &= ~(1 << slot);
  }
  
  /**
   * @brief Check if a slot is used.
   */
  bool is_slot_used(uint8_t slot) const {
    return (slots_bitmap & (1 << slot)) != 0;
  }
};

/**
 * @struct IAMPage
 * @brief Index Allocation Map — tracks which extents belong to one table.
 *
 * Stores extent IDs as a compact list rather than a sparse bitmap.
 * Each table has its own IAM chain; pages are chained via `next_page_id`
 * when a single page's 1022-entry list is exhausted (~32 MB per IAM page).
 *
 * @verbatim
 *  +--------------------------------------------------------------+ 0
 *  | next_page_id (page_id_t)                                     |
 *  |   - Next IAM page in this table's chain                      |
 *  |   - INVALID_PAGE_ID if this is the last page                 |
 *  +--------------------------------------------------------------+ 4
 *  | extent_count (uint16_t)                                      |
 *  |   - Number of valid entries in extent_ids[]                  |
 *  +--------------------------------------------------------------+ 6
 *  | reserved (uint16_t)                                          |
 *  |   - Alignment padding; always 0                              |
 *  +--------------------------------------------------------------+ 8
 *  | extent_ids[1022]                                             |
 *  |   - 4-byte extent IDs owned by this table                    |
 *  |   - 1022 entries * 4 bytes = 4088 bytes                      |
 *  |   - 1022 extents * 8 pages * 4096 bytes = ~32 MB per page    |
 *  +--------------------------------------------------------------+ 4096
 * @endverbatim
 */
struct IAMPage {
  page_id_t next_page_id = INVALID_PAGE_ID;  // 4 bytes
  uint16_t extent_count = 0;                  // 2 bytes
  uint16_t reserved = 0;                      // 2 bytes (alignment)
  uint32_t extent_ids[IAM_MAX_EXTENTS];       // 4088 bytes = 1022 extent IDs
  
  /**
   * @brief Add an extent to the list.
   * @return true if added, false if list is full.
   */
  bool add_extent(uint32_t extent_id) {
    if (extent_count >= IAM_MAX_EXTENTS) {
      return false;
    }
    extent_ids[extent_count++] = extent_id;
    return true;
  }
  
  /**
   * @brief Check if this page has room for another extent.
   */
  bool has_space() const {
    return extent_count < IAM_MAX_EXTENTS;
  }
  
  /**
   * @brief Check if an extent is in this page's list.
   */
  bool contains_extent(uint32_t extent_id) const {
    for (uint16_t i = 0; i < extent_count; ++i) {
      if (extent_ids[i] == extent_id) {
        return true;
      }
    }
    return false;
  }
};
#pragma pack(pop)

/**
 * @class Bitmap
 * @brief A helper class to manipulate raw bits stored in the bitmap array inside the GAMPage class.
 *
 * This class provides an abstraction over raw byte arrays to allow for easy setting, clearing,
 * and checking of individual bits. This is primarily used for managing Allocation Maps (GAM/IAM),
 * where each bit represents the status (allocated/free) of an extent (for GAM) or page (for IAM).
 */
class Bitmap {
 public:
  /**
   * @brief Constructs a Bitmap wrapper around raw bitmap data.
   * @param data A pointer to the start of the bitmap data (e.g., GAMPage::bitmap).
   * @param size_in_bits The total number of bits the bitmap can hold.
   */
  explicit Bitmap(char *data, size_t size_in_bits)
	  : data(reinterpret_cast<uint8_t *>(data)), size(size_in_bits) {}

  /**
   * @brief Checks if a specific bit is set to 1.
   * @param bit_index The index of the bit to check.
   * @return True if the bit is set, false otherwise.
   */
  bool is_set(uint32_t bit_index) const {
	if (bit_index >= size) return false;
	// Find the byte, then check the specific bit within that byte.
	return (data[bit_index / 8] & (1 << (bit_index % 8))) != 0;
  }

  /**
   * @brief Sets a specific bit to 1.
   * @param bit_index The index of the bit to set.
   */
  void set(uint32_t bit_index) {
	if (bit_index >= size) return;
	data[bit_index / 8] |= (1 << (bit_index % 8));
  }

  /**
   * @brief Clears a specific bit, setting it to 0.
   * @param bit_index The index of the bit to clear.
   */
  void clear(uint32_t bit_index) {
	if (bit_index >= size) return;
	data[bit_index / 8] &= ~(1 << (bit_index % 8));
  }

  /**
   * @brief Returns the size of the bitmap.
   * @return
   */
  size_t get_size_in_bits() {
	return size;
  }
 private:
  uint8_t *data;
  size_t size;
};
}