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

static_assert(sizeof(SlotDirectoryHeader) == 4,
              "SlotDirectoryHeader must be exactly 4 bytes on disk");
static_assert(sizeof(Slot) == 4, "Slot must be exactly 4 bytes on disk");

SlottedPage SlottedPage::from_page(Page* page) {
  return SlottedPage(reinterpret_cast<Byte*>(page->get_body()));
}

SlottedPage::SlottedPage(Byte* buffer)
    : data_(buffer),
      header_(load_header_from_bytes(buffer)) {
}

SlottedPage SlottedPage::init(Byte* buffer) {
  std::memset(buffer, 0, PAGE_BODY_SIZE);

  SlotDirectoryHeader header{};
  header.free_space_pointer = PAGE_BODY_SIZE;
  header.num_slots = 0;
  store_header_to_bytes(buffer, header);
  return SlottedPage(buffer);
}

void SlottedPage::store_header() {
  store_header_to_bytes(data_, header_);
}

size_t SlottedPage::slot_data_offset(uint16_t slot_id) {
  return sizeof(SlotDirectoryHeader) + (slot_id * sizeof(Slot));
}

size_t SlottedPage::get_free_space() const {
  size_t headers_size = sizeof(SlotDirectoryHeader) + (header_.num_slots * sizeof(Slot));
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

  std::memset(data_, 0, PAGE_BODY_SIZE);
  header_.num_slots = 0;
  header_.free_space_pointer = PAGE_BODY_SIZE;
  store_header();

  for (const auto& t : live_tuples) {
    insert_tuple(t.data(), static_cast<uint32_t>(t.size()));
  }

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

  assert(slot->offset + slot->length <= PAGE_BODY_SIZE);

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

void SlottedPage::repack_tuples_preserving_slots(uint16_t excluded_slot_id) {
  std::vector< std::pair<uint16_t, std::vector<Byte>> > live_slots;
  for (uint16_t i = 0; i < header_.num_slots; ++i) {
    if (i == excluded_slot_id) {
      continue;
    }
    auto slot = get_slot(i);
    if (slot && slot->length > 0) {
      std::vector<Byte> buf(slot->length);
      std::memcpy(buf.data(), data_ + slot->offset, slot->length);
      live_slots.emplace_back(i, std::move(buf));
    }
  }

  header_.free_space_pointer = PAGE_BODY_SIZE;
  for (const auto& entry : live_slots) {
    uint16_t slot_idx = entry.first;
    const std::vector<Byte>& tuple_bytes = entry.second;
    uint16_t len = static_cast<uint16_t>(tuple_bytes.size());
    header_.free_space_pointer -= len;
    std::memcpy(data_ + header_.free_space_pointer, tuple_bytes.data(), len);
    set_slot(slot_idx, Slot{header_.free_space_pointer, len});
  }
}

bool SlottedPage::apply_slot_payload(uint16_t slot_id, const Byte* tuple_data, uint32_t tuple_size) {
  if (header_.free_space_pointer == 0 && header_.num_slots == 0) {
    header_.free_space_pointer = PAGE_BODY_SIZE;
  }

  if (tuple_size == 0) {
    if (slot_id >= header_.num_slots) {
      return true;
    }
    auto slot = get_slot(slot_id);
    if (!slot || slot->length == 0) {
      return true;
    }
    return delete_tuple(slot_id);
  }

  uint16_t required_slots = (slot_id >= header_.num_slots) ? static_cast<uint16_t>(slot_id + 1) : header_.num_slots;
  size_t required_header_bytes = sizeof(SlotDirectoryHeader) + (required_slots * sizeof(Slot));

  if (required_header_bytes + tuple_size > header_.free_space_pointer) {
    repack_tuples_preserving_slots(slot_id);
    if (required_header_bytes + tuple_size > header_.free_space_pointer) {
      return false;
    }
  }

  if (slot_id >= header_.num_slots) {
    for (uint16_t i = header_.num_slots; i <= slot_id; ++i) {
      store_slot_to_bytes(data_ + slot_data_offset(i), Slot{0, 0});
    }
    header_.num_slots = required_slots;
  }

  header_.free_space_pointer -= static_cast<uint16_t>(tuple_size);
  std::memcpy(data_ + header_.free_space_pointer, tuple_data, tuple_size);
  set_slot(slot_id, Slot{header_.free_space_pointer, static_cast<uint16_t>(tuple_size)});
  store_header();
  return true;
}

}
