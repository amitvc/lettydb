//
// Created by Amit Chavan on 11/17/26.
//

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "storage/config.h"
#include "storage/log_record.h"

namespace letty {

class LogManager;
class BufferPoolManager;
class Page;

/**
 * @class RollbackManager
 * @brief The ARIES undo_transactions engine. Reverses updates by applying their inverse and
 * logging a Compensation Log Record (CLR) for each, so rollback is itself
 * recoverable. Used both by crash-recovery undo_transactions and by transactions that the user aborts
 */
class RollbackManager {
 public:
  RollbackManager(LogManager& log_manager, BufferPoolManager& buffer_pool, std::string db_path);

  /**
   * @brief Recovery Undo: roll back every loser transaction.
   *
   * Processes loser records in strictly descending LSN order (interleaved
   * across transactions) so updates to a shared page are reversed newest-first.
   * Writes a CLR per undone update and an END record when a transaction's chain
   * reaches its BEGIN. CLRs are skipped (never re-undone) via undo_next_lsn.
   *
   * @param active_transactions Loser ATT: txn_id -> last LSN written.
   * @return Count of updates undone.
   */
  int undo_transactions(const std::unordered_map<txn_id, lsn_t>& active_transactions);

  /**
   * @brief Voluntary abort: roll back a single transaction.
   *
   * Walks tran_id's prev_lsn chain backward from last_lsn, applying the inverse
   * and logging a CLR for each update, skipping CLRs via undo_next_lsn, until it
   * reaches the transaction's BEGIN. Writes an END record on completion — the
   * sole terminal marker, so a crash mid-rollback leaves the transaction a loser
   * that recovery resumes.
   *
   * @param tran_id Transaction being aborted.
   * @param last_lsn Most recent LSN this transaction wrote (its ABORT marker).
   */
  void rollback_transaction(txn_id tran_id, lsn_t last_lsn);

 private:
  bool build_undo_clr(const LogRecord& record, CompensationPatch& clr) const;
  bool undo_record(txn_id tran_id, const LogRecord& record, lsn_t& last_written_lsn);
  void write_end_record(txn_id tran_id, lsn_t last_written_lsn);

  LogManager& log_manager_;
  BufferPoolManager& bpm_;
  std::string db_path_;
};

}
