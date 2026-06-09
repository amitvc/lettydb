#pragma once

#include <optional>

#include "buffer/buffer_pool_manager.h"
#include "storage_def.h"

namespace letty {

/**
 * @class TableScanner
 * @brief Streams raw tuple bytes from a table without materializing all rows.
 *
 * TableScanner walks the table's IAM chain, extents, pages, and slots. It keeps
 * at most one data page pinned at a time and unpins it before moving forward.
 */
class TableScanner {
 public:
  TableScanner(BufferPoolManager& buffer_pool, page_id_t iam_head);
  ~TableScanner();

  TableScanner(const TableScanner&) = delete;
  TableScanner& operator=(const TableScanner&) = delete;

  /**
   * @brief Advances to the next live tuple.
   * @return true if a tuple is available, false when the scan is complete.
   */
  bool next();

  /**
   * @brief Returns the current tuple bytes.
   *
   * The pointer remains valid until the next call to next() or until the
   * scanner is destroyed.
   */
  const char* get_tuple_data() const;

  /**
   * @brief Returns the size of the current tuple in bytes.
   */
  uint32_t get_tuple_size() const;

 private:
  bool load_next_iam_page();
  bool move_to_next_page();
  void release_current_page();

  BufferPoolManager& buffer_pool_;
  page_id_t next_iam_page_id_;
  std::optional<IAMPage> current_iam_page_;
  uint16_t current_extent_index_ = 0;
  int current_page_offset_ = 0;
  uint16_t current_slot_ = 0;
  page_id_t current_page_id_ = INVALID_PAGE_ID;
  Page* current_page_ = nullptr;
  const char* current_tuple_data_ = nullptr;
  uint32_t current_tuple_size_ = 0;
};

}
