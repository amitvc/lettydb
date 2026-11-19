#include "storage/recovery_manager.h"

#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

#include "buffer/buffer_pool_manager.h"
#include "storage/log_manager.h"
#include "storage/log_record.h"
#include "storage/log_apply.h"
#include "storage/rollback_manager.h"
#include "storage/slotted_page.h"
#include "common/logger.h"


namespace letty {

RecoveryManager::RecoveryManager(std::string db_path,
								 LogManager &log_manager,
								 BufferPoolManager &buffer_pool,
								 RollbackManager &rollback_manager)
	: db_path_(std::move(db_path)),
	  log_manager_(log_manager),
	  bpm_(buffer_pool),
	  rollback_manager_(rollback_manager) {

}

lsn_t RecoveryManager::read_checkpoint_marker() const {
  if (db_path_.empty()) {
	return INVALID_LSN;
  }

  std::string marker_path = db_path_ + ".checkpoint";
  int fd = open(marker_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd == -1) {
	return INVALID_LSN;
  }

  ByteBuffer buffer(sizeof(CheckpointMarker));
  ssize_t bytes_read = pread(fd, buffer.data(), buffer.size(), 0);
  close(fd);

  if (bytes_read != static_cast<ssize_t>(buffer.size())) {
	return INVALID_LSN;
  }

  CheckpointMarker marker{};
  if (!deserialize_checkpoint_marker(buffer, marker)) {
	return INVALID_LSN;
  }

  return marker.last_checkpoint_lsn;
}

AnalysisResult RecoveryManager::analyze() {
  // Get the LSN of the most recent checkpoint that was successful.
  // This avoids recovery manager from scanning all wal log files and go straight this offset.
  lsn_t checkpoint_lsn = read_checkpoint_marker();
  return analyze_from(checkpoint_lsn);
}

AnalysisResult RecoveryManager::analyze_from(lsn_t checkpoint_lsn) {
  AnalysisResult result;

  lsn_t scan_start_lsn = (checkpoint_lsn != INVALID_LSN) ? checkpoint_lsn : WAL_SIGNATURE_SIZE;
  std::vector<LogRecord> records = log_manager_.read_records_from(scan_start_lsn);
  if (records.empty()) {
	return result;
  }

  result.scan_start_lsn = scan_start_lsn;
  LOG_RECOVERY_MGR_INFO("Starting ARIES Analysis from LSN {} (checkpoint_lsn={})",
                        scan_start_lsn, checkpoint_lsn);

  if (checkpoint_lsn != INVALID_LSN) {
	bool is_checkpoint = false;

	// Iterate through records to find begin and end checkpoint. begin checkpoint is just a marker record
	// The ATT and DTP are actually read from the EndCheckpoint record. These 2 structures are important for us to
	// identify which active transactions and dirty pages were in the system at the time check point was taken.
	for (const auto &record : records) {
	  if (record.header.lsn == checkpoint_lsn &&
		  record.header.type == LogRecordType::BEGIN_CHECKPOINT) {
		// System records such as these don't have transaction id's set.
		assert(record.header.tran_id == INVALID_TXN_ID);
		is_checkpoint = true;
	  }
	  if (is_checkpoint && record.header.lsn >= checkpoint_lsn &&
		  record.header.type == LogRecordType::END_CHECKPOINT) {
		assert(record.header.tran_id == INVALID_TXN_ID);
		apply_checkpoint_snapshot(record, result.active_transactions, result.dirty_page_table);
		LOG_RECOVERY_MGR_INFO(
		    "Applied END_CHECKPOINT snapshot at LSN {}: seeded DPT={} pages, ATT={} active txns",
		    record.header.lsn, result.dirty_page_table.size(), result.active_transactions.size());
		break;
	  }
	}
  }

  for (const auto &record : records) {
	process_log_record(record, result.active_transactions, result.dirty_page_table);
	result.last_log_lsn = record.header.lsn;
  }

  lsn_t min_rec_lsn = INVALID_LSN;
  for (const auto &[page_id, rec_lsn] : result.dirty_page_table) {
	if (min_rec_lsn == INVALID_LSN || rec_lsn < min_rec_lsn) {
	  min_rec_lsn = rec_lsn;
	}
  }
  result.min_rec_lsn = min_rec_lsn;
  LOG_RECOVERY_MGR_INFO(
      "Analysis scan finished: scanned={} records (LSN {} -> {}), min_rec_lsn={}, DPT={} pages, ATT={} loser txns",
      records.size(), scan_start_lsn, result.last_log_lsn, result.min_rec_lsn,
      result.dirty_page_table.size(), result.active_transactions.size());
  result.records_redone = redo(result);
  result.records_undone = rollback_manager_.undo_transactions(result.active_transactions);
  LOG_RECOVERY_MGR_INFO(
      "Recovery complete: DPT={} dirty pages, ATT={} loser txns, redone={} records, undone={} records",
      result.dirty_page_table.size(),
      result.active_transactions.size(),
      result.records_redone,
      result.records_undone);
  return result;
}

void RecoveryManager::apply_checkpoint_snapshot(const LogRecord &record,
												std::unordered_map<txn_id, lsn_t> &active_transaction_table,
												std::unordered_map<page_id_t, lsn_t> &dirty_page_table) {
  CheckpointPatch patch{};
  if (!deserialize_checkpoint_patch(record.payload, patch)) {
	return;
  }

  for (const auto &[page_id, rec_lsn] : patch.dirty_pages) {
	if (dirty_page_table.find(page_id) == dirty_page_table.end()) {
	  dirty_page_table[page_id] = rec_lsn;
	}
  }

  for (const auto &[tran_id, last_lsn] : patch.active_transactions) {
	auto it = active_transaction_table.find(tran_id);
	if (it == active_transaction_table.end() || it->second < last_lsn) {
	  active_transaction_table[tran_id] = last_lsn;
	}
  }
}

void RecoveryManager::process_log_record(const LogRecord &record,
										 std::unordered_map<txn_id, lsn_t> &active_transaction_table,
										 std::unordered_map<page_id_t, lsn_t> &dirty_page_table) {
  switch (record.header.type) {
	case LogRecordType::BEGIN: active_transaction_table[record.header.tran_id] = record.header.lsn;
	  break;

	case LogRecordType::COMMIT:
	case LogRecordType::END: active_transaction_table.erase(record.header.tran_id);
	  break;

	case LogRecordType::ABORT:
	  // ABORT only marks that rollback has begun; the transaction stays a loser
	  // until its END record. A crash mid-rollback is therefore resumed by recovery.
	  break;

	case LogRecordType::PAGE_UPDATE:
	  process_update<PagePatch>(record,
								active_transaction_table,
								dirty_page_table,
								deserialize_page_patch);
	  break;

	case LogRecordType::ALLOCATE_EXTENT:
	case LogRecordType::DEALLOCATE_EXTENT:
	  process_update<GamPatch>(record,
							   active_transaction_table,
							   dirty_page_table,
							   deserialize_extent_patch);
	  break;

	case LogRecordType::IAM_ADD_EXTENT:
	case LogRecordType::IAM_REMOVE_EXTENT:
	  process_update<IamPatch>(record,
							   active_transaction_table,
							   dirty_page_table,
							   deserialize_iam_patch);
	  break;

	case LogRecordType::COMPENSATION:
	  process_update<CompensationPatch>(record,
										active_transaction_table,
										dirty_page_table,
										deserialize_compensation_patch);
	  break;

	case LogRecordType::BEGIN_CHECKPOINT:
	case LogRecordType::END_CHECKPOINT:break;
  }
}

template<typename PatchType, typename Deserializer>
void RecoveryManager::process_update(const LogRecord &record,
									 std::unordered_map<txn_id, lsn_t> &active_transaction_table,
									 std::unordered_map<page_id_t, lsn_t> &dirty_page_table,
									 Deserializer deserializer) {
  if (record.header.tran_id != INVALID_TXN_ID) {
	active_transaction_table[record.header.tran_id] = record.header.lsn;
  }

  PatchType patch{};
  if (deserializer(record.payload, patch)) {
	page_id_t page_id = patch_traits<PatchType>::get_page_id(patch);
	// Add to dpt if page is not already added.
	if (dirty_page_table.find(page_id) == dirty_page_table.end()) {
	  dirty_page_table[page_id] = record.header.lsn;
	}
  }
}

page_id_t RecoveryManager::extract_page_id(const LogRecord& record) const {
  switch (record.header.type) {
    case LogRecordType::PAGE_UPDATE: {
      PagePatch patch{};
      if (deserialize_page_patch(record.payload, patch)) {
        return patch.page_id;
      }
      break;
    }
    case LogRecordType::COMPENSATION: {
      CompensationPatch patch{};
      if (deserialize_compensation_patch(record.payload, patch)) {
        return patch.page_id;
      }
      break;
    }
    case LogRecordType::ALLOCATE_EXTENT:
    case LogRecordType::DEALLOCATE_EXTENT: {
      GamPatch patch{};
      if (deserialize_extent_patch(record.payload, patch)) {
        return patch.gam_page_id;
      }
      break;
    }
    case LogRecordType::IAM_ADD_EXTENT:
    case LogRecordType::IAM_REMOVE_EXTENT: {
      IamPatch patch{};
      if (deserialize_iam_patch(record.payload, patch)) {
        return patch.iam_page_id;
      }
      break;
    }
    default:
      break;
  }
  return INVALID_PAGE_ID;
}

void RecoveryManager::materialize_data_extent(uint32_t extent_id) {
  page_id_t extent_start = static_cast<page_id_t>(extent_id * EXTENT_SIZE);
  page_id_t file_pages = bpm_.get_file_size_in_pages();
  for (uint8_t offset = 0; offset < EXTENT_SIZE; ++offset) {
    page_id_t data_page_id = extent_start + offset;
    if (data_page_id >= file_pages) {
      PageGuard data_guard = bpm_.allocate_page_guard(data_page_id);
      if (data_guard) {
        SlottedPage::init(reinterpret_cast<Byte*>(data_guard.get_data()));
        data_guard.mark_dirty();
      }
      continue;
    }
    PageGuard data_guard = bpm_.fetch_page_guard(data_page_id);
    if (!data_guard) {
      data_guard = bpm_.allocate_page_guard(data_page_id);
      if (data_guard) {
        SlottedPage::init(reinterpret_cast<Byte*>(data_guard.get_data()));
        data_guard.mark_dirty();
      }
    }
  }
}

bool RecoveryManager::redo_record(const LogRecord& record, Page* page) {
  switch (record.header.type) {
    case LogRecordType::PAGE_UPDATE: {
      PagePatch patch{};
      if (!deserialize_page_patch(record.payload, patch)) {
        LOG_RECOVERY_MGR_ERROR("Failed to deserialize PAGE_UPDATE patch");
        return false;
      }
      return apply_payload_to_page(page, patch.offset, patch.after_payload);
    }
    case LogRecordType::COMPENSATION: {
      CompensationPatch patch{};
      if (!deserialize_compensation_patch(record.payload, patch)) {
        LOG_RECOVERY_MGR_ERROR("Failed to deserialize COMPENSATION patch");
        return false;
      }
      return apply_compensation(page, patch);
    }
    case LogRecordType::ALLOCATE_EXTENT:
    case LogRecordType::DEALLOCATE_EXTENT: {
      GamPatch patch{};
      if (!deserialize_extent_patch(record.payload, patch)) {
        LOG_RECOVERY_MGR_ERROR("Failed to deserialize extent patch");
        return false;
      }
      return apply_gam_bit(page, patch.bit_index,
                           record.header.type == LogRecordType::ALLOCATE_EXTENT);
    }
    case LogRecordType::IAM_ADD_EXTENT:
    case LogRecordType::IAM_REMOVE_EXTENT: {
      IamPatch patch{};
      if (!deserialize_iam_patch(record.payload, patch)) {
        LOG_RECOVERY_MGR_ERROR("Failed to deserialize IAM patch");
        return false;
      }
      bool is_add = (record.header.type == LogRecordType::IAM_ADD_EXTENT);
      if (!apply_iam_extent(page, patch.extent_id, is_add)) {
        return false;
      }
      if (is_add) {
        materialize_data_extent(patch.extent_id);
      }
      return true;
    }
    default:
      return false;
  }
}

int RecoveryManager::redo(const AnalysisResult &analysis) {
  if (analysis.min_rec_lsn == INVALID_LSN) {
	LOG_RECOVERY_MGR_INFO("Skipping Redo phase: min_rec_lsn is INVALID_LSN (no dirty pages)");
	return 0;
  }

  std::vector<LogRecord> records = log_manager_.read_records_from(analysis.min_rec_lsn);
  int pages_redone = 0;
  int skipped_not_in_dpt = 0;
  int skipped_up_to_date = 0;
  int materialized_pages = 0;

  for (const auto &record : records) {
    page_id_t page_id = extract_page_id(record);
    if (page_id == INVALID_PAGE_ID) {
      continue;
    }

    auto dpt_it = analysis.dirty_page_table.find(page_id);
    if (dpt_it == analysis.dirty_page_table.end() || record.header.lsn < dpt_it->second) {
      skipped_not_in_dpt++;
      LOG_RECOVERY_MGR_DEBUG("Redo skipping LSN {} for page {}: not dirty at this LSN",
                             record.header.lsn, page_id);
      continue;
    }

    bool beyond_eof = (page_id >= bpm_.get_file_size_in_pages());
    PageGuard page_guard = beyond_eof
                               ? bpm_.allocate_page_guard(page_id)
                               : bpm_.fetch_page_guard(page_id);
    if (!page_guard) {
      page_guard = bpm_.allocate_page_guard(page_id);
      beyond_eof = true;
    }
    Page* page = page_guard.get_page();
    if (!page) {
      LOG_RECOVERY_MGR_WARN("Redo could not fetch or allocate page {}", page_id);
      continue;
    }
    if (beyond_eof) {
      materialized_pages++;
    }

	// If the page's LSN is greater than log records lsn then this log record has already been proceesed. Nothing to do here.
    if (page->get_page_lsn() >= record.header.lsn) {
      skipped_up_to_date++;
      LOG_RECOVERY_MGR_DEBUG("Skipping page {} because page_lsn {} >= record_lsn {}",
                             page_id, page->get_page_lsn(), record.header.lsn);
      continue;
    }

    if (!redo_record(record, page)) {
      continue;
    }

    page->set_page_lsn(record.header.lsn);
    page_guard.mark_dirty();
    pages_redone++;
  }

  LOG_RECOVERY_MGR_INFO(
      "Redo phase complete: scanned={} records from min_rec_lsn={}, applied={}, skipped_already_on_disk={}, skipped_clean_dpt={}, materialized_new_pages={}",
      records.size(), analysis.min_rec_lsn, pages_redone, skipped_up_to_date,
      skipped_not_in_dpt, materialized_pages);
  return pages_redone;
}

}
