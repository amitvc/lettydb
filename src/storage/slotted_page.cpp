//
// Created by Amit Chavan on 10/2/26.
//

#include "storage/slotted_page.h"
#include "storage/page.h"
#include "page_utils.h"
#include <cassert>
#include <cstring>
#include <vector>

namespace letty {

static_assert(sizeof(SlottedPageHeader) == 20,
              "SlottedPageHeader must be exactly 20 bytes on disk");
static_assert(sizeof(Slot) == 4, "Slot must be exactly 4 bytes on disk");

SlottedPage SlottedPage::from_page(Page* page) {
  return SlottedPage(reinterpret_cast<Byte*>(page->get_data()));
}

SlottedPage::SlottedPage(Byte* buffer)
    : data_(buffer),
      header_(load_header_from_bytes(buffer)) {
}

SlottedPage SlottedPage::init(Byte* buffer) {
  std::memset(buffer, 0, PAGE_SIZE);

  SlottedPageHeader header{};
  header.free_space_pointer = PAGE_SIZE;
  header.num_slots = 0;
  header.next_page_id = INVALID_PAGE_ID;
  header.prev_page_id = INVALID_PAGE_ID;

  store_header_to_bytes(buffer, header);
  return SlottedPage(buffer);
}

void SlottedPage::store_header() {
  store_header_to_bytes(data_, header_);
}

size_t SlottedPage::slot_data_offset(uint16_t slot_id) {
  return sizeof(SlottedPageHeader) + (slot_id * sizeof(Slot));
}

size_t SlottedPage::get_free_space() const {
  size_t headers_size = sizeof(SlottedPageHeader) + (header_.num_slots * sizeof(Slot));
  if (headers_size > header_.free_space_pointer) {
    return 0;
  }
  return header_.free_space_pointer - headers_size;
}

uint16_t SlottedPage::get_num_slots() const {
  return header_.num_slots;
}

std::optional<Slot> SlottedPage::get_slot(uint16_t slot_id) const {
  if (slot_id >= header_.num_slots) {
    return std::nullopt;
  }
  const size_t off = slot_data_offset(slot_id);
  return load_slot_from_bytes(data_ + off);
}

bool SlottedPage::set_slot(uint16_t slot_id, const Slot& slot) {
  if (slot_id >= header_.num_slots) {
    return false;
  }
  const size_t off = slot_data_offset(slot_id);
  store_slot_to_bytes(data_ + off, slot);
  return true;
}

bool SlottedPage::is_empty() const {
  for (uint16_t i = 0; i < header_.num_slots; ++i) {
    if (auto slot = get_slot(i); slot && slot->length > 0) return false;
  }
  return true;
}

bool SlottedPage::compact() {
  bool has_tombstones = false;
  for (uint16_t i = 0; i < header_.num_slots; ++i) {
    if (auto slot = get_slot(i); slot && slot->length == 0) {
      has_tombstones = true;
      break;
    }
  }
  if (!has_tombstones) {
    return false;
  }

  std::vector<std::vector<Byte>> live_tuples;
  for (uint16_t i = 0; i < header_.num_slots; ++i) {
    if (auto slot = get_slot(i); slot && slot->length > 0) {
      std::vector<Byte> buf(slot->length);
      std::memcpy(buf.data(), data_ + slot->offset, slot->length);
      live_tuples.push_back(std::move(buf));
    }
  }

  SlottedPage::init(data_);
  SlottedPage fresh(data_);

  for (const auto& t : live_tuples) {
    fresh.insert_tuple(t.data(), static_cast<uint32_t>(t.size()));
  }

  header_ = fresh.header_;
  return true;
}

std::optional<uint16_t> SlottedPage::insert_tuple(const Byte* tuple_data, uint32_t tuple_size) {
  int target_slot_id = -1;
  for (int i = 0; i < header_.num_slots; ++i) {
    if (auto slot = get_slot(static_cast<uint16_t>(i)); slot && slot->length == 0) {
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

  header_.free_space_pointer -= static_cast<uint16_t>(tuple_size);
  std::memcpy(data_ + header_.free_space_pointer, tuple_data, tuple_size);

  Slot new_slot{static_cast<uint16_t>(header_.free_space_pointer),
                static_cast<uint16_t>(tuple_size)};

  if (target_slot_id != -1) {
    set_slot(static_cast<uint16_t>(target_slot_id), new_slot);
  } else {
    uint16_t new_slot_id = header_.num_slots;
    size_t off = slot_data_offset(new_slot_id);
    store_slot_to_bytes(data_ + off, new_slot);
    header_.num_slots++;
  }

  store_header();

  if (target_slot_id != -1) {
    return static_cast<uint16_t>(target_slot_id);
  }
  return static_cast<uint16_t>(header_.num_slots - 1);
}

const Byte* SlottedPage::get_tuple(uint16_t slot_id, uint32_t* size) const {
  auto slot = get_slot(slot_id);
  if (!slot || slot->length == 0) {
    return nullptr;
  }

  assert(slot->offset + slot->length <= PAGE_SIZE);

  if (size) *size = slot->length;
  return data_ + slot->offset;
}

bool SlottedPage::delete_tuple(uint16_t slot_id) {
  auto slot = get_slot(slot_id);
  if (!slot || slot->length == 0) {
    return false;
  }

  slot->length = 0;
  set_slot(slot_id, *slot);

  while (header_.num_slots > 0) {
    if (auto s = get_slot(static_cast<uint16_t>(header_.num_slots - 1)); s && s->length > 0) {
      break;
    }
    header_.num_slots--;
  }

  store_header();
  return true;
}

void SlottedPage::set_page_lsn(lsn_t lsn) {
  header_.page_lsn = lsn;
  store_header();
}

}
