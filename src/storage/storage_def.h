#pragma once

/**
 * @file storage_def.h
 * @brief On-disk metadata page structures and directory hierarchy for LettyDB.
 *
 * LettyDB writes all data to a single database file .db extension. The data is divided into fixed size
 * blocks called Page. Currently, the page size is fixed at 4 KB. We also group a sequence of Pages together.
 * This is called an Extent. Currently size of extent is 32 KB with 8 pages.
 *
 * Three metadata page types form a two-level directory hierarchy:
 *
 * 1. DatabaseHeader (Page 0):
 *    The fixed entry point of the database file. Stores the file signature,
 *    format version, the root GAM page ID (always Page 8), and the IAM head
 *    page IDs for the system catalog tables (`sys_tables` and `sys_columns`).
 *    This page allows the storage engine to bootstrap and understand what tables are defined and their schema.
 *
 * 2. GAMPage (Global Allocation Map — Database-Wide Extent Directory):
 *    Tracks which 8-page extents across the entire database file are free (0)
 *    or allocated (1) using a bitmap. Page 8 is the first GAM page; additional
 *    GAM pages are chained via `next_page_id` when the database grows beyond
 *    32,656 extents (~1.045 GB).
 *
 * 3. IAMPage (Index Allocation Map — Per-Table Extent Directory):
 *    Each table (system or user) has its own linked list of IAM pages. An
 *    IAMPage stores a compact array of up to 1,020 `extent_ids` owned by that
 *    specific table (~32.6 MB of table data per IAM page).
 *
 *  Reference - You can read more about this here -
 *  https://learn.microsoft.com/en-us/sql/relational-databases/pages-and-extents-architecture-guide?view=sql-server-ver17
 *
 * @verbatim
 * High-Level Directory Hierarchy
 *
 *  +-------------------------------------------------------------------+
 *  | Page 0: DatabaseHeader (Superblock)                               |
 *  |   gam_page_id          = 8  ----------------------------------+   |
 *  |   sys_tables_iam_page  = 16 -----------------+                |   |
 *  |   sys_columns_iam_page = 24                  |                |   |
 *  +----------------------------------------------|----------------|---+
 *                                                 |                |
 *        +----------------------------------------+                |
 *        v                                                         v
 *  +---------------------------------------+    +---------------------------------------+
 *  | Page 16: IAMPage ("sys_tables" Dir)   |    | Page 8: GAMPage (Global Extent Dir)   |
 *  |   extent_ids = [ 4 ]                  |    |   bit 0 = 1 (Extent 0: Header)        |
 *  |   next_page_id = INVALID_PAGE_ID      |    |   bit 1 = 1 (Extent 1: GAM)           |
 *  +-------------------+-------------------+    |   bit 2 = 1 (Extent 2: sys_tables IAM)|
 *                      |                        |   bit 3 = 1 (Extent 3: sys_cols IAM)  |
 *                      v                        |   bit 4 = 1 (Extent 4: sys_tables data|
 *  +---------------------------------------+    |   bit 5 = 1 (Extent 5: "users" IAM)   |
 *  | Extent 4 (Pages 32..39): sys_tables   |    |   bit 6 = 1 (Extent 6: "users" data)  |
 *  |   Row: {oid=100, name="users", iam=40}|    |   bit 7 = 0 (FREE)                    |
 *  +-------------------+-------------------+    +---------------------------------------+
 *                      |
 *                      v
 *  +---------------------------------------+
 *  | Page 40: IAMPage ("users" Table Dir)  |
 *  |   extent_ids = [ 6, ... ]             | ---> Extent 6 (Pages 48..55): SlottedPages
 *  +---------------------------------------+
 * @endverbatim
 */

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
 * @brief Page 0 — the superblock and entry point for all database metadata.
 *
 * Always the first page in the database file. Has a fixed layout and
 * occupies exactly one page (PAGE_SIZE = 4096 bytes). All other pages are
 * reachable by following page IDs stored in this header.
 *
 * @verbatim
 * Byte Offsets (PAGE_SIZE = 4096)
 *
 *  +--------------------------------------------------------------+ 0
 *  | signature[8]   ("LETTYDB\0")                                 |
 *  |   - Identifies this file as a LettyDB database               |
 *  +--------------------------------------------------------------+ 8
 *  | version (uint32_t)                                           |
 *  |   - Database file format version (currently 1)               |
 *  +--------------------------------------------------------------+ 12
 *  | page_size (uint32_t)                                         |
 *  |   - Size of each page in bytes (4096)                        |
 *  +--------------------------------------------------------------+ 16
 *  | gam_page_id (page_id_t)                                      |
 *  |   - Head page ID of the Global Allocation Map (always 8)     |
 *  +--------------------------------------------------------------+ 20
 *  | page_lsn (lsn_t)                                             |
 *  |   - LSN of the last WAL record that modified Page 0          |
 *  +--------------------------------------------------------------+ 28
 *  | sys_tables_iam_page (page_id_t)                              |
 *  |   - IAM head page for system catalog table: sys_tables       |
 *  +--------------------------------------------------------------+ 32
 *  | sys_columns_iam_page (page_id_t)                             |
 *  |   - IAM head page for system catalog table: sys_columns      |
 *  +--------------------------------------------------------------+ 36
 *  | next_table_oid (uint16_t)                                    |
 *  |   - Next available OID for user tables (starts at 100)       |
 *  +--------------------------------------------------------------+ 38
 *  | padding[4058]                                                |
 *  |   - Zero-filled; reserved for future metadata                |
 *  +--------------------------------------------------------------+ 4096
 * @endverbatim
 *
 * @par Invariants
 *  - DatabaseHeader is always stored at HEADER_PAGE_ID (Page 0)
 *  - Layout is fixed and versioned via `version`
 *  - All system and user tables are bootstrapped from this page
 */
