//
// Created by Amit Chavan on 8/30/25.
//

#include "disk_manager.h"
#include "common/logger.h"
#include "storage/config.h"
#include <cassert>
#include <utility>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace letty {
DiskManager::DiskManager(std::string db_file) : file_name_(std::move(db_file)) {
  assert(!file_name_.empty() && "Database file path cannot be empty");
  mode_t mode = S_IRUSR | S_IWUSR; // 0600
  fd_ = open(file_name_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, mode);
  if (fd_ == -1) {
	throw std::runtime_error("FATAL: Failed to create or open database file: " + file_name_ + " error no : ( " + std::strerror(errno) + ")");
  }

  LOG_STORAGE_INFO("Opening database file {}", file_name_);
}

DiskManager::~DiskManager() {
  if (fd_ != -1) {
	fsync(fd_);
	close(fd_);
  }
}

IOResult DiskManager::write_page(page_id_t page_id, const char *page_data) {
  off_t offset = static_cast<off_t>(page_id) * PAGE_SIZE;
  ssize_t written = pwrite(fd_, page_data, PAGE_SIZE, offset);
  if (written != PAGE_SIZE) {
	return IOResult::WRITE_ERROR;
  }

  ++write_count_;
  return IOResult::SUCCESS;
}

IOResult DiskManager::read_page(page_id_t page_id, char *page_data) {
  off_t offset = static_cast<off_t>(page_id) * PAGE_SIZE;
  ssize_t bytes_read = pread(fd_, page_data, PAGE_SIZE, offset);
  if (bytes_read != PAGE_SIZE) {
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

void DiskManager::sync() {
  if (fd_ != -1) {
    fsync(fd_);
  }
}

page_id_t DiskManager::get_file_size_in_pages() {
  struct stat st{};
  if (fstat(fd_, &st) != 0) {
    return 0;
  }
  return static_cast<page_id_t>(st.st_size / PAGE_SIZE);
}
}
