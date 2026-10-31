//
// Created by Amit Chavan on 10/31/26.
//

#include "transaction_manager.h"

#include <optional>
#include "common/logger.h"
#include "storage/log_record.h"

namespace letty {

TransactionManager::TransactionManager(LogManager& log_manager)
    : log_manager_(log_manager) {}

uint64_t TransactionManager::begin() {
  std::lock_guard<std::mutex> lock(latch_);

  uint64_t txn_id = next_txn_id_.fetch_add(1);

  LogRecord begin_rec{};
  if (!build_transaction_record(LogRecordType::BEGIN, txn_id, begin_rec)) {
    LOG_TXN_ERROR("Failed to build BEGIN record for txn {}", txn_id);
    return 0;
  }

  lsn_t begin_lsn = log_manager_.append_record(begin_rec);
  if (begin_lsn == INVALID_LSN) {
    LOG_TXN_ERROR("Failed to append BEGIN record for txn {}", txn_id);
    return 0;
  }

  Transaction txn{txn_id, TransactionStatus::Active, begin_lsn, begin_lsn};
  transactions_.emplace(txn_id, txn);

  LOG_TXN_INFO("Transaction {} started with LSN {}", txn_id, begin_lsn);
  return txn_id;
}

bool TransactionManager::commit(uint64_t txn_id) {
  std::lock_guard<std::mutex> lock(latch_);

  auto it = transactions_.find(txn_id);
  if (it == transactions_.end()) {
    LOG_TXN_ERROR("Cannot commit unknown transaction {}", txn_id);
    return false;
  }

  if (it->second.status != TransactionStatus::Active) {
    LOG_TXN_ERROR("Transaction {} is not active (status: {})",
                  txn_id, static_cast<int>(it->second.status));
    return false;
  }

  LogRecord commit_rec{};
  if (!build_transaction_record(LogRecordType::COMMIT, txn_id, commit_rec)) {
    LOG_TXN_ERROR("Failed to build COMMIT record for txn {}", txn_id);
    return false;
  }

  lsn_t commit_lsn = log_manager_.append_record(commit_rec);
  if (commit_lsn == INVALID_LSN) {
    LOG_TXN_ERROR("Failed to append COMMIT record for txn {}", txn_id);
    return false;
  }

  it->second.last_lsn = commit_lsn;

  // Block until all WAL records for this transaction are on disk.
  if (!log_manager_.flush_to_lsn(commit_lsn)) {
    LOG_TXN_ERROR("Failed to flush WAL for transaction {}", txn_id);
    return false;
  }

  it->second.status = TransactionStatus::Committed;

  LOG_TXN_INFO("Transaction {} committed at LSN {}", txn_id, commit_lsn);
  return true;
}

bool TransactionManager::abort(uint64_t txn_id) {
  std::lock_guard<std::mutex> lock(latch_);

  auto it = transactions_.find(txn_id);
  if (it == transactions_.end()) {
    LOG_TXN_ERROR("Cannot abort unknown transaction {}", txn_id);
    return false;
  }

  if (it->second.status != TransactionStatus::Active) {
    LOG_TXN_ERROR("Transaction {} is not active (status: {})",
                  txn_id, static_cast<int>(it->second.status));
    return false;
  }

  LogRecord abort_rec{};
  if (!build_transaction_record(LogRecordType::ABORT, txn_id, abort_rec)) {
    LOG_TXN_ERROR("Failed to build ABORT record for txn {}", txn_id);
    return false;
  }

  lsn_t abort_lsn = log_manager_.append_record(abort_rec);
  if (abort_lsn == INVALID_LSN) {
    LOG_TXN_ERROR("Failed to append ABORT record for txn {}", txn_id);
    return false;
  }

  it->second.last_lsn = abort_lsn;
  it->second.status = TransactionStatus::Aborted;

  LOG_TXN_INFO("Transaction {} aborted at LSN {}", txn_id, abort_lsn);
  return true;
}

void TransactionManager::record_lsn(uint64_t txn_id, lsn_t lsn) {
  std::lock_guard<std::mutex> lock(latch_);

  auto it = transactions_.find(txn_id);
  if (it == transactions_.end()) {
    LOG_TXN_WARN("Cannot record LSN for unknown transaction {}", txn_id);
    return;
  }
  it->second.last_lsn = lsn;
}

std::optional<TransactionStatus> TransactionManager::get_status(uint64_t txn_id) const {
  std::lock_guard<std::mutex> lock(latch_);

  auto it = transactions_.find(txn_id);
  if (it == transactions_.end()) {
    return std::nullopt;
  }
  return it->second.status;
}

lsn_t TransactionManager::get_next_lsn() const {
  return log_manager_.get_next_lsn();
}

lsn_t TransactionManager::get_flushed_lsn() const {
  return log_manager_.get_flushed_lsn();
}

}