struct DatabaseHeader {
  char signature[8];
  uint32_t version = 1;
  // Page size stored for file validation and potential future support of variable page sizes.
  // Standalone tools can use this to read db pages.
  uint32_t page_size = PAGE_SIZE;
  // The first GAM page is always the 1st page in extent 1. Extent 1 covers pages 8 to 15.
  page_id_t gam_page_id = FIRST_GAM_PAGE_ID;

  lsn_t page_lsn = 0;

  // Points to the head page of an IAM dedicated for the 'sys_tables' table
  page_id_t sys_tables_iam_page = INVALID_PAGE_ID;

  // Points to the head page of an IAM dedicated for the 'sys_columns' table
  page_id_t sys_columns_iam_page = INVALID_PAGE_ID;

  // Next available OID for user tables. System tables use OIDs 1-99.
  // User tables start at 100 and increment from there.
  uint16_t next_table_oid = 100;

  // 8 (sig) + 4 (ver) + 4 (page size) + 4 (gam_pg_id) + 8 (page_lsn) + 4 (sys_tables_pg_id) + 4 (sys_cols_pg_id) + 2 (next_oid) = 38 bytes
  uint8_t padding[PAGE_SIZE - 38]; // Padding to ensure the header completely fills the page.

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
  header.sys_tables_iam_page = INVALID_PAGE_ID;
  header.sys_columns_iam_page = INVALID_PAGE_ID;
  header.next_table_oid = 100;
  return header;
}

/**
 * @struct GAMPage
 * @brief Global Allocation Map — the database-wide extent allocation directory.
 *
 * Overlays a 4 KB page. Each bit in `bitmap` represents one 8-page (32 KB) extent
 * in the database file: 0 = free, 1 = allocated.
 * A single GAM page tracks 32,656 extents (261,248 pages, ~1.045 GB). Additional
 * GAM pages are chained via `next_page_id` when the database grows beyond that.
 *
 * @verbatim
 * Byte Offsets (PAGE_SIZE = 4096)
 *
 *  +--------------------------------------------------------------+ 0
 *  | next_page_id (page_id_t, 4 bytes)                            |
 *  |   - Next GAM page in the chain (INVALID_PAGE_ID if last)     |
 *  +--------------------------------------------------------------+ 4
 *  | page_lsn (lsn_t, 8 bytes)                                    |
 *  |   - LSN of the last WAL record that modified this GAM page   |
 *  +--------------------------------------------------------------+ 12
 *  | first_free_bit_hint (uint16_t, 2 bytes)                      |
 *  |   - Bit index in [0, 32656) to start the next free search    |
 *  |   - Avoids scanning from bit 0 on every extent allocation    |
 *  +--------------------------------------------------------------+ 14
 *  | bitmap[4082] (4082 bytes = 32,656 bits)                      |
 *  |   - 1 bit per extent: 0 = free, 1 = allocated                |
 *  |   - 32,656 extents * 8 pages * 4096 bytes = ~1.045 GB        |
 *  +--------------------------------------------------------------+ 4096
 * @endverbatim
 */
