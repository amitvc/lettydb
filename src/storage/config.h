#pragma once

#include <cstdint>
#include <cstddef>

/**
 * @file config.h
 * @brief Contains compile-time constants and type definitions for the database.
 */


namespace letty {
  static constexpr char DB_SIGNATURE[] = "LETTYDB";

  // Use a type alias for page IDs for clarity and future flexibility.
  using page_id_t = int32_t;
  using lsn_t = int32_t;
  using frame_id_t = int32_t;

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
  
  // Shared extent(s) for IAM pages and other metadata
  // First shared extent starts at extent 2 (pages 16-23),
  static constexpr page_id_t FIRST_SHARED_EXTENT_PAGE_ID = 16;
  
  // GAMPage layout: next_page_id (4 bytes) + first_free_hint (2 bytes) + bitmap
  static constexpr size_t GAM_BITMAP_ARRAY_SIZE = PAGE_SIZE - 6; // 4 bytes PAGE_ID + 2 bytes hint
  static constexpr size_t GAM_MAX_BITS = GAM_BITMAP_ARRAY_SIZE * 8;  // 32,720 bits
  
  // IAMPage layout: next_page_id (4) + extent_count (2) + reserved (2) + extent_ids[]
  // Header = 8 bytes, remaining = 4088 bytes, each extent_id = 4 bytes
  static constexpr size_t IAM_PAGE_HEADER_SIZE = 8;
  static constexpr size_t IAM_MAX_EXTENTS = (PAGE_SIZE - IAM_PAGE_HEADER_SIZE) / sizeof(uint32_t); // 1022 extents
  
  // Shared extent: 7 slots available per extent (slot 0 is directory header)
  static constexpr size_t SHARED_EXTENT_SLOTS = EXTENT_SIZE - 1; // 7 slots

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



