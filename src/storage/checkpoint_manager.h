//
// Created by Amit Chavan on 11/9/26.
//

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include "storage_def.h"
#include "log_record.h"

namespace letty {

// Forward declarations
class BufferPoolManager;
class LogManager;
class TransactionManager;

/**
 * @class CheckpointManager
 * @brief Manages fuzzy checkpoint orchestration and marker file I/O.
 *
 * Periodically writes a checkpoint snapshot (DPT + ATT) to the WAL and
 * persists the checkpoint marker file with the BEGIN_CHECKPOINT LSN for recovery.
 * Triggers: time-based (every N seconds) or size-based (every M bytes of WAL).
 *
 * Locking strategy:
 * - All public methods assume the caller holds no latches (checkpoint is non-blocking).
 * - Snapshot methods on BPM/TxnMgr acquire their own latches internally.
 */
class CheckpointManager {
 public:
  /**
   * @brief Constructs CheckpointManager.
   * @param db_path Database file path prefix (for marker file location).
   * @param buffer_pool Reference to BufferPoolManager (for DPT snapshot).
   * @param log_manager Reference to LogManager (for WAL operations).
   * @param txn_manager Reference to TransactionManager (for ATT snapshot).
   * @param checkpoint_interval_ms Time-based trigger interval (default 30000ms = 30s).
   * @param checkpoint_wal_size_threshold_bytes Size-based trigger threshold (default 52428800 = 50MB).
   */
  explicit CheckpointManager(const std::string& db_path,
                             BufferPoolManager& buffer_pool,
                             LogManager& log_manager,
                             TransactionManager& txn_manager,
                             uint32_t checkpoint_interval_ms = 30000,
                             uint64_t checkpoint_wal_size_threshold_bytes = 52428800);

  /**
   * @brief Checks if a checkpoint should be triggered.
   * @return true if time or WAL size threshold has been exceeded since last checkpoint.
   */
  bool should_checkpoint() const;

  /**
   * @brief Executes a fuzzy checkpoint: writes BEGIN_CHECKPOINT, snapshots DPT/ATT,
   *        writes END_CHECKPOINT, flushes WAL, and persists checkpoint marker file.
   * @return true on success, false if any step fails.
   */
  bool run_checkpoint();

  /**
   * @brief Reads the checkpoint marker file and returns the stored checkpoint LSN.
   * @return The LSN of the BEGIN_CHECKPOINT record from the last checkpoint,
   *         or INVALID_LSN if no marker file exists or is corrupt.
   */
  lsn_t read_checkpoint_marker() const;

 private:
  std::string db_path_;
  BufferPoolManager& buffer_pool_;
  LogManager& log_manager_;
  TransactionManager& txn_manager_;

  std::chrono::milliseconds checkpoint_interval_;
  uint64_t checkpoint_wal_size_threshold_;

  std::chrono::steady_clock::time_point last_checkpoint_time_;
  lsn_t last_checkpoint_lsn_ = INVALID_LSN;
  mutable std::mutex latch_;

  std::string checkpoint_marker_path() const;
  bool write_checkpoint_marker(lsn_t checkpoint_lsn) const;
};

}
