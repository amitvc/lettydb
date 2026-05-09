#include "storage/slotted_page.h"
#include <cstring>
#include <cassert>

namespace letty {

size_t SlottedPage::slot_offset(uint16_t slot_id) {
  return sizeof(SlottedPageHeader) + (slot_id * sizeof(Slot));
}

SlottedPage::SlottedPage(char* buffer) : data_(buffer) {

}

SlottedPage SlottedPage::init(char* buffer) {
  std::memset(buffer, 0, PAGE_SIZE);
  SlottedPage page(buffer);

  SlottedPageHeader header{};
  header.free_space_pointer = PAGE_SIZE;
  header.num_slots = 0;
  header.next_page_id = INVALID_PAGE_ID;
  header.prev_page_id = INVALID_PAGE_ID;
  page.store_header(header);
  return page;
}

size_t SlottedPage::get_free_space() const {
  SlottedPageHeader header = load_header();
  size_t headers_size = sizeof(SlottedPageHeader) + (header.num_slots * sizeof(Slot));
  if (headers_size > header.free_space_pointer) {
    return 0;
  }
  return header.free_space_pointer - headers_size;
}

uint16_t SlottedPage::get_num_slots() const {
  return load_header().num_slots;
}

std::optional<uint16_t> SlottedPage::insert_tuple(const char* tuple_data, uint32_t tuple_size) {
  SlottedPageHeader header = load_header();

  // Prefer reusing a deleted slot so the slot directory does not grow if it
  // already has a tombstone entry.
  int target_slot_id = -1;
  for (int i = 0; i < header.num_slots; ++i) {
    if (load_slot(static_cast<uint16_t>(i)).length == 0) {
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

  // Tuple bytes grow backward from the end of the page into the free region.
  header.free_space_pointer -= tuple_size;
  std::memcpy(data_ + header.free_space_pointer, tuple_data, tuple_size);

  if (target_slot_id != -1) {
    Slot slot{};
    slot.offset = header.free_space_pointer;
    slot.length = static_cast<uint16_t>(tuple_size);
    store_slot(static_cast<uint16_t>(target_slot_id), slot);
    store_header(header);
    return static_cast<uint16_t>(target_slot_id);
  }

  // Allocate a new slot entry
  uint16_t new_slot_id = header.num_slots;
  Slot slot{};
  slot.offset = header.free_space_pointer;
  slot.length = static_cast<uint16_t>(tuple_size);
  store_slot(new_slot_id, slot);
  header.num_slots++;
  store_header(header);
  return new_slot_id;
}

const char* SlottedPage::get_tuple(uint16_t slot_id, uint32_t* size) const {
  SlottedPageHeader header = load_header();
  if (slot_id >= header.num_slots) {
    return nullptr;
  }

  Slot slot = load_slot(slot_id);
  if (slot.length == 0) {
    return nullptr;
  }

  assert(slot.offset + slot.length <= PAGE_SIZE);

  if (size) *size = slot.length;
  return data_ + slot.offset;
}

SlottedPageHeader SlottedPage::load_header() const {
  SlottedPageHeader header{};
  std::memcpy(&header, data_, sizeof(SlottedPageHeader));
  return header;
}

void SlottedPage::store_header(const SlottedPageHeader& header) {
  std::memcpy(data_, &header, sizeof(SlottedPageHeader));
}

Slot SlottedPage::load_slot(uint16_t slot_id) const {
  assert(slot_offset(slot_id) + sizeof(Slot) <= PAGE_SIZE);
  Slot slot{};
  std::memcpy(&slot, data_ + slot_offset(slot_id), sizeof(Slot));
  return slot;
}

void SlottedPage::store_slot(uint16_t slot_id, const Slot& slot) {
  assert(slot_offset(slot_id) + sizeof(Slot) <= PAGE_SIZE);
  std::memcpy(data_ + slot_offset(slot_id), &slot, sizeof(Slot));
}

} // namespace letty
