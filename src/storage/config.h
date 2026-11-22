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
  static constexpr uint32_t DEFAULT_WAL_SEG1MENT_SIZE = 100 * 1024 * 1024;
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
  static constexpr int DEFAULT_PAGE_SIZE = 4096; // 4kb pages
  static constexpr int PAGE_SIZE = DEFAULT_PAGE_SIZE;

  static constexpr int INVALID_PAGE_ID = -1;
  static constexpr int INVALID_FRAME_ID = -1;
  static constexpr int HEADER_PAGE_ID = 0;

  // Default buffer pool size (number of page frames)
  static constexpr size_t DEFAULT_POOL_SIZE = 256;
}



