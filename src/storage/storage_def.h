#pragma once

/**
 * @file storage_def.h
 * @brief On-disk metadata page structures and layouts for LettyDB.
 *
 * LettyDB writes all data to a single database file with a .db extension.
 * The file is divided into fixed-size blocks called Pages (DEFAULT_PAGE_SIZE = 4096 bytes).
 *
 * Every page starts with a 20-byte PageHeader holding page_lsn, prev/next
 * page links, table_id, and page_type at fixed offsets across the file.
 *
 * Page 0 holds DatabaseMetadata in its body: signature, version, table_count,
 * next_table_id, page_count, free_page_list_head_page, and the first page IDs
 * for sys_tables and sys_columns.
 *
 * @verbatim
 * High-Level Directory Layout
 *
 *  +-------------------------------------------------------------------+
 *  | Page 0: PageHeader | DatabaseMetadata (Superblock)                |
 *  |   free_page_list_head_page -> [Page X] -> [Page Y] -> INVALID     |
 *  |   sys_tables_first_page    -> [Page 1] <-> [Page ...]             |
 *  |   sys_columns_first_page   -> [Page 2] <-> [Page ...]             |
 *  +-------------------------------------------------------------------+
 * @endverbatim
 */

#include "config.h"
#include <cstddef>
#include <cstring>
#include <type_traits>


namespace letty {

enum class PageType : uint8_t {
  INVALID         = 0,
  DATABASE_HEADER = 1,
  DATA            = 2,
  INDEX           = 3,
  FREE            = 4,
};

// Disable compiler-inserted alignment padding so struct layouts match
// their on-disk byte offsets exactly.
// See: https://learn.microsoft.com/en-us/cpp/preprocessor/pack
#pragma pack(push, 1)
struct PageHeader {
  lsn_t page_lsn = 0;
  page_id_t next_page_id = INVALID_PAGE_ID;
  page_id_t prev_page_id = INVALID_PAGE_ID;
  uint16_t table_id = 0;
  PageType page_type = PageType::INVALID;
  uint8_t reserved = 0;
};

static_assert(sizeof(PageHeader) == 20,
              "PageHeader must be exactly 20 bytes on disk");
static_assert(std::is_trivially_copyable_v<PageHeader>,
              "PageHeader must be safe to copy to/from page bytes");
static_assert(std::is_standard_layout_v<PageHeader>,
              "PageHeader must have stable field offsets");

static constexpr size_t PAGE_HEADER_SIZE = sizeof(PageHeader);
static constexpr size_t PAGE_BODY_SIZE = DEFAULT_PAGE_SIZE - PAGE_HEADER_SIZE;

/**
 * @struct DatabaseMetadata
 * @brief Represents some metadata about the database that is essential for the db engine to bootstrap.
 * If this record gets corrupt we cannot bootstrap the database easily.
 *
 * Stored in the body of Page 0 (starting at byte offset PAGE_HEADER_SIZE = 20).
 * Page 0's PageHeader sits at bytes 0..19, and DatabaseMetadata occupies the
 * start of the 4076-byte body.
 *
 * @verbatim
 * Body Byte Offsets (relative to Page::get_body(), starting at byte 20 of Page 0)
 *
 *  +--------------------------------------------------------------+ 0
 *  | signature[8]   ("LETTYDB\0")                                 |
 *  |   - Identifies this file as a LettyDB database               |
 *  +--------------------------------------------------------------+ 8
 *  | version (uint8_t, 1 byte)                                    |
 *  |   - Database file format version (currently 1)               |
 *  +--------------------------------------------------------------+ 9
 *  | page_size (uint32_t, 4 bytes)                                |
 *  |   - Size of each page in bytes (4096)                        |
 *  +--------------------------------------------------------------+ 13
 *  | table_count (uint16_t, 2 bytes)                              |
 *  |   - Total active table count.Max it can hold is 65k          |
 *  +--------------------------------------------------------------+ 15
 *  | next_table_id (uint16_t, 2 bytes)                            |
 *  |   - Next available table OID (user tables start at 100)      |
 *  +--------------------------------------------------------------+ 17
 *  | page_count  (uint32_t, 4 bytes)                              |
 *  |   - Tracks the total number of pages allocated for the db    |
 *  +--------------------------------------------------------------+ 21
 *  | free_page_list_head_page (page_id_t, 4 bytes)                |
 *  |   - Head of the free page list (INVALID_PAGE_ID if empty)    |
 *  +--------------------------------------------------------------+ 25
 *  | sys_tables_first_page (page_id_t, 4 bytes)                   |
 *  |   - First page of sys_tables system catalog                  |
 *  +--------------------------------------------------------------+ 29
 *  | sys_columns_first_page (page_id_t, 4 bytes)                  |
 *  |   - First page of sys_columns system catalog                 |
 *  +--------------------------------------------------------------+ 33
 * @endverbatim
 */
struct DatabaseMetadata {
  char signature[8];
  uint8_t version = 1;
  uint32_t page_size = DEFAULT_PAGE_SIZE;
  uint16_t table_count = 0;
  uint16_t next_table_id = 100;
  uint32_t page_count = 1;
  page_id_t free_page_list_head_page = INVALID_PAGE_ID;
  // This convention is enforced in code. Page 1 & 2 are dedicated to Table catalog.
  page_id_t sys_tables_first_page = 1;
  page_id_t sys_columns_first_page = 2;

  DatabaseMetadata() {
    std::memset(signature, 0, sizeof(signature));
    std::memcpy(signature, DB_SIGNATURE, sizeof(DB_SIGNATURE));
  }
};
#pragma pack(pop)

/**
 * @struct DbMetadataState
 * @brief Sub-structure holding the mutable fields of DatabaseMetadata.
 */
struct DbMetadataState {
  page_id_t free_page_list_head_page = INVALID_PAGE_ID;
  uint32_t page_count = 1;
  uint16_t table_count = 0;
  uint16_t next_table_id = 100;
};

static_assert(sizeof(DatabaseMetadata) == 33,
              "DatabaseMetadata must be exactly 33 bytes on disk");
static_assert(sizeof(DatabaseMetadata) <= PAGE_BODY_SIZE,
              "DatabaseMetadata must fit inside PAGE_BODY_SIZE");
static_assert(std::is_trivially_copyable_v<DatabaseMetadata>,
              "DatabaseMetadata must be safe to copy to/from page bytes");
static_assert(std::is_standard_layout_v<DatabaseMetadata>,
              "DatabaseMetadata must have stable field offsets");
static_assert(offsetof(DatabaseMetadata, version) == 8,
              "DatabaseMetadata::version offset changed");

/**
 * @brief Creates a default-initialized DatabaseMetadata payload for a new DB.
 */
inline DatabaseMetadata make_database_metadata() {
  DatabaseMetadata meta{};
  std::memset(meta.signature, 0, sizeof(meta.signature));
  std::memcpy(meta.signature, DB_SIGNATURE, sizeof(DB_SIGNATURE));
  meta.version = 1;
  meta.page_size = DEFAULT_PAGE_SIZE;
  meta.table_count = 0;
  meta.next_table_id = 100;
  meta.page_count = 1;
  meta.free_page_list_head_page = INVALID_PAGE_ID;
  meta.sys_tables_first_page = 1;
  meta.sys_columns_first_page = 2;
  return meta;
}
}
