//
// Created by Amit Chavan on 11/17/26.
//

#include "rollback_manager.h"

#include <optional>
#include <utility>

#include "buffer/buffer_pool_manager.h"
#include "storage/log_manager.h"
#include "storage/log_record.h"
#include "storage/log_apply.h"
#include "common/logger.h"

namespace letty {

RollbackManager::RollbackManager(LogManager& log_manager, BufferPoolManager& buffer_pool, std::string db_path)
    : log_manager_(log_manager),
      bpm_(buffer_pool),
      db_path_(std::move(db_path)) {}

bool RollbackManager::build_undo_clr(const LogRecord& record, CompensationPatch& clr) const {
  switch (record.header.type) {
    case LogRecordType::PAGE_UPDATE: {
      PagePatch patch{};
      if (!deserialize_page_patch(record.payload, patch)) {
        LOG_RECOVERY_MGR_ERROR("Failed to deserialize PAGE_UPDATE patch during undo_transactions");
        return false;
      }
      clr.page_id = patch.page_id;
      clr.kind = CompensationKind::BytePatch;
      clr.offset = patch.offset;
      clr.payload = patch.before_payload;
      break;
    }
    case LogRecordType::ALLOCATE_EXTENT:
    case LogRecordType::DEALLOCATE_EXTENT: {
      GamPatch patch{};
      if (!deserialize_extent_patch(record.payload, patch)) {
        LOG_RECOVERY_MGR_ERROR("Failed to deserialize extent patch during undo_transactions");
        return false;
      }
      clr.page_id = patch.gam_page_id;
      clr.kind = (record.header.type == LogRecordType::ALLOCATE_EXTENT)
                     ? CompensationKind::GamClearBit : CompensationKind::GamSetBit;
      clr.param = patch.bit_index;
      break;
    }
    case LogRecordType::IAM_ADD_EXTENT:
    case LogRecordType::IAM_REMOVE_EXTENT: {
      IamPatch patch{};
      if (!deserialize_iam_patch(record.payload, patch)) {
        LOG_RECOVERY_MGR_ERROR("Failed to deserialize IAM patch during undo_transactions");
        return false;
      }
      clr.page_id = patch.iam_page_id;
      clr.kind = (record.header.type == LogRecordType::IAM_ADD_EXTENT)
                     ? CompensationKind::IamRemoveExtent : CompensationKind::IamAddExtent;
      clr.param = patch.extent_id;
      break;
    }
    default:
      return false;
  }
  // undo_next_lsn is where this transaction's undo resumes next: the prev_lsn
  // of the record being compensated.
  clr.undo_next_lsn = record.header.prev_lsn;
  return true;
}

bool RollbackManager::undo_record(txn_id tran_id, const LogRecord& record, lsn_t& last_written_lsn) {
  CompensationPatch clr{};
  if (!build_undo_clr(record, clr)) {
    return false;
  }

  LogRecord clr_record{};
  if (!build_compensation_patch_record(tran_id, clr, clr_record)) {
    LOG_RECOVERY_MGR_ERROR("Failed to build CLR for txn {} page {}", tran_id, clr.page_id);
    return false;
  }
  clr_record.header.prev_lsn = last_written_lsn;

  lsn_t clr_lsn = log_manager_.append_record(clr_record);
  if (clr_lsn == INVALID_LSN) {
    LOG_RECOVERY_MGR_ERROR("Failed to append CLR for txn {} page {}", tran_id, clr.page_id);
    return false;
  }

  // Only mutate the page once the CLR justifying that mutation is durably
  // logged. Otherwise a crash between the write and this point would leave
  // the page undone but its page_lsn still pointing at the original
  // (now-undone) record, which a later redo pass would re-apply.
  PageGuard page_guard = bpm_.fetch_page_guard(clr.page_id);
  Page* page = page_guard.get_page();
  if (!page) {
    LOG_RECOVERY_MGR_ERROR("Undo could not fetch page {} for txn {}", clr.page_id, tran_id);
    return false;
  }

  if (!apply_compensation(page, clr)) {
    return false;
  }

  bpm_.update_dirtied_page_lsn(clr.page_id, clr_lsn);
  page->set_page_lsn(clr_lsn);
  page_guard.mark_dirty();
  last_written_lsn = clr_lsn;
  return true;
}

void RollbackManager::write_end_record(txn_id tran_id, lsn_t last_written_lsn) {
  LogRecord end_record{};
  if (!build_transaction_record(LogRecordType::END, tran_id, end_record)) {
    LOG_RECOVERY_MGR_ERROR("Failed to build END record for txn {}", tran_id);
    return;
  }
  end_record.header.prev_lsn = last_written_lsn;
  log_manager_.append_record(end_record);
}

void RollbackManager::rollback_transaction(txn_id tran_id, lsn_t last_lsn) {
  // Aborting one transaction is undoing a loser set of size one: the sweep
  // walks its chain to BEGIN, writing CLRs and a terminal END.
  undo_transactions({{tran_id, last_lsn}});
}

int RollbackManager::undo_transactions(const std::unordered_map<txn_id, lsn_t>& active_transactions) {
  if (active_transactions.empty()) {
    return 0;
  }

  std::unordered_map<txn_id, lsn_t> next_lsn_to_undo = active_transactions;
  std::unordered_map<txn_id, lsn_t> last_written_lsn = active_transactions;
  int pages_undone = 0;

  while (!next_lsn_to_undo.empty()) {
    txn_id current_txn = INVALID_TXN_ID;
    lsn_t current_lsn = INVALID_LSN;
    for (const auto& [txn, lsn] : next_lsn_to_undo) {
      if (current_lsn == INVALID_LSN || lsn > current_lsn) {
        current_txn = txn;
        current_lsn = lsn;
      }
    }

    std::optional<LogRecord> record_opt = log_manager_.read_record_at(current_lsn);
    if (!record_opt.has_value()) {
      LOG_RECOVERY_MGR_ERROR("Undo could not find log record for LSN {} (txn {})", current_lsn, current_txn);
      next_lsn_to_undo.erase(current_txn);
      continue;
    }
    const LogRecord& record = record_opt.value();

    if (record.header.type == LogRecordType::BEGIN) {
      write_end_record(current_txn, last_written_lsn[current_txn]);
      next_lsn_to_undo.erase(current_txn);
      continue;
    }

    // A CLR is redo-only — never undone. Skip straight to the record it says to
    // undo next, so work already compensated in a prior crashed recovery isn't redone.
    if (record.header.type == LogRecordType::COMPENSATION) {
      CompensationPatch clr_patch{};
      if (deserialize_compensation_patch(record.payload, clr_patch)) {
        next_lsn_to_undo[current_txn] = clr_patch.undo_next_lsn;
      } else {
        LOG_RECOVERY_MGR_ERROR("Failed to deserialize COMPENSATION patch during undo_transactions");
        next_lsn_to_undo[current_txn] = record.header.prev_lsn;
      }
      continue;
    }

    // Any page-modifying record is reversed by applying its inverse and logging
    // a CLR. undo_record returns false for records it can't undo (which never
    // appear in a loser's chain), leaving the chain to advance normally.
    if (undo_record(current_txn, record, last_written_lsn[current_txn])) {
      pages_undone++;
    }
    next_lsn_to_undo[current_txn] = record.header.prev_lsn;
  }

  return pages_undone;
}

}
