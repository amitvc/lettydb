#pragma once

#include "config.h"
#include "sql/utils.h"
#include <cstddef>
#include <cstring>
#include <type_traits>


namespace letty {


// Disable compiler-inserted alignment padding so struct layouts match
// their on-disk byte offsets exactly.
// See: https://learn.microsoft.com/en-us/cpp/preprocessor/pack
#pragma pack(push, 1)

/**
 * @struct DatabaseHeader
 * @brief Page 0 — the entry point for all database metadata.
 *
 * Always the first page in the database file. Has a fixed layout and
 * occupies exactly one page (PAGE_SIZE bytes). All other pages are reachable by
 * following pointers stored in this page.
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
 *  |   - Always page 8                                            |
 *  +--------------------------------------------------------------+ 20
 *  | shared_extent_page_id (page_id_t)                            |
 *  |   - First page of the shared extent                          |
 *  |   - Used to allocate IAM pages dynamically                   |
 *  +--------------------------------------------------------------+ 24
 *  | sys_tables_iam_page (page_id_t)                              |
 *  |   - IAM page for system catalog: sys_tables                  |
 *  |   - Dynamically allocated from shared extent                 |
 *  +--------------------------------------------------------------+ 28
 *  | sys_columns_iam_page (page_id_t)                             |
 *  |   - IAM page for system catalog: sys_columns                 |
 *  |   - Dynamically allocated from shared extent                 |
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
 *  - database header page is always 0
 *  - Layout is fixed and backward-compatible via `version`
 *  - All other pages are bootstrapped through this page
 */
struct DatabaseHeader {
  char signature[8];
  uint32_t version = 1;
  // Page size stored for file validation and potential future support of variable page sizes.
  // Standalone tools can use this to read db pages.
  uint32_t page_size = PAGE_SIZE;
  // The first GAM page is always the 1st page in extent 1. Extent 1 covers pages 8 to 15.
  page_id_t gam_page_id = FIRST_GAM_PAGE_ID;

  // Points to the first shared extent (for IAM pages and other metadata)
  page_id_t shared_extent_page_id = FIRST_SHARED_EXTENT_PAGE_ID;

  // Points to the IAM page for the 'sys_tables' table (dynamically assigned from shared extent)
  page_id_t sys_tables_iam_page = INVALID_PAGE_ID;

  // Points to the IAM page for the 'sys_columns' table (dynamically assigned from shared extent)
  page_id_t sys_columns_iam_page = INVALID_PAGE_ID;

  // Next available OID for user tables. System tables use OIDs 1-99.
  // User tables start at 100 and increment from there.
  uint16_t next_table_oid = 100;

  // 8 (sig) + 4 (ver) + 4 (page size) + 4 (gam_pg_id) + 4 (shared_extent_pg_id) + 4 (sys_tables_pg_id) + 4 (sys_cols_pg_id) + 2 (next_oid) = 34 bytes
  uint8_t padding[PAGE_SIZE - 34]; // Padding to ensure the header completely fills the page.

  DatabaseHeader() {
	  std::memset(signature, 0, sizeof(signature));  // zero pad the entire field
	  std::memcpy(signature, DB_SIGNATURE, sizeof(DB_SIGNATURE));
  }
};

static_assert(sizeof(DatabaseHeader) == PAGE_SIZE, 
              "DatabaseHeader must be exactly PAGE_SIZE bytes");
static_assert(std::is_trivially_copyable_v<DatabaseHeader>,
              "DatabaseHeader must be safe to copy to/from page bytes");
static_assert(std::is_standard_layout_v<DatabaseHeader>,
              "DatabaseHeader must have stable field offsets");
static_assert(offsetof(DatabaseHeader, version) == 8,
              "DatabaseHeader::version offset changed");

/**
 * Create a DatabaseHeader object and initialize it.
 * @return
 */
inline DatabaseHeader make_database_header() {
  DatabaseHeader header{};
  std::memset(header.signature, 0, sizeof(header.signature));
  std::memcpy(header.signature, DB_SIGNATURE, sizeof(DB_SIGNATURE));
  header.version = 1;
  header.page_size = PAGE_SIZE;
  header.gam_page_id = FIRST_GAM_PAGE_ID;
  header.shared_extent_page_id = FIRST_SHARED_EXTENT_PAGE_ID;
  header.sys_tables_iam_page = INVALID_PAGE_ID;
  header.sys_columns_iam_page = INVALID_PAGE_ID;
  header.next_table_oid = 100;
  return header;
}

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

  /* This is an optimization in order to avoid scanning all 4090 bits.
   * We start from this bit until the end of the bitmap to find an unused gam slot.
   * If we dont find one we rewind and search from 0 until this slot to find an empty gam page slot.
   * The reason for doing this is to make sure we dont miss out on gam pages that are freed up after table or page deletions
   **/
  uint16_t first_free_bit_hint = 0;

  // Bitmap tracking extent allocation (1 = allocated, 0 = free)
  uint8_t bitmap[GAM_BITMAP_ARRAY_SIZE]; // 4090 = 4096 - (4 bytes PAGE_ID + 2 bytes hint)
};

static_assert(sizeof(GAMPage) == PAGE_SIZE,
              "GAMPage must be exactly PAGE_SIZE bytes");
