//
// Created by Amit Chavan on 10/2/26.
//

#include "log_manager.h"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <unistd.h>
#include <utility>

#include "common/db_exception.h"
#include "common/logger.h"
#include "log_record.h"

namespace letty {

LogManager::LogManager(std::string file_prefix, uint32_t segment_size, uint32_t flush_interval_ms)
    : file_prefix_(std::move(file_prefix)),
      segment_size_(segment_size),
      flush_interval_(flush_interval_ms),
      last_flush_time_(std::chrono::steady_clock::now()) {
  assert(!file_prefix_.empty() && "WAL file prefix cannot be empty");
  assert(segment_size_ > WAL_SIGNATURE_SIZE && "Segment size must be larger than WAL header size");

  std::lock_guard<std::mutex> lock(latch_);

  auto latest = find_latest_segment();
  if (!latest.has_value()) {
    fd_ = create_segment_file(0);
    current_segment_id_ = 0;
    next_lsn_ = WAL_SIGNATURE_SIZE;
    flushed_lsn_ = WAL_SIGNATURE_SIZE;
  } else {
    current_segment_id_ = latest->first;
    fd_ = open_segment_file(current_segment_id_);
    next_lsn_ = build_lsn(current_segment_id_, latest->second);
    flushed_lsn_ = next_lsn_;
  }
}

LogManager::~LogManager() {
  {
    std::lock_guard<std::mutex> lock(latch_);
    if (fd_ != -1) {
      flush_buffer_to_disk();
    }
  }
  if (fd_ != -1) {
    close(fd_);
  }
}

std::vector<uint32_t> LogManager::list_segment_ids() const {
  namespace fs = std::filesystem;
  std::vector<uint32_t> segment_ids;
  fs::path prefix_path(file_prefix_);
  fs::path dir = prefix_path.parent_path();
  if (dir.empty()) {
    dir = ".";
  }
  if (!fs::exists(dir)) {
    return segment_ids;
  }

  std::string base_prefix = prefix_path.filename().string() + ".";
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    std::string filename = entry.path().filename().string();
    if (filename.rfind(base_prefix, 0) != 0 || filename.size() <= base_prefix.size()) {
      continue;
    }

    const char* suffix_begin = filename.data() + base_prefix.size();
    const char* suffix_end = filename.data() + filename.size();
    uint32_t seg_id = 0;
    auto [ptr, ec] = std::from_chars(suffix_begin, suffix_end, seg_id);
    if (ec == std::errc{} && ptr == suffix_end) {
      segment_ids.push_back(seg_id);
    }
  }

  std::sort(segment_ids.begin(), segment_ids.end());
  return segment_ids;
}

std::optional<std::pair<uint32_t, size_t>> LogManager::find_latest_segment() const {
  std::vector<uint32_t> segment_ids = list_segment_ids();
  if (segment_ids.empty()) {
    return std::nullopt;
  }
  uint32_t max_seg_id = segment_ids.back();
  size_t max_seg_size = static_cast<size_t>(std::filesystem::file_size(segment_file_path(max_seg_id)));
  return std::make_pair(max_seg_id, max_seg_size);
}

int LogManager::create_segment_file(uint32_t segment_id) {
  std::string path = segment_file_path(segment_id);
  mode_t mode = S_IRUSR | S_IWUSR;
  int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (fd == -1) {
    throw DbException(DbErrorCode::IOError,
                      "FATAL: Failed to create WAL segment file: " + path +
                          " (" + std::strerror(errno) + ")");
  }

  ssize_t written = pwrite(fd, WAL_SIGNATURE, WAL_SIGNATURE_SIZE, 0);
  if (written != static_cast<ssize_t>(WAL_SIGNATURE_SIZE) || fsync(fd) != 0) {
    int err = errno;
    close(fd);
    throw DbException(DbErrorCode::IOError,
                      "FATAL: Failed to initialize WAL header in segment file: " + path +
                          " (" + std::strerror(err) + ")");
  }

  LOG_STORAGE_INFO("Created WAL segment file {}", path);
  return fd;
}

int LogManager::open_segment_file(uint32_t segment_id) {
  std::string path = segment_file_path(segment_id);
  int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd == -1) {
    throw DbException(DbErrorCode::IOError,
                      "FATAL: Failed to open WAL segment file: " + path +
                          " (" + std::strerror(errno) + ")");
  }

  char header[WAL_SIGNATURE_SIZE]{};
  ssize_t bytes_read = pread(fd, header, WAL_SIGNATURE_SIZE, 0);
  if (bytes_read != static_cast<ssize_t>(WAL_SIGNATURE_SIZE) ||
      std::memcmp(header, WAL_SIGNATURE, WAL_SIGNATURE_SIZE) != 0) {
    close(fd);
    throw DbException(DbErrorCode::Corruption,
                      "FATAL: Invalid or corrupt WAL header in segment file: " + path);
  }

  LOG_STORAGE_INFO("Opened existing WAL segment file {}", path);
  return fd;
}

