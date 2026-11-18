//
// Created by Amit Chavan on 11/17/26.
//

#include "storage/log_apply.h"
#include <cstring>
#include "storage/page.h"
#include "storage/storage_def.h"
#include "storage/page_utils.h"
#include "common/logger.h"

namespace letty {

bool apply_payload_to_page(Page* page, uint16_t offset, const ByteBuffer& payload) {
  if (offset + payload.size() > PAGE_SIZE) {
    LOG_RECOVERY_MGR_WARN("Patch offset {} + size {} exceeds page size",
                          offset, payload.size());
    return false;
  }
  std::memcpy(page->get_data() + offset, payload.data(), payload.size());
  return true;
}

bool apply_gam_bit(Page* page, uint16_t bit_index, bool set_bit) {
  GAMPage gam = load_page_layout<GAMPage>(page);
  Bitmap bitmap(gam.bitmap, GAM_MAX_BITS);
  if (set_bit) {
    bitmap.set(bit_index);
  } else {
    bitmap.clear(bit_index);
  }
  store_page_layout(page, gam);
  return true;
}

bool apply_iam_extent(Page* page, uint32_t extent_id, bool add) {
  IAMPage iam = load_page_layout<IAMPage>(page);
  if (add) {
    iam.add_extent(extent_id);
  } else {
    iam.remove_extent(extent_id);
  }
  store_page_layout(page, iam);
  return true;
}

bool apply_compensation(Page* page, const CompensationPatch& clr) {
  switch (clr.kind) {
    case CompensationKind::BytePatch:
      return apply_payload_to_page(page, clr.offset, clr.payload);
    case CompensationKind::GamSetBit:
      return apply_gam_bit(page, static_cast<uint16_t>(clr.param), true);
    case CompensationKind::GamClearBit:
      return apply_gam_bit(page, static_cast<uint16_t>(clr.param), false);
    case CompensationKind::IamAddExtent:
      return apply_iam_extent(page, clr.param, true);
    case CompensationKind::IamRemoveExtent:
      return apply_iam_extent(page, clr.param, false);
  }
  return false;
}

}