static_assert(std::is_trivially_copyable_v<GAMPage>,
              "GAMPage must be safe to copy to/from page bytes");
static_assert(std::is_standard_layout_v<GAMPage>,
              "GAMPage must have stable field offsets");
static_assert(offsetof(GAMPage, bitmap) == 6,
              "GAMPage::bitmap offset changed");

inline GAMPage make_gam_page() {
  GAMPage page{};
  page.next_page_id = INVALID_PAGE_ID;
  page.first_free_bit_hint = 0;
  return page;
}

/**
 * @struct SharedExtentDirectoryPage
 * @brief Directory page for a shared extent.
 *
 * A shared extent allows IAM pages for different tables to coexist in the same
 * extent instead of wasting a full extent per IAM page. For example, if each
 * table's first IAM page used a dedicated 8-page extent, creating 20 small
 * tables would reserve 160 pages even though only 20 IAM pages are needed.
 * With shared extents, those 20 IAM pages fit into 3 shared extents, reserving
 * 24 pages instead of 160.
 *
 * Page 0 of each shared extent serves as the directory page, while pages 1-7
 * are allocatable metadata slots. The directory tracks which slots are occupied
 * and links to the next shared extent when this one is full.
 *
 * @verbatim
 *  +--------------------------------------------------------------+ 0
 *  | next_pool_page (page_id_t)                                   |
 *  |   - Links to the next pool extent when this one is full      |
 *  |   - INVALID_PAGE_ID if this is the last extent               |
 *  +--------------------------------------------------------------+ 4
 *  | slot_used[7] (uint8_t array)                                 |
 *  |   - slot_used[i] is 1 when slot i+1 is occupied, otherwise 0 |
 *  |   - Slot numbers are 1-7 because page 0 is the directory     |
 *  +--------------------------------------------------------------+ 11
 *  | padding[4085]                                                |
 *  |   - Zero-filled; ensures the struct fills exactly one page   |
 *  +--------------------------------------------------------------+ 4096
 * @endverbatim
 */
struct SharedExtentDirectoryPage {
  page_id_t next_pool_page = INVALID_PAGE_ID;  // 4 bytes
  uint8_t slot_used[SHARED_EXTENT_SLOTS] = {};  // 7 bytes - slot N is stored at index N - 1
  char padding[PAGE_SIZE - 4 - SHARED_EXTENT_SLOTS]; // This padding is essentially not being used.
  
  static constexpr uint8_t NO_FREE_SLOT = 0;

  /**
   * @brief Find a free slot in this pool (1-7).
   * @return Slot number (1-7) or NO_FREE_SLOT if no free slot.
   */
  uint8_t find_free_slot() const {
    for (uint8_t slot = 1; slot <= SHARED_EXTENT_SLOTS; ++slot) {
      if (slot_used[slot - 1] == 0) {
        return slot;
      }
    }
    return NO_FREE_SLOT;
  }
  
  /**
   * @brief Mark a slot as used.
   */
  void mark_slot_used(uint8_t slot) {
    slot_used[slot - 1] = 1;
  }
  
  /**
   * @brief Mark a slot as free.
   */
  void mark_slot_free(uint8_t slot) {
    slot_used[slot - 1] = 0;
  }
  
  /**
   * @brief Check if a slot is used.
   */
  bool is_slot_used(uint8_t slot) const {
    return slot_used[slot - 1] != 0;
  }
};

static_assert(sizeof(SharedExtentDirectoryPage) == PAGE_SIZE,
              "SharedExtentDirectoryPage must be exactly PAGE_SIZE bytes");
static_assert(std::is_trivially_copyable_v<SharedExtentDirectoryPage>,
              "SharedExtentDirectoryPage must be safe to copy to/from page bytes");
static_assert(std::is_standard_layout_v<SharedExtentDirectoryPage>,
              "SharedExtentDirectoryPage must have stable field offsets");
static_assert(offsetof(SharedExtentDirectoryPage, slot_used) == 4,
              "SharedExtentDirectoryPage::slot_used offset changed");

inline SharedExtentDirectoryPage make_shared_extent_directory_page() {
  SharedExtentDirectoryPage page{};
  page.next_pool_page = INVALID_PAGE_ID;
  return page;
}

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

static_assert(sizeof(IAMPage) == PAGE_SIZE,
              "IAMPage must be exactly PAGE_SIZE bytes");
static_assert(std::is_trivially_copyable_v<IAMPage>,
              "IAMPage must be safe to copy to/from page bytes");
static_assert(std::is_standard_layout_v<IAMPage>,
              "IAMPage must have stable field offsets");
static_assert(offsetof(IAMPage, extent_ids) == 8,
              "IAMPage::extent_ids offset changed");

inline IAMPage make_iam_page() {
  IAMPage page{};
  page.next_page_id = INVALID_PAGE_ID;
  page.extent_count = 0;
  page.reserved = 0;
  return page;
}
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
  explicit Bitmap(uint8_t *data, size_t size_in_bits)
	  : data(data), size(size_in_bits) {}

  explicit Bitmap(char *data, size_t size_in_bits)
	  : data(static_cast<uint8_t *>(static_cast<void *>(data))), size(size_in_bits) {}

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
