//
// Created by Amit Chavan on 10/31/26.
//

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "storage/log_manager.h"

namespace letty {

enum class TransactionStatus {
  Active,
  Committed,
  Aborted,
};

struct Transaction {
  uint64_t txn_id;
  TransactionStatus status;
  lsn_t begin_lsn;
  // LSN of the last WAL record for the transaction. This field keeps getting updated as Transaction manager keeps
  // processing statements in the current transaction.
  lsn_t last_lsn;
};

/**
 * @class TransactionManager
 * @brief Manages transaction lifecycle: begin, commit, rollback.
 *
 * Locking strategy:
 * - Single std::mutex (latch_) protects transaction state and LogManager access.
 * - Each transaction is assigned a unique txn_id and tracked until commit/abort.
 *
 * WAL protocol:
 * - begin() appends a BEGIN record, returns the assigned txn_id.
 * - commit(txn_id) appends a COMMIT record, then calls flush_to_lsn() to ensure
 *   durability. Returns only after fsync completes.
 * - abort(txn_id) appends an ABORT record and marks the transaction as aborted.
 *   Rollback of modified pages is handled separately during recovery.
 */
class TransactionManager {
 public:
  explicit TransactionManager(LogManager& log_manager);

  /**
   * Marks the beginning of the transaction.
   * @return The transaction_id assigned to it.
   */
  uint64_t begin();

  /**
   * This marks the completion of a transaction by written COMMIT wal record.
   * @param txn_id
   * @return
   */
  bool commit(uint64_t txn_id);

  /** @brief Aborts a transaction. Appends ABORT record. */
  bool abort(uint64_t txn_id);

  /** @brief Records a WAL LSN against the transaction (called after each write operation). */
  void record_lsn(uint64_t txn_id, lsn_t lsn);

  /** @brief Returns the transaction status, or std::nullopt if not found. */
  std::optional<TransactionStatus> get_status(uint64_t txn_id) const;

  /** @brief Returns the next LSN that will be assigned (for testing/debugging). */
  lsn_t get_next_lsn() const;

  /** @brief Returns the highest LSN known to be on disk. */
  lsn_t get_flushed_lsn() const;

 private:
  LogManager& log_manager_;
  mutable std::mutex latch_;
  std::atomic<uint64_t> next_txn_id_{1};
  std::unordered_map<uint64_t, Transaction> transactions_;
};

}
