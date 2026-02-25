#include "storage/slotted_page.h"
#include <cstring>
#include <cassert>

namespace letty {

SlottedPage::SlottedPage(char* buffer) : data_(buffer) {
  header_ = reinterpret_cast<SlottedPageHeader*>(buffer);
  slots_ = reinterpret_cast<Slot*>(buffer + sizeof(SlottedPageHeader));
}

SlottedPage SlottedPage::init(char* buffer) {
  std::memset(buffer, 0, PAGE_SIZE);
  auto* header = reinterpret_cast<SlottedPageHeader*>(buffer);
  header->free_space_pointer = PAGE_SIZE;
  header->num_slots = 0;
  header->next_page_id = INVALID_PAGE_ID;
  header->prev_page_id = INVALID_PAGE_ID;
  return SlottedPage(buffer);
}

size_t SlottedPage::get_free_space() const {
  size_t headers_size = sizeof(SlottedPageHeader) + (header_->num_slots * sizeof(Slot));
  if (headers_size > header_->free_space_pointer) {
    return 0;
  }
  return header_->free_space_pointer - headers_size;
}

uint16_t SlottedPage::get_num_slots() const {
  return header_->num_slots;
}

std::optional<uint16_t> SlottedPage::insert_tuple(const char* tuple_data, uint32_t tuple_size) {
  // Search for a reusable deleted slot
  int target_slot_id = -1;
  for (int i = 0; i < header_->num_slots; ++i) {
    if (slots_[i].length == 0) {
      target_slot_id = i;
      break;
    }
  }

  size_t needed_space = tuple_size;
  if (target_slot_id == -1) {
    needed_space += sizeof(Slot);
  }

  if (get_free_space() < needed_space) {
    return std::nullopt;
  }

  // Write tuple data at the end of the free region
  header_->free_space_pointer -= tuple_size;
  std::memcpy(data_ + header_->free_space_pointer, tuple_data, tuple_size);

  if (target_slot_id != -1) {
    slots_[target_slot_id].offset = header_->free_space_pointer;
    slots_[target_slot_id].length = tuple_size;
    return static_cast<uint16_t>(target_slot_id);
  }

  // Allocate a new slot entry
  slots_[header_->num_slots].offset = header_->free_space_pointer;
  slots_[header_->num_slots].length = tuple_size;
  return header_->num_slots++;
}

const char* SlottedPage::get_tuple(uint16_t slot_id, uint32_t* size) const {
  if (slot_id >= header_->num_slots) {
    return nullptr;
  }

  const Slot& slot = slots_[slot_id];
  if (slot.length == 0) {
    return nullptr;
  }

  assert(slot.offset + slot.length <= PAGE_SIZE);

  if (size) *size = slot.length;
  return data_ + slot.offset;
}

} // namespace letty