bool LogManager::flush_buffer_to_disk() {
  if (log_buffer_.empty()) {
    return true;
  }
  if (fd_ == -1) {
    LOG_WAL_ERROR("Cannot flush WAL buffer: file descriptor is closed");
    return false;
  }

  uint32_t file_offset = calculate_file_offset_from_lsn(flushed_lsn_);
  ssize_t written = pwrite(fd_, log_buffer_.data(), log_buffer_.size(), static_cast<off_t>(file_offset));
  if (written != static_cast<ssize_t>(log_buffer_.size())) {
    LOG_WAL_ERROR("Failed to write WAL buffer at LSN {}: {}", flushed_lsn_, std::strerror(errno));
    return false;
  }

  if (fsync(fd_) != 0) {
    LOG_WAL_ERROR("Failed to fsync WAL segment {}: {}", current_segment_id_, std::strerror(errno));
    return false;
  }

  log_buffer_.clear();
  flushed_lsn_ = next_lsn_;
  return true;
}

bool LogManager::rollover_to_next_segment() {
  if (!flush_buffer_to_disk()) {
    return false;
  }
  if (fd_ != -1) {
    close(fd_);
    fd_ = -1;
  }

  ++current_segment_id_;
  fd_ = create_segment_file(current_segment_id_);
  next_lsn_ = build_lsn(current_segment_id_, WAL_SIGNATURE_SIZE);
  flushed_lsn_ = next_lsn_;
  return true;
}

lsn_t LogManager::append_record(LogRecord& record) {
  std::lock_guard<std::mutex> lock(latch_);

  size_t record_size = sizeof(LogRecordHeader) + record.payload.size();
  if (WAL_SIGNATURE_SIZE + record_size > segment_size_) {
    LOG_WAL_ERROR("Log record size {} exceeds max segment capacity {}",
                  record_size, segment_size_ - WAL_SIGNATURE_SIZE);
    return INVALID_LSN;
  }

  if (calculate_file_offset_from_lsn(next_lsn_) + record_size > segment_size_) {
    if (!rollover_to_next_segment()) {
      return INVALID_LSN;
    }
  }

  // This is the LSN we assigned to the WAL record. We need to return it to the caller.
  lsn_t assigned_lsn = next_lsn_;

  record.header.lsn = assigned_lsn;
  ByteBuffer bytes;
  if (!serialize_log_record(record, bytes)) {
    return INVALID_LSN;
  }

  // Start the flush timer for this batch.
  if (log_buffer_.empty()) {
    last_flush_time_ = std::chrono::steady_clock::now();
  }
  log_buffer_.insert(log_buffer_.end(), bytes.begin(), bytes.end());

  // Move the cursor to current wal log record + next_lsn_. That new position is the next lsn.
  next_lsn_ += bytes.size();

  auto elapsed = std::chrono::steady_clock::now() - last_flush_time_;
  if (elapsed >= flush_interval_) {
    if (!flush_buffer_to_disk()) {
      return INVALID_LSN;
    }
  }

  return assigned_lsn;
}

bool LogManager::flush_to_lsn(lsn_t target_lsn) {
  std::lock_guard<std::mutex> lock(latch_);

  if (target_lsn == INVALID_LSN || target_lsn <= flushed_lsn_) {
	LOG_WAL_INFO("Last flushed lsn {} is greater than target lsn {}", flushed_lsn_, target_lsn);
    return true;
  }
  return flush_buffer_to_disk();
}

std::optional<LogRecord> LogManager::read_record_from_buffer(lsn_t expected_lsn) const {
  if (expected_lsn < flushed_lsn_ || expected_lsn >= next_lsn_) {
    return std::nullopt;
  }

  size_t buf_offset = static_cast<size_t>(expected_lsn - flushed_lsn_);
  if (buf_offset + sizeof(LogRecordHeader) > log_buffer_.size()) {
    return std::nullopt;
  }

  LogRecordHeader hdr{};
  std::memcpy(&hdr, log_buffer_.data() + buf_offset, sizeof(LogRecordHeader));
  size_t total_size = sizeof(LogRecordHeader) + hdr.payload_size;
  if (buf_offset + total_size > log_buffer_.size()) {
    return std::nullopt;
  }

  ByteBuffer slice(log_buffer_.begin() + static_cast<std::ptrdiff_t>(buf_offset),
                   log_buffer_.begin() + static_cast<std::ptrdiff_t>(buf_offset + total_size));
  LogRecord record{};
  if (!deserialize_log_record(slice, record) || record.header.lsn != expected_lsn) {
    return std::nullopt;
  }
  return record;
}

