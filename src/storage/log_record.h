#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "config.h"

namespace letty {

/** @brief
 * Identifies record type for the WAL log record.
 * */
enum class LogRecordType : uint16_t {
  BEGIN,
  PAGE_UPDATE,
  ALLOCATE_EXTENT,
  DEALLOCATE_EXTENT,
  IAM_ADD_EXTENT,
  IAM_REMOVE_EXTENT,
  COMMIT,
  ABORT,
  COMPENSATION,
  END,
  BEGIN_CHECKPOINT,
  END_CHECKPOINT,
};

#pragma pack(push, 1)
/**
 * @struct LogRecordHeader
 * @brief Common on-disk header for every WAL record.
 *
 * Packed explicitly so no padding bytes end up in a check-summed, persisted
 * struct — unpacked, the three 8-byte LSN/txn fields would force 2 bytes of
 * implementation-defined padding after `type`.
 */
struct LogRecordHeader {
  uint32_t checksum;
  LogRecordType type;
  lsn_t lsn;
  lsn_t prev_lsn;
  txn_id tran_id;
  uint32_t payload_size;
};
#pragma pack(pop)
static_assert(sizeof(LogRecordHeader) == 34,
			  "LogRecordHeader must be exactly 34 bytes");

/**
 * @struct LogRecord
 * @brief In-memory representation of a WAL record: header plus payload bytes.
 *
 * Not an on-disk struct. BEGIN/COMMIT/ABORT/END/checkpoint
 * records are a LogRecord with an empty payload; no separate wrapper type is
 * needed for them.
 */
struct LogRecord {
  LogRecordHeader header;
  // payload is a stream of bytes which are serialized field by field for each of log record structures.
  // See serialize/deserialize methods for specialized struct types supporting different actions on the database pages.
  std::vector<Byte> payload;
};

/**
 * @struct PagePatch
 * @brief Builder input for a PAGE_UPDATE record. One patch, before and after bytes.
 *
 * Covers things like inserting/updating or deleting a tuple.
 * before_payload is for undo, after_payload is for redo.
 * Not an on-disk struct, so serialize field by field, don't memcpy.
 * One struct = one patch. More patches or more pages means more records.
 */
struct PagePatch {
  page_id_t page_id;
  uint16_t offset;
  ByteBuffer before_payload;
  ByteBuffer after_payload;
};

static constexpr auto PAGE_PATCH_HEADER_SIZE = 10; // page_id + offset + before_payloadsize + after_payloadsize

/**
 * @struct CompensationPatch
 * @brief Builder input for a COMPENSATION record. Logs undoing one PAGE_UPDATE.
 *
 * Written during rollback. payload holds the restoration bytes (the
 * original before-image) applied at page_id/offset. Only one direction
 * needed here, not before/after like PagePatch, because a CLR is
 * redo-only and never gets undone itself. That's what keeps rollback from
 * recursing and guarantees it terminates.
 *
 * undo_next_lsn is the important field. It's the prev_lsn of the record
 * being compensated for, i.e. where undo picks up next. If we crash
 * mid-rollback, recovery reads this off the last CLR and resumes there
 * instead of re-undoing what's already done. Not the same as the header's
 * own prev_lsn, which just points to whatever came before in append order.
 *
 * Not an on-disk struct, serialize field by field.
 */
struct CompensationPatch {
  page_id_t page_id;
  uint16_t offset;
  lsn_t undo_next_lsn; // CRITICAL: Points to the next log record to undo (copied from target's PrevLSN)
  ByteBuffer payload;
};

static constexpr auto COMPENSATION_PATCH_HEADER_SIZE = 16; // page_id + offset + undo_next_lsn + payload_size


/**
 * @brief
 * LogRecord to capture changes done to an GAM page
 */
struct GamPatch {
  page_id_t gam_page_id;
  uint16_t bit_index;
};
static constexpr auto EXTENT_PATCH_SIZE = 6; // page_id_t + uint16_t

/**
 * @brief
 * LogRecord to capture changes done to an IAM page
 */
struct IamPatch {
  page_id_t iam_page_id;
  uint32_t extent_id;
};
static constexpr auto IAM_PATCH_SIZE = 8; // page_id_t + uint32_t


bool serialize_page_patch(const PagePatch &payload, ByteBuffer &out);
bool deserialize_page_patch(const ByteBuffer &bytes, PagePatch &out);
bool serialize_extent_patch(const GamPatch &payload, ByteBuffer &out);
bool deserialize_extent_patch(const ByteBuffer &bytes, GamPatch &out);
bool serialize_iam_patch(const IamPatch &payload, ByteBuffer &out);
bool deserialize_iam_patch(const ByteBuffer &bytes, IamPatch &out);

bool serialize_compensation_patch(const CompensationPatch &compensation_patch, ByteBuffer &out);
bool deserialize_compensation_patch(const ByteBuffer &bytes, CompensationPatch& compensation_patch);
bool serialize_log_record(const LogRecord &record, ByteBuffer &out);
bool deserialize_log_record(const ByteBuffer &bytes, LogRecord &out);
bool build_page_patch_record(txn_id tran_id, const PagePatch &patch, LogRecord &out);
bool build_compensation_patch_record(txn_id tran_id, const CompensationPatch &patch, LogRecord &out);
bool build_transaction_record(LogRecordType type, txn_id tran_id, LogRecord &out);

bool build_extent_record(LogRecordType type, txn_id tran_id, const GamPatch &patch, LogRecord &out);
bool build_iam_extent_record(LogRecordType type, txn_id tran_id, const IamPatch &patch, LogRecord &out);
}
