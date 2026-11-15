#include "storage/recovery_manager.h"

#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

#include "buffer/buffer_pool_manager.h"
#include "storage/log_manager.h"
#include "storage/log_record.h"
#include "common/logger.h"


namespace letty {

RecoveryManager::RecoveryManager(std::string db_path,
								 LogManager &log_manager,
								 BufferPoolManager &buffer_pool)
	: db_path_(std::move(db_path)),
	  log_manager_(log_manager),
	  bpm_(buffer_pool) {

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

  if (checkpoint_lsn != INVALID_LSN) {
	bool is_checkpoint = false;
	bool found_end_checkpoint = false;

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
		found_end_checkpoint = true;
		break;
	  }
	}

	if (is_checkpoint && !found_end_checkpoint) {
	  scan_start_lsn = WAL_SIGNATURE_SIZE;
	  result.scan_start_lsn = scan_start_lsn;
	  records = log_manager_.read_records_from(scan_start_lsn);
	}
  }

  for (const auto &record : records) {
	if (record.header.lsn < scan_start_lsn) {
	  continue;
	}

	if (record.header.type == LogRecordType::BEGIN_CHECKPOINT ||
		record.header.type == LogRecordType::END_CHECKPOINT) {
	  result.last_log_lsn = record.header.lsn;
	  continue;
	}

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
  redo(result);
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
	case LogRecordType::ABORT:
	case LogRecordType::END: active_transaction_table.erase(record.header.tran_id);
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

bool RecoveryManager::is_page_modifying_record(const LogRecord& record) const {
  return record.header.type == LogRecordType::PAGE_UPDATE ||
         record.header.type == LogRecordType::COMPENSATION;
}

page_id_t RecoveryManager::extract_page_id(const LogRecord& record) const {
  if (record.header.type == LogRecordType::PAGE_UPDATE) {
    PagePatch patch{};
    if (deserialize_page_patch(record.payload, patch)) {
      return patch.page_id;
    }
  } else if (record.header.type == LogRecordType::COMPENSATION) {
    CompensationPatch patch{};
    if (deserialize_compensation_patch(record.payload, patch)) {
      return patch.page_id;
    }
  }
  return INVALID_PAGE_ID;
}

bool RecoveryManager::apply_payload_to_page(Page* page, uint16_t offset,
                                             const ByteBuffer& payload) const {
  if (offset + payload.size() > PAGE_SIZE) {
    LOG_RECOVERY_MGR_WARN("Patch offset {} + size {} exceeds page size",
                          offset, payload.size());
    return false;
  }
  std::memcpy(page->get_data() + offset, payload.data(), payload.size());
  return true;
}

uint64_t RecoveryManager::redo(const AnalysisResult &analysis) {
  if (analysis.min_rec_lsn == INVALID_LSN) {
	LOG_RECOVERY_MGR_WARN("Can't continue the redo phase since min_rec_lsn is invalid");
	return 0;
  }


  std::vector<LogRecord> records = log_manager_.read_records_from(analysis.min_rec_lsn);
  uint64_t pages_redone = 0;

  for (const auto &record : records) {
    if (!is_page_modifying_record(record)) {
      continue;
    }

    page_id_t page_id = extract_page_id(record);
    if (page_id == INVALID_PAGE_ID) {
      continue;
    }

    if (analysis.dirty_page_table.find(page_id) == analysis.dirty_page_table.end()) {
      continue;
    }

    PageGuard page_guard = bpm_.fetch_page_guard(page_id);
    Page* page = page_guard.get_page();
    if (!page) {
      continue;
    }

    if (page->get_page_lsn() >= record.header.lsn) {
      continue;
    }

    if (record.header.type == LogRecordType::PAGE_UPDATE) {
      PagePatch patch{};
      if (!deserialize_page_patch(record.payload, patch)) {
        LOG_RECOVERY_MGR_ERROR("Failed to deserialize PAGE_UPDATE patch");
        continue;
      }
      if (!apply_payload_to_page(page, patch.offset, patch.after_payload)) {
        continue;
      }
    } else if (record.header.type == LogRecordType::COMPENSATION) {
      CompensationPatch patch{};
      if (!deserialize_compensation_patch(record.payload, patch)) {
        LOG_RECOVERY_MGR_ERROR("Failed to deserialize COMPENSATION patch");
        continue;
      }
      if (!apply_payload_to_page(page, patch.offset, patch.payload)) {
        continue;
      }
    }

    page->set_page_lsn(record.header.lsn);
    pages_redone++;
  }

  return pages_redone;
}

}
