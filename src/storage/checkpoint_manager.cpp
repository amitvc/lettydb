#include "checkpoint_manager.h"

#include <cassert>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "buffer/buffer_pool_manager.h"
#include "log_manager.h"
#include "transaction/transaction_manager.h"
#include "common/logger.h"
#include "common/db_exception.h"
#include "page_utils.h"

namespace letty {

CheckpointManager::CheckpointManager(const std::string& db_path,
                                     BufferPoolManager& buffer_pool,
                                     LogManager& log_manager,
                                     TransactionManager& txn_manager,
                                     uint32_t checkpoint_interval_ms,
                                     uint64_t checkpoint_wal_size_threshold_bytes)
    : db_path_(db_path),
      buffer_pool_(buffer_pool),
      log_manager_(log_manager),
      txn_manager_(txn_manager),
      checkpoint_interval_(checkpoint_interval_ms),
      checkpoint_wal_size_threshold_(checkpoint_wal_size_threshold_bytes),
      last_checkpoint_time_(std::chrono::steady_clock::now()) {
  last_checkpoint_lsn_ = read_checkpoint_marker();
  if (last_checkpoint_lsn_ == INVALID_LSN) {
    last_checkpoint_lsn_ = log_manager_.get_next_lsn();
  }
}

std::string CheckpointManager::checkpoint_marker_path() const {
  return db_path_ + ".checkpoint";
}

bool CheckpointManager::should_checkpoint() const {
  auto elapsed = std::chrono::steady_clock::now() - last_checkpoint_time_;
  bool time_trigger = elapsed >= checkpoint_interval_;

  lsn_t current_lsn = log_manager_.get_next_lsn();
  lsn_t wal_bytes_since_checkpoint = current_lsn - last_checkpoint_lsn_;
  bool size_trigger = wal_bytes_since_checkpoint >= checkpoint_wal_size_threshold_;

  return time_trigger || size_trigger;
}

bool CheckpointManager::run_checkpoint() {
  LOG_STORAGE_INFO("Starting checkpoint");

  LogRecord begin_checkpoint{};
  if (!build_transaction_record(LogRecordType::BEGIN_CHECKPOINT, INVALID_TXN_ID, begin_checkpoint)) {
    LOG_STORAGE_ERROR("Failed to build BEGIN_CHECKPOINT record");
    return false;
  }

  lsn_t begin_checkpoint_lsn = log_manager_.append_record(begin_checkpoint);
  if (begin_checkpoint_lsn == INVALID_LSN) {
    LOG_STORAGE_ERROR("Failed to append BEGIN_CHECKPOINT record");
    return false;
  }

  auto dpt = buffer_pool_.get_dirty_page_table();
  auto att = txn_manager_.get_active_transaction_table();

  LOG_STORAGE_DEBUG("Checkpoint snapshot: {} dirty pages, {} active transactions",
                    dpt.size(), att.size());

  CheckpointPatch checkpoint_patch;
  for (const auto& [page_id, rec_lsn] : dpt) {
    checkpoint_patch.dirty_pages.emplace_back(page_id, rec_lsn);
  }
  for (const auto& [txn_id, last_lsn] : att) {
    checkpoint_patch.active_transactions.emplace_back(txn_id, last_lsn);
  }

  LogRecord end_checkpoint{};
  if (!build_transaction_record(LogRecordType::END_CHECKPOINT, INVALID_TXN_ID, end_checkpoint)) {
    LOG_STORAGE_ERROR("Failed to build END_CHECKPOINT record header");
    return false;
  }

  ByteBuffer checkpoint_payload;
  if (!serialize_checkpoint_patch(checkpoint_patch, checkpoint_payload)) {
    LOG_STORAGE_ERROR("Failed to serialize checkpoint patch");
    return false;
  }

  end_checkpoint.header.payload_size = static_cast<uint32_t>(checkpoint_payload.size());
  end_checkpoint.payload = std::move(checkpoint_payload);

  lsn_t end_checkpoint_lsn = log_manager_.append_record(end_checkpoint);
  if (end_checkpoint_lsn == INVALID_LSN) {
    LOG_STORAGE_ERROR("Failed to append END_CHECKPOINT record");
    return false;
  }

  if (!log_manager_.flush_to_lsn(end_checkpoint_lsn)) {
    LOG_STORAGE_ERROR("Failed to flush WAL to END_CHECKPOINT LSN");
    return false;
  }

  if (!write_checkpoint_marker(begin_checkpoint_lsn)) {
    LOG_STORAGE_ERROR("Failed to write checkpoint marker file");
    return false;
  }

  last_checkpoint_time_ = std::chrono::steady_clock::now();
  last_checkpoint_lsn_ = begin_checkpoint_lsn;

  LOG_STORAGE_INFO("Checkpoint completed at LSN {}", begin_checkpoint_lsn);
  return true;
}

bool CheckpointManager::write_checkpoint_marker(lsn_t checkpoint_lsn) const {
  CheckpointMarker marker{};
  std::memcpy(marker.signature, CHECKPOINT_MARKER_SIGNATURE, sizeof(marker.signature));
  marker.last_checkpoint_lsn = checkpoint_lsn;
  marker.checksum = 0;

  ByteBuffer buffer;
  if (!serialize_checkpoint_marker(marker, buffer)) {
    LOG_STORAGE_ERROR("Failed to serialize checkpoint marker");
    return false;
  }

  std::string path = checkpoint_marker_path();
  int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, S_IRUSR | S_IWUSR);
  if (fd == -1) {
    LOG_STORAGE_ERROR("Failed to open checkpoint marker file {}: {}", path, std::strerror(errno));
    return false;
  }

  ssize_t written = pwrite(fd, buffer.data(), buffer.size(), 0);
  if (written != static_cast<ssize_t>(buffer.size())) {
    LOG_STORAGE_ERROR("Failed to write checkpoint marker file {}: {}", path, std::strerror(errno));
    close(fd);
    return false;
  }

  if (fsync(fd) != 0) {
    LOG_STORAGE_ERROR("Failed to fsync checkpoint marker file {}: {}", path, std::strerror(errno));
    close(fd);
    return false;
  }

  close(fd);
  LOG_STORAGE_DEBUG("Wrote checkpoint marker to {}", path);
  return true;
}

lsn_t CheckpointManager::read_checkpoint_marker() const {
  std::string path = checkpoint_marker_path();

  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd == -1) {
    LOG_STORAGE_DEBUG("No checkpoint marker file found at {} (first checkpoint?)", path);
    return INVALID_LSN;
  }

  ByteBuffer buffer(sizeof(CheckpointMarker));
  ssize_t bytes_read = pread(fd, buffer.data(), buffer.size(), 0);
  close(fd);

  if (bytes_read != static_cast<ssize_t>(buffer.size())) {
    LOG_STORAGE_WARN("Checkpoint marker file {} is corrupt or incomplete", path);
    return INVALID_LSN;
  }

  CheckpointMarker marker{};
  if (!deserialize_checkpoint_marker(buffer, marker)) {
    LOG_STORAGE_WARN("Failed to deserialize checkpoint marker file {}", path);
    return INVALID_LSN;
  }

  LOG_STORAGE_DEBUG("Read checkpoint marker from {}: last_checkpoint_lsn = {}", path, marker.last_checkpoint_lsn);
  return marker.last_checkpoint_lsn;
}

}  // namespace letty
