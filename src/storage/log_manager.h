#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "log_record.h"

namespace letty {

/**
 * @class LogManager
 * @brief Manages appending, buffering, flushing, and reading WAL records across segmented WAL files.
 *
 * Locking strategy:
 * - Single std::mutex (latch_) protects all public operations.
 * - append_record() and flush_to_lsn() acquire the latch for the duration of their work.
 *
 * The WAL is split into fixed-capacity segment files named `<file_prefix>.<segment_id>`
 * (e.g., `letty.wal.0`, `letty.wal.1`). Every segment file begins with an 8-byte
 * magic header (`WAL_SIGNATURE` = `"LETTYWAL"`), so the first record in any segment
 * starts at file offset `WAL_SIGNATURE_SIZE` (`8`), and `INVALID_LSN` (`0`) is never
 * a valid record address.
 *
 * The LSN(LogSequenceNumber) encodes the wal segment id and the offset within the file where the record is located.
 * This allows the RecoveryManager to jump to the exact location in the WAL file when needed.
 * - `segment_id = lsn / segment_size_`
 * - `file_offset = lsn % segment_size_`
 * - `lsn = (segment_id * segment_size_) + file_offset`
 *
 * `append_record()` serializes records into an in-memory `log_buffer_` and flushes
 * the batch to disk (`pwrite` + `fsync`) when `flush_to_lsn()` is called, when
 * `flush_interval_` (`5 ms` by default) has elapsed since the buffer started
 * accumulating records, or before rolling over to a new WAL segment.
 */
class LogManager {
 public:
  /**
   * @brief Opens or creates the WAL segment files.
   * Writes the 8-byte WAL_SIGNATURE if no segment exists yet, or locates the
   * latest segment, verifies its signature, and sets next_lsn_ from its file size.
   * @param file_prefix Path prefix for WAL segment files (e.g., "letty.wal").
   * @param segment_size Maximum size in bytes a single WAL segment file can reach before rollover.
   * @param flush_interval_ms Time window in milliseconds before buffered records are auto-flushed.
   * @throws DbException if the segment file cannot be opened or has an invalid signature.
   */
  explicit LogManager(std::string file_prefix,
                      uint32_t segment_size = DEFAULT_WAL_SEGMENT_SIZE,
                      uint32_t flush_interval_ms = DEFAULT_WAL_FLUSH_INTERVAL_MS);

  /** @brief Closes the WAL file. */
  ~LogManager();

  LogManager(const LogManager&) = delete;
  LogManager& operator=(const LogManager&) = delete;

  /**
   * @brief Assigns an LSN to the record, serializes it with a CRC32 checksum,
   *        and writes it to the WAL file.
   * @param record The record to append (record.header.lsn is updated in place).
   * @return The assigned LSN (> 0) on success, or INVALID_LSN (0) on failure.
   */
  lsn_t append_record(LogRecord& record);

  /**
   * @brief Ensures all WAL records up to target_lsn are synced to stable storage.
   * @param target_lsn The LSN that must be durable on disk before returning.
   * @return true on success, false if fsync failed.
   */
  bool flush_to_lsn(lsn_t target_lsn);

  /**
   * @brief Reads a single WAL record starting at the given byte-offset LSN.
   * @param lsn Byte offset in the WAL file (>= WAL_HEADER_SIZE).
   * @return The deserialized LogRecord, or std::nullopt if out of range or corrupt.
   */
  std::optional<LogRecord> read_record_at(lsn_t lsn);

  /**
   * @brief Resets the log back to just the 8-byte WAL header. Call only after a checkpoint.
   * @return true on success.
   */
  bool truncate();

  /**
   * @brief Reads all valid records sequentially from WAL_HEADER_SIZE.
   * Stops at EOF or the first incomplete/corrupt record (e.g., torn tail write).
   * @return Valid records in append order.
   */
  std::vector<LogRecord> read_records();

  /** @brief Returns the LSN that will be assigned to the next appended record. */
  lsn_t get_next_lsn() const { return next_lsn_; }

  /** @brief Returns the highest LSN known to be synced to disk. */
  lsn_t get_flushed_lsn() const { return flushed_lsn_; }


  /** @brief Returns the currently active WAL segment ID. */
  uint32_t get_current_segment_id() const { return current_segment_id_; }

 private:
  uint32_t segment_id_from_lsn(lsn_t lsn) const {
    return static_cast<uint32_t>(lsn / segment_size_);
  }

  uint32_t calculate_file_offset_from_lsn(lsn_t lsn) const {
    return static_cast<uint32_t>(lsn % segment_size_);
  }

  lsn_t build_lsn(uint32_t segment_id, size_t offset) const {
    return (static_cast<lsn_t>(segment_id) * segment_size_) + offset;
  }

  std::string segment_file_path(uint32_t segment_id) const {
    return file_prefix_ + "." + std::to_string(segment_id);
  }

  std::optional<std::pair<uint32_t, size_t>> find_latest_segment() const;
  std::vector<uint32_t> list_segment_ids() const;
  int create_segment_file(uint32_t segment_id);
  int open_segment_file(uint32_t segment_id);
  bool flush_buffer_to_disk();
  bool rollover_to_next_segment();
  std::optional<LogRecord> read_record_from_fd(int fd, lsn_t expected_lsn, uint32_t offset) const;
  std::optional<LogRecord> read_record_from_buffer(lsn_t expected_lsn) const;

  std::string file_prefix_;
  uint32_t segment_size_;
  std::chrono::milliseconds flush_interval_;
  std::chrono::steady_clock::time_point last_flush_time_{};
  ByteBuffer log_buffer_;
  int fd_ = -1;
  // We use the file offset where the wal records is going to be written as the log sequence number.
  // This gives us unique and monotonically increasing number. Every wal file starts with wal signature. So the starting LSN for each new file will
  // always be  WAL_SIGNATURE_SIZE
  lsn_t next_lsn_ = WAL_SIGNATURE_SIZE;
  lsn_t flushed_lsn_ = 0;
  uint32_t current_segment_id_ = 0;

  mutable std::mutex latch_;
};

}  // namespace letty
