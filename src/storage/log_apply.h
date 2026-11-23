//
// Created by Amit Chavan on 11/17/26.
//

#pragma once

#include <cstdint>

#include "storage/config.h"
#include "storage/log_record.h"

namespace letty {

class Page;

// Stateless page-transform primitives shared by the Redo and Undo paths. Each
// applies one change to a pinned page's bytes; the caller is responsible for
// page_lsn and dirty-flag bookkeeping.
bool apply_payload_to_page(Page* page, uint16_t offset, const ByteBuffer& payload);
bool apply_db_metadata_state(Page* page, const DbMetadataState& state);
bool apply_page_header_patch(Page* page, const PageHeader& header);
bool apply_compensation(Page* page, const CompensationPatch& clr);
}
