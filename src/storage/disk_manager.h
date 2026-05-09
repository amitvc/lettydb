#pragma once

#include <atomic>
#include <cstdint>
#include "config.h"
#include <string>
#include "error_codes.h"

namespace letty {
/**
 * @brief
 * Capture i/o statistics
 */
struct IOStats {
  uint64_t reads  = 0;
  uint64_t writes = 0;
};

/**
 * @interface IDiskManager
 * @brief Interface for disk management operations
 *
 * This interface defines the contract for disk I/O operations at the page level.
 * Page is nothing but a continuous block of bytes on disk. It is smallest unit we load/write to the disk.
 * Having an interface for this I/O functionality allows to simulate I/O failures in unit tests
 */
class IDiskManager {
 public:
  virtual ~IDiskManager() = default;

  virtual IOResult write_page(page_id_t page_id, const char *data) = 0;
  virtual IOResult read_page(page_id_t page_id, char *buffer) = 0;
  virtual page_id_t get_file_size_in_pages() = 0;

  virtual IOStats get_io_stats() const { return {}; }

  virtual void reset_io_stats() {}

  /**
   * Flush all buffered writes to the disk.
   * @return true if the sync succeeds.
   */
  virtual bool sync() { return true; }
};

/**
 * @class DiskManager
 * @brief
 * Component responsible for persisting database pages
 * to stable storage and retrieving them on demand
 * The DiskManager operates strictly at the *page* granularity. A page is
 * the smallest unit of I/O and has a fixed size defined by PAGE_SIZE
 * (see config.h)
 *
 * Responsibilities:
 *  - Read a full page from disk given a page_id
 *  - Write a full page to disk given a page_id
 *  - Allocate new pages by extending the database file
 *  - Maintain durability by ensuring page writes reach stable storage
 *
 * Non-responsibilities:
 *  - Does NOT cache pages (handled by the Buffer Pool)
 *  - Does NOT understand page contents or page types
 *  - Does NOT manage free space within pages
 *  - Does NOT perform concurrency control or locking
 *
 * Page addressing model:
 *  - page_id is a logical identifier
 *  - Physical file offset = page_id * PAGE_SIZE
 *  - Page 0 is always the Database Header Page
 */
class DiskManager : public IDiskManager {
 public:
  explicit DiskManager(std::string db_file_name);

  /**
   * @brief Shuts down the disk manager, closing the file stream.
   */
  ~DiskManager() override;

  /**
   * @brief Writes the contents of the page to the db. The offset where the bytes are written are calculated by the
   * page_id. Formula for fp_offset = page_id * PAGE_SIZE (4096)
   * @param page_id  ID of the page that is being written.
   * @param data  Data that is to be written.
   */
  IOResult write_page(page_id_t page_id, const char *data) override;

  /**
   * @brief Reads the content of specified page in the accompanying buffer. The offset where the bytes are written are calculated by the
   * page_id. Formula for fp_offset = page_id * PAGE_SIZE (4096)
   * @param page_id ID of the page that is being written.
   * @param buffer Buffer in which data will be written.
   */
  IOResult read_page(page_id_t page_id, char *buffer) override;

  /**
   * @brief Returns the current size of the database file in pages.
   * @return Number of pages in the file (file_size / PAGE_SIZE)
   */
  page_id_t get_file_size_in_pages() override;

  IOStats get_io_stats() const override;
  void reset_io_stats() override;
  bool sync() override;

 private:
  std::string file_name_;
  int fd_ = -1;
  std::atomic<uint64_t> read_count_{0};
  std::atomic<uint64_t> write_count_{0};
  std::atomic<uint64_t> read_error_{0};
  std::atomic<uint64_t> write_error_{0};
};
}