struct GAMPage {
  page_id_t next_page_id = INVALID_PAGE_ID;
  lsn_t page_lsn = 0;

  // Starting bit index (0..GAM_MAX_BITS-1) for the next free-extent search.
  // ExtentManager scans from this hint to GAM_MAX_BITS, then wraps around to
  // [0, hint) to reclaim extents freed by table drops or compaction.
  uint16_t first_free_bit_hint = 0;

  // Extent allocation bitmap (1 = allocated, 0 = free).
  // 4082 bytes = 4096 - (4 bytes next_page_id + 8 bytes page_lsn + 2 bytes hint).
  uint8_t bitmap[GAM_BITMAP_ARRAY_SIZE];
};

static_assert(sizeof(GAMPage) == PAGE_SIZE,
              "GAMPage must be exactly PAGE_SIZE bytes");
static_assert(std::is_trivially_copyable_v<GAMPage>,
              "GAMPage must be safe to copy to/from page bytes");
static_assert(std::is_standard_layout_v<GAMPage>,
              "GAMPage must have stable field offsets");
static_assert(offsetof(GAMPage, bitmap) == 14,
              "GAMPage::bitmap offset changed");

inline GAMPage make_gam_page() {
  GAMPage page{};
  page.next_page_id = INVALID_PAGE_ID;
  page.page_lsn = 0;
  page.first_free_bit_hint = 0;
  return page;
}

/**
 * @struct IAMPage
 * @brief Index Allocation Map — the per-table extent directory.
 *
 * Each table owns its own linked list of IAM pages. Rather than a sparse
 * bitmap covering the entire database, an IAMPage stores a compact array of
 * the `extent_ids` (0..extent_count-1) allocated to that specific table.
 * When a single IAM page's 1,020-entry array is full (~32.6 MB of table data),
 * another IAM page is chained via `next_page_id`.
 *
 * @verbatim
 * Byte Offsets (PAGE_SIZE = 4096)
 *
 *  +--------------------------------------------------------------+ 0
 *  | next_page_id (page_id_t, 4 bytes)                            |
 *  |   - Next IAM page in this table's chain (or INVALID_PAGE_ID) |
 *  +--------------------------------------------------------------+ 4
 *  | page_lsn (lsn_t, 8 bytes)                                    |
 *  |   - LSN of the last WAL record that modified this IAM page   |
 *  +--------------------------------------------------------------+ 12
 *  | extent_count (uint16_t, 2 bytes)                             |
 *  |   - Number of valid entries in extent_ids[0..extent_count-1] |
 *  +--------------------------------------------------------------+ 14
 *  | reserved (uint16_t, 2 bytes)                                 |
 *  |   - Reserved padding to align extent_ids[] at byte 16        |
 *  +--------------------------------------------------------------+ 16
 *  | extent_ids[1020] (1020 * 4 bytes = 4080 bytes)               |
 *  |   - 4-byte extent IDs owned by this table                    |
 *  |   - 1020 extents * 8 pages * 4096 bytes = ~32.6 MB per page  |
 *  +--------------------------------------------------------------+ 4096
 * @endverbatim
 */
struct IAMPage {
  page_id_t next_page_id = INVALID_PAGE_ID;   // 4 bytes
  lsn_t page_lsn = 0;	     					  // 8 bytes
  uint16_t extent_count = 0;                  // 2 bytes
  uint16_t reserved = 0;                      // 2 bytes (alignment)
  uint32_t extent_ids[IAM_MAX_EXTENTS];       // 4080 bytes = 1020 extent IDs
  
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

  bool remove_extent(uint32_t extent_id) {
    for (int i = 0; i < extent_count; ++i) {
      if (extent_ids[i] == extent_id) {
        for (int j = i; j < extent_count - 1; ++j) {
          extent_ids[j] = extent_ids[j + 1];
        }
        extent_count--;
        return true;
      }
    }
    return false;
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
static_assert(offsetof(IAMPage, extent_ids) == 16,
              "IAMPage::extent_ids offset changed");

inline IAMPage make_iam_page() {
  IAMPage page{};
  page.next_page_id = INVALID_PAGE_ID;
  page.page_lsn = 0;
  page.extent_count = 0;
  page.reserved = 0;
  return page;
}
#pragma pack(pop)

/**
 * @class Bitmap
 * @brief Non-owning bit-array view over `GAMPage::bitmap`.
 *
 * Provides bit-level set, clear, and query operations where bit `i` represents
 * whether extent `i` within a `GAMPage` is allocated (1) or free (0).
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
