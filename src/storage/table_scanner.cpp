#include "table_scanner.h"

#include <string>

#include "common/db_exception.h"
#include "page_utils.h"
#include "slotted_page.h"

namespace letty {

TableScanner::TableScanner(BufferPoolManager& buffer_pool, page_id_t iam_head)
    : buffer_pool_(buffer_pool), next_iam_page_id_(iam_head) {}

TableScanner::~TableScanner() {
  release_current_page();
}

bool TableScanner::next() {
  current_tuple_data_ = nullptr;
  current_tuple_size_ = 0;

  while (true) {
    if (!current_iam_page_ && !load_next_iam_page()) {
      return false;
    }

    if (!current_page_ && !move_to_next_page()) {
      current_iam_page_.reset();
      continue;
    }

    SlottedPage slotted_page(current_page_->get_data());
    while (current_slot_ < slotted_page.get_num_slots()) {
      const char* data = slotted_page.get_tuple(current_slot_, &current_tuple_size_);
      last_found_slot_ = current_slot_;
      ++current_slot_;
      if (data) {
        current_tuple_data_ = data;
        return true;
      }
    }

    release_current_page();
  }
}

const char* TableScanner::get_tuple_data() const {
  return current_tuple_data_;
}

uint32_t TableScanner::get_tuple_size() const {
  return current_tuple_size_;
}

bool TableScanner::load_next_iam_page() {
  if (next_iam_page_id_ == INVALID_PAGE_ID) {
    return false;
  }

  current_iam_page_ = load_page_value<IAMPage>(buffer_pool_, next_iam_page_id_);
  if (!current_iam_page_) {
    throw DbException(DbErrorCode::IOError, "failed to fetch IAM page " + std::to_string(next_iam_page_id_));
  }

  next_iam_page_id_ = current_iam_page_->next_page_id;
  current_extent_index_ = 0;
  current_page_offset_ = 0;
  return true;
}

bool TableScanner::move_to_next_page() {
  while (current_iam_page_) {
    while (current_extent_index_ < current_iam_page_->extent_count) {
      while (current_page_offset_ < EXTENT_SIZE) {
        uint32_t extent_id = current_iam_page_->extent_ids[current_extent_index_];
        current_page_id_ = static_cast<page_id_t>(extent_id * EXTENT_SIZE + current_page_offset_);
        current_page_ = buffer_pool_.fetch_page(current_page_id_);
        ++current_page_offset_;

        if (current_page_) {
          current_slot_ = 0;
          return true;
        }
      }

      ++current_extent_index_;
      current_page_offset_ = 0;
    }

    return false;
  }

  return false;
}

void TableScanner::release_current_page() {
  if (!current_page_) {
    return;
  }

  buffer_pool_.unpin_page(current_page_id_, false);
  current_page_ = nullptr;
  current_page_id_ = INVALID_PAGE_ID;
  current_slot_ = 0;
}

page_id_t TableScanner::current_page_id() const {
  return current_page_id_;
}

uint16_t TableScanner::current_slot_id() const {
  return last_found_slot_;
}

}
