#include "disk_manager.h"
#include "common/logger.h"
#include "storage/config.h"
#include "common/db_exception.h"
#include <cassert>
#include <cstring>
#include <errno.h>
#include <utility>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace letty {
DiskManager::DiskManager(std::string db_file) : file_name_(std::move(db_file)) {
  assert(!file_name_.empty() && "Database file path cannot be empty");
  // Open with read/write permissions for the owner only (0600).
  // This prevents other users on the system from reading or
  mode_t mode = S_IRUSR | S_IWUSR;
  fd_ = open(file_name_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, mode);
  if (fd_ == -1) {
	throw DbException(DbErrorCode::IOError, "FATAL: Failed to create or open database file: " + file_name_ + " error no : ( " + std::strerror(errno) + ")");
  }
  LOG_STORAGE_INFO("Opening database file {}", file_name_);
}

DiskManager::~DiskManager() {
  if (fd_ != -1) {
	sync();
	close(fd_);
  }
}

IOResult DiskManager::write_page(page_id_t page_id, const char *page_data) {
  off_t offset = static_cast<off_t>(page_id) * PAGE_SIZE;
  ssize_t written = pwrite(fd_, page_data, PAGE_SIZE, offset);
  if (written != PAGE_SIZE) {
	LOG_STORAGE_ERROR("Failed to write page {} to disk: {} (wrote {} of {} bytes)",
                      page_id, std::strerror(errno), written, PAGE_SIZE);
	++write_error_;
	return IOResult::WRITE_ERROR;
  }

  ++write_count_;
  return IOResult::SUCCESS;
}

IOResult DiskManager::read_page(page_id_t page_id, char *page_data) {
  off_t offset = static_cast<off_t>(page_id) * PAGE_SIZE;
  ssize_t bytes_read = pread(fd_, page_data, PAGE_SIZE, offset);
  if (bytes_read != PAGE_SIZE) {
	LOG_STORAGE_ERROR("Failed to read page {} from disk: {} (read {} of {} bytes)",
                      page_id, std::strerror(errno), bytes_read, PAGE_SIZE);
	++read_error_;
	return IOResult::READ_ERROR;
  }

  ++read_count_;
  return IOResult::SUCCESS;
}

IOStats DiskManager::get_io_stats() const {
  return {read_count_.load(), write_count_.load()};
}

void DiskManager::reset_io_stats() {
  read_count_ = 0;
  write_count_ = 0;
}

bool DiskManager::sync() {
  if (fd_ == -1) {
    LOG_STORAGE_ERROR("Cannot sync database file {}: file descriptor is closed", file_name_);
    return false;
  }

  if (fsync(fd_) == 0) {
    return true;
  }

  int err = errno;
  LOG_STORAGE_ERROR("Problem while syncing database file {}: {}", file_name_, std::strerror(err));
  return false;
}

page_id_t DiskManager::get_file_size_in_pages() {
  struct stat st{};
  if (fstat(fd_, &st) != 0) {
    return 0;
  }
  return static_cast<page_id_t>(st.st_size / PAGE_SIZE);
}
}
