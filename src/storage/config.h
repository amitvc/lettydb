#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

/**
 * @file config.h
 * @brief Contains compile-time constants and type definitions for the database.
 */


namespace letty {
  static constexpr char DB_SIGNATURE[] = "LETTYDB";
  static constexpr char WAL_SIGNATURE[] = "LETTYWAL";
  static constexpr size_t WAL_SIGNATURE_SIZE = 8;
  static constexpr uint32_t DEFAULT_WAL_SEGMENT_SIZE = 10 * 1024 * 1024;
  static constexpr uint32_t DEFAULT_WAL_FLUSH_INTERVAL_MS = 5;

  // Use a type alias for page IDs for clarity and future flexibility.
  using page_id_t = int32_t;
  using lsn_t = uint64_t;
  using txn_id = uint64_t;
  using Byte = uint8_t;
  using ByteBuffer = std::vector<Byte>;
  // Index into the buffer pool's fixed-size frame array. A frame is a slot that
  // can hold any page — the page_table_ maps page_id → frame_id at runtime.
  using frame_id_t = int32_t;

  static constexpr lsn_t INVALID_LSN = 0;
  static constexpr txn_id INVALID_TXN_ID = 0;

  /**
   * @brief The size of a single page in bytes.
   *
   * All disk I/O will be in chunk size of 4096 bytes.
   */
  static constexpr int PAGE_SIZE = 4096; // 4kb pages

  static constexpr int EXTENT_SIZE = 8;       // 8 pages per extent
  static constexpr int INVALID_PAGE_ID = -1;
  static constexpr int INVALID_FRAME_ID = -1;
  static constexpr int HEADER_PAGE_ID = 0;
  static constexpr page_id_t FIRST_GAM_PAGE_ID = 8; // First gam page is allocated in extent 1 which covers page 8-15

  // GAMPage layout: next_page_id (4) + page_lsn (8) + first_free_bit_hint (2) + bitmap (4082)
  static constexpr size_t GAM_BITMAP_ARRAY_SIZE = PAGE_SIZE - 14; // 4082 bytes
  static constexpr size_t GAM_MAX_BITS = GAM_BITMAP_ARRAY_SIZE * 8;  // 32,656 bits (extents per GAM page)
  
  // IAMPage layout: next_page_id (4) + page_lsn (8) + extent_count (2) + reserved (2) + extent_ids[] (4080)
  // Header = 16 bytes, remaining = 4080 bytes, each extent_id = 4 bytes
  static constexpr size_t IAM_PAGE_HEADER_SIZE = 16;
  static constexpr size_t IAM_MAX_EXTENTS = (PAGE_SIZE - IAM_PAGE_HEADER_SIZE) / sizeof(uint32_t); // 1020 extents
  
  // Default buffer pool size (number of page frames)
  static constexpr size_t DEFAULT_POOL_SIZE = 256;

  /** @brief Converts a starting page ID to its extent index. */
  constexpr uint32_t extent_id_from_page(page_id_t page_id) {
    return static_cast<uint32_t>(page_id / EXTENT_SIZE);
  }

  /** @brief Converts an extent index to the first page ID in that extent. */
  constexpr page_id_t first_page_of_extent(uint32_t extent_id) {
    return static_cast<page_id_t>(extent_id * EXTENT_SIZE);
  }
}