std::optional<LogRecord> LogManager::read_record_from_fd(int fd, lsn_t expected_lsn, uint32_t offset) const {
  if (offset < WAL_SIGNATURE_SIZE || offset + sizeof(LogRecordHeader) > segment_size_) {
    return std::nullopt;
  }

  LogRecordHeader hdr{};
  ssize_t hdr_read = pread(fd, &hdr, sizeof(LogRecordHeader), static_cast<off_t>(offset));
  if (hdr_read != static_cast<ssize_t>(sizeof(LogRecordHeader))) {
    return std::nullopt;
  }

  size_t total_size = sizeof(LogRecordHeader) + hdr.payload_size;
  if (offset + total_size > segment_size_) {
    return std::nullopt;
  }

  ByteBuffer buffer(total_size);
  std::memcpy(buffer.data(), &hdr, sizeof(LogRecordHeader));
  if (hdr.payload_size > 0) {
    off_t payload_offset = static_cast<off_t>(offset + sizeof(LogRecordHeader));
    ssize_t payload_read = pread(fd, buffer.data() + sizeof(LogRecordHeader), hdr.payload_size, payload_offset);
    if (payload_read != static_cast<ssize_t>(hdr.payload_size)) {
      return std::nullopt;
    }
  }

  LogRecord record{};
  if (!deserialize_log_record(buffer, record) || record.header.lsn != expected_lsn) {
    return std::nullopt;
  }
  return record;
}

std::optional<LogRecord> LogManager::read_record_at(lsn_t lsn) {
  std::lock_guard<std::mutex> lock(latch_);

  if (lsn < WAL_SIGNATURE_SIZE || lsn >= next_lsn_) {
    return std::nullopt;
  }
  if (lsn >= flushed_lsn_) {
    return read_record_from_buffer(lsn);
  }

  uint32_t seg_id = segment_id_from_lsn(lsn);
  uint32_t file_offset = calculate_file_offset_from_lsn(lsn);

  if (seg_id == current_segment_id_) {
    return read_record_from_fd(fd_, lsn, file_offset);
  }

  std::string path = segment_file_path(seg_id);
  if (!std::filesystem::exists(path)) {
    return std::nullopt;
  }
  int temp_fd = open_segment_file(seg_id);
  auto record = read_record_from_fd(temp_fd, lsn, file_offset);
  close(temp_fd);
  return record;
}

bool LogManager::truncate() {
  std::lock_guard<std::mutex> lock(latch_);

  log_buffer_.clear();
  if (fd_ != -1) {
    close(fd_);
    fd_ = -1;
  }

  for (uint32_t seg_id : list_segment_ids()) {
    std::filesystem::remove(segment_file_path(seg_id));
  }

  fd_ = create_segment_file(0);
  current_segment_id_ = 0;
  next_lsn_ = WAL_SIGNATURE_SIZE;
  flushed_lsn_ = WAL_SIGNATURE_SIZE;
  return true;
}

std::vector<LogRecord> LogManager::read_records_from(lsn_t start_lsn) {
  std::lock_guard<std::mutex> lock(latch_);

  std::vector<LogRecord> records;
  if (start_lsn >= next_lsn_) {
    return records;
  }

  uint32_t start_seg_id = segment_id_from_lsn(start_lsn);
  uint32_t start_offset = calculate_file_offset_from_lsn(start_lsn);
  if (start_offset < WAL_SIGNATURE_SIZE) {
    start_offset = static_cast<uint32_t>(WAL_SIGNATURE_SIZE);
  }

  for (uint32_t seg_id : list_segment_ids()) {
    if (seg_id < start_seg_id) {
      continue;
    }

    int seg_fd = (seg_id == current_segment_id_) ? fd_ : open_segment_file(seg_id);
    uint32_t offset = (seg_id == start_seg_id)
                          ? start_offset
                          : static_cast<uint32_t>(WAL_SIGNATURE_SIZE);

    while (offset + sizeof(LogRecordHeader) <= segment_size_) {
      lsn_t expected_lsn = build_lsn(seg_id, offset);
      if (expected_lsn >= next_lsn_) {
        break;
      }
      auto record = (expected_lsn >= flushed_lsn_)
                        ? read_record_from_buffer(expected_lsn)
                        : read_record_from_fd(seg_fd, expected_lsn, offset);
      if (!record.has_value()) {
        break;
      }
      offset += static_cast<uint32_t>(sizeof(LogRecordHeader) + record->header.payload_size);
      records.push_back(std::move(*record));
    }

    if (seg_fd != fd_) {
      close(seg_fd);
    }
  }
  return records;
}

std::vector<LogRecord> LogManager::read_records() {
  return read_records_from(WAL_SIGNATURE_SIZE);
}

}