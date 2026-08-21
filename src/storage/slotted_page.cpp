#include "storage/slotted_page.h"
#include <cassert>
#include <cstring>
#include <vector>

namespace letty {

static_assert(sizeof(SlottedPageHeader) == 16,
              "SlottedPageHeader must be exactly 16 bytes on disk");
static_assert(sizeof(Slot) == 4, "Slot must be exactly 4 bytes on disk");

SlottedPage::SlottedPage(char* buffer)
    : data_(buffer),
      header_(reinterpret_cast<SlottedPageHeader*>(buffer)) {
}

SlottedPage SlottedPage::init(char* buffer) {
  std::memset(buffer, 0, PAGE_SIZE);
  SlottedPage page(buffer);

  page.header_->free_space_pointer = PAGE_SIZE;
  page.header_->num_slots = 0;
  page.header_->next_page_id = INVALID_PAGE_ID;
  page.header_->prev_page_id = INVALID_PAGE_ID;
  return page;
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

bool SlottedPage::compact() {
  auto* slot_dir = reinterpret_cast<Slot*>(data_ + sizeof(SlottedPageHeader));

  bool has_tombstones = false;
  for (uint16_t i = 0; i < header_->num_slots; ++i) {
    if (slot_dir[i].length == 0) {
      has_tombstones = true;
      break;
    }
  }
  if (!has_tombstones) {
    return false;
  }

  std::vector<std::vector<char>> live_tuples;
  for (uint16_t i = 0; i < header_->num_slots; ++i) {
    if (slot_dir[i].length > 0) {
      std::vector<char> buf(slot_dir[i].length);
      std::memcpy(buf.data(), data_ + slot_dir[i].offset, slot_dir[i].length);
      live_tuples.push_back(std::move(buf));
    }
  }

  SlottedPage::init(data_);
  SlottedPage fresh(data_);

  for (const auto& t : live_tuples) {
    fresh.insert_tuple(t.data(), static_cast<uint32_t>(t.size()));
  }
  return true;
}

std::optional<uint16_t> SlottedPage::insert_tuple(const char* tuple_data, uint32_t tuple_size) {
  auto* slot_dir = reinterpret_cast<Slot*>(data_ + sizeof(SlottedPageHeader));

  // Prefer reusing a deleted slot so the slot directory does not grow if it
  // already has a tombstone entry.
  int target_slot_id = -1;
  for (int i = 0; i < header_->num_slots; ++i) {
    if (slot_dir[i].length == 0) {
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
  header_->free_space_pointer -= tuple_size;
  std::memcpy(data_ + header_->free_space_pointer, tuple_data, tuple_size);

  if (target_slot_id != -1) {
    slot_dir[target_slot_id].offset = header_->free_space_pointer;
    slot_dir[target_slot_id].length = static_cast<uint16_t>(tuple_size);
    return static_cast<uint16_t>(target_slot_id);
  }

  uint16_t new_slot_id = header_->num_slots;
  slot_dir[new_slot_id].offset = header_->free_space_pointer;
  slot_dir[new_slot_id].length = static_cast<uint16_t>(tuple_size);
  header_->num_slots++;
  return new_slot_id;
}

const char* SlottedPage::get_tuple(uint16_t slot_id, uint32_t* size) const {
  if (slot_id >= header_->num_slots) {
    return nullptr;
  }

  const auto* slot_dir = reinterpret_cast<const Slot*>(data_ + sizeof(SlottedPageHeader));
  const Slot& slot = slot_dir[slot_id];
  if (slot.length == 0) {
    return nullptr;
  }

  assert(slot.offset + slot.length <= PAGE_SIZE);

  if (size) *size = slot.length;
  return data_ + slot.offset;
}

bool SlottedPage::delete_tuple(uint16_t slot_id) {
  if (slot_id >= header_->num_slots) {
    return false;
  }

  auto* slot_dir = reinterpret_cast<Slot*>(data_ + sizeof(SlottedPageHeader));
  if (slot_dir[slot_id].length == 0) {
    return false;
  }

  slot_dir[slot_id].length = 0;

  // Shrink num_slots past any trailing tombstones.
  while (header_->num_slots > 0) {
    if (slot_dir[header_->num_slots - 1].length > 0) {
      break;
    }
    header_->num_slots--;
  }

  return true;
}

SlottedPageHeader SlottedPage::load_header() const {
  SlottedPageHeader header{};
  std::memcpy(&header, header_, sizeof(SlottedPageHeader));
  return header;
}

void SlottedPage::store_header(const SlottedPageHeader& header) {
  std::memcpy(header_, &header, sizeof(SlottedPageHeader));
}

Slot SlottedPage::load_slot(uint16_t slot_id) const {
  assert(slot_offset(slot_id) + sizeof(Slot) <= PAGE_SIZE);
  const auto* slot_dir = reinterpret_cast<const Slot*>(data_ + sizeof(SlottedPageHeader));
  Slot slot{};
  std::memcpy(&slot, slot_dir + slot_id, sizeof(Slot));
  return slot;
}

void SlottedPage::store_slot(uint16_t slot_id, const Slot& slot) {
  assert(slot_offset(slot_id) + sizeof(Slot) <= PAGE_SIZE);
  auto* slot_dir = reinterpret_cast<Slot*>(data_ + sizeof(SlottedPageHeader));
  std::memcpy(slot_dir + slot_id, &slot, sizeof(Slot));
}

size_t SlottedPage::slot_offset(uint16_t slot_id) {
  return sizeof(SlottedPageHeader) + (slot_id * sizeof(Slot));
}

}
