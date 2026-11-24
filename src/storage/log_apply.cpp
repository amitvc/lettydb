//
// Created by Amit Chavan on 11/17/26.
//

#include "storage/log_apply.h"
#include "storage/page.h"
#include "storage/slotted_page.h"
#include "storage/storage_def.h"
#include "storage/page_utils.h"
#include "common/logger.h"

namespace letty {

bool apply_payload_to_page(Page* page, uint16_t offset, const ByteBuffer& payload) {
  SlottedPage slotted_page = SlottedPage::from_page(page);
  if (!slotted_page.apply_slot_payload(offset, payload.data(), static_cast<uint32_t>(payload.size()))) {
    LOG_RECOVERY_MGR_WARN("Failed to apply slot payload at slot {} (size {}) on page {}",
                          offset, payload.size(), page->get_page_id());
    return false;
  }
  return true;
}

bool apply_db_metadata_state(Page* page, const DbMetadataState& state) {
  DatabaseMetadata meta = load_page_body<DatabaseMetadata>(page);
  meta.free_page_list_head_page = state.free_page_list_head_page;
  meta.page_count = state.page_count;
  meta.table_count = state.table_count;
  meta.next_table_id = state.next_table_id;
  store_page_body<DatabaseMetadata>(page, meta);
  return true;
}

bool apply_page_header_patch(Page* page, const PageHeader& header) {
  lsn_t existing_lsn = page->get_page_lsn();
  PageHeader page_header = header;
  page_header.page_lsn = existing_lsn;
  page->set_header(page_header);

  if (header.page_type == PageType::DATA) {
    SlotDirectoryHeader slot_dir = load_header_from_bytes(reinterpret_cast<const Byte*>(page->get_body()));
    if (slot_dir.num_slots == 0 && slot_dir.free_space_pointer == 0) {
      SlottedPage::init(reinterpret_cast<Byte*>(page->get_body()));
    }
  }
  return true;
}

bool apply_compensation(Page* page, const CompensationPatch& clr) {
  switch (clr.kind) {
    case CompensationKind::BytePatch:
      return apply_payload_to_page(page, clr.offset, clr.payload);
    case CompensationKind::DbMetadataRestore: {
      DbMetadataState state{};
      if (!deserialize_db_metadata_state(clr.payload, state)) {
        return false;
      }
      return apply_db_metadata_state(page, state);
    }
    case CompensationKind::PageHeaderRestore: {
      PageHeader header{};
      if (!deserialize_page_header(clr.payload, header)) {
        return false;
      }
      return apply_page_header_patch(page, header);
    }
  }
  return false;
}

}
