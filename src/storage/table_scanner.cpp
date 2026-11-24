#include "table_scanner.h"

#include <string>

#include "common/db_exception.h"
#include "page_utils.h"
#include "slotted_page.h"

namespace letty {

TableScanner::TableScanner(BufferPoolManager& buffer_pool, page_id_t first_page_id)
    : buffer_pool_(buffer_pool), next_page_id_(first_page_id){}

TableScanner::~TableScanner() = default;

bool TableScanner::next() {
  current_tuple_data_ = nullptr;
  current_tuple_size_ = 0;

  while (true) {

    if (!current_page_guard_ && !move_to_next_page()) {
      return false;
    }

    SlottedPage slotted_page = SlottedPage::from_page(current_page_guard_);
    while (current_slot_ < slotted_page.get_num_slots()) {
      const Byte* data = slotted_page.get_tuple(current_slot_, &current_tuple_size_);
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

const Byte* TableScanner::get_tuple_data() const {
  return current_tuple_data_;
}

uint32_t TableScanner::get_tuple_size() const {
  return current_tuple_size_;
}


bool TableScanner::move_to_next_page() {
  if (next_page_id_ == INVALID_PAGE_ID) {
    return false;
  }

  current_page_id_ = next_page_id_;
  current_page_guard_ = buffer_pool_.fetch_page_guard(current_page_id_);
  if (!current_page_guard_) {
    throw DbException(DbErrorCode::IOError, "failed to fetch table page " + std::to_string(current_page_id_));
  }

  PageHeader header = load_page_layout<PageHeader>(current_page_guard_);
  next_page_id_ = header.next_page_id;
  current_slot_ = 0;
  return true;
}

void TableScanner::release_current_page() {
  current_page_guard_.drop();
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
