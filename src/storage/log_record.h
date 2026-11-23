#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "config.h"
#include "storage_def.h"

namespace letty {

/** @brief
 * Identifies record type for the WAL log record.
 * */
enum class LogRecordType : uint16_t {
  BEGIN,
  PAGE_UPDATE,
  DB_METADATA_UPDATE,
  PAGE_HEADER_UPDATE,
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
 * before_payload is for undo_transactions, after_payload is for redo.
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
 * @enum CompensationKind
 * @brief Identifies how a CLR's payload restores a page during undo/redo.
 *
 * - BytePatch: restores slotted-page tuple bytes at `offset` (slot id).
 * - DbMetadataRestore: restores mutable DatabaseMetadata fields on Page 0.
 * - PageHeaderRestore: restores mutable PageHeader fields (preserving page_lsn).
 */
enum class CompensationKind : uint8_t {
  BytePatch,
  DbMetadataRestore,
  PageHeaderRestore
};

/**
 * @struct CompensationPatch
 * @brief Builder input for a COMPENSATION record. Logs undoing one update.
 *
 * `kind` selects how `payload` is applied to `page_id` (slotted-page slot
 * restoration via BytePatch, Page 0 metadata via DbMetadataRestore, or page
 * header fields via PageHeaderRestore). A CLR is redo-only and never gets
 * undone itself, which keeps rollback from recursing and guarantees it terminates.
 *
 * undo_next_lsn is the important field. It's the prev_lsn of the record
 * being compensated for, i.e. where undo_transactions picks up next. If we crash
 * mid-rollback, recovery reads this off the last CLR and resumes there
 * instead of re-undoing what's already done. Not the same as the header's
 * own prev_lsn, which just points to whatever came before in append order.
 *
 * Not an on-disk struct, serialize field by field.
 */
struct CompensationPatch {
  page_id_t page_id;
  CompensationKind kind = CompensationKind::BytePatch;
  uint16_t offset = 0;          // BytePatch only (slot id)
  lsn_t undo_next_lsn;          // CRITICAL: next log record to undo (copied from target's PrevLSN)
  ByteBuffer payload;           // BytePatch, DbMetadataRestore, or PageHeaderRestore
};

static constexpr auto COMPENSATION_PATCH_HEADER_SIZE = 17; // page_id(4) + kind(1) + offset(2) + undo_next_lsn(8) + payload_size(2)

/**
 * @struct CheckpointPatch
 * @brief Builder input for an END_CHECKPOINT record.
 *
 * Carries a fuzzy snapshot of the dirty page table and active transaction
 * table, taken sometime after the matching BEGIN_CHECKPOINT was written.
 * BEGIN_CHECKPOINT itself carries no payload — it is a plain marker record
 * built with build_transaction_record(), whose LSN is what recovery's
 * Analysis phase rewinds to, since this snapshot may already be stale by
 * the time END_CHECKPOINT is durable.
 *
 * Not an on-disk struct, so serialize field by field.
 */
struct CheckpointPatch {
  std::vector<std::pair<page_id_t, lsn_t>> dirty_pages;   // page_id -> recLSN
  std::vector<std::pair<txn_id, lsn_t>> active_transactions;      // txn_id  -> last_lsn
};

static constexpr auto CHECKPOINT_PATCH_HEADER_SIZE = 8; // dirty_pages_count + active_txns_count
static constexpr auto DIRTY_PAGE_ENTRY_SIZE = 12;  // page_id_t + lsn_t
static constexpr auto ACTIVE_TXN_ENTRY_SIZE = 16;  // txn_id + lsn_t

static constexpr auto DB_METADATA_STATE_SIZE = 12; // free_head(4) + page_count(4) + table_count(2) + next_table_id(2)

/**
 * @struct DbMetadataPatch
 * @brief LogRecord payload representing changes in DatabaseMetadata (Page 0).
 */
struct DbMetadataPatch {
  DbMetadataState before;
  DbMetadataState after;
};
static constexpr auto DB_METADATA_PATCH_SIZE = DB_METADATA_STATE_SIZE * 2; // 24 bytes

/**
 * @struct PageHeaderPatch
 * @brief LogRecord payload representing changes to a page's PageHeader.
 *
 * Captures mutable fields of PageHeader (next_page_id, prev_page_id, table_id,
 * page_type, status) before and after mutation. page_lsn is managed by ARIES
 * redo/undo and is not serialized in this patch.
 */
struct PageHeaderPatch {
  page_id_t page_id;
  PageHeader before;
  PageHeader after;
};
static constexpr auto PAGE_HEADER_MUTABLE_SIZE = 12; // next(4) + prev(4) + table_id(2) + type(1) + status(1)
static constexpr auto PAGE_HEADER_PATCH_SIZE = 4 + (PAGE_HEADER_MUTABLE_SIZE * 2); // page_id(4) + before(12) + after(12) = 28 bytes

/**
 * @struct CheckpointMarker
 * @brief On-disk checkpoint marker file format.
 *
 * Stores the LSN of the last BEGIN_CHECKPOINT record, allowing recovery to
 * know where to start scanning the WAL. Written to a separate file
 * (e.g., `<db_path>.checkpoint`) for atomicity and to keep the database
 * header page read-only.
 *
 * Format:
 *   [signature (18B): "CHECK_POINT_MARKER"]
 *   [last_checkpoint_lsn (8B)]
 *   [checksum (4B): CRC32 of the above fields]
 * Total: 30 bytes
 */
#pragma pack(push, 1)
struct CheckpointMarker {
  char signature[18];       // "CHECK_POINT_MARKER"
  lsn_t last_checkpoint_lsn;
  uint32_t checksum;
};
#pragma pack(pop)
static_assert(sizeof(CheckpointMarker) == 30, "CheckpointMarker must be exactly 30 bytes");

static constexpr const char CHECKPOINT_MARKER_SIGNATURE[] = "CHECK_POINT_MARKER";


bool serialize_page_patch(const PagePatch &payload, ByteBuffer &out);
bool deserialize_page_patch(const ByteBuffer &bytes, PagePatch &out);
bool serialize_db_metadata_state(const DbMetadataState &state, ByteBuffer &out, size_t offset = 0);
bool deserialize_db_metadata_state(const ByteBuffer &bytes, DbMetadataState &out, size_t offset = 0);
bool serialize_db_metadata_patch(const DbMetadataPatch &patch, ByteBuffer &out);
bool deserialize_db_metadata_patch(const ByteBuffer &bytes, DbMetadataPatch &out);
bool serialize_page_header(const PageHeader &header, ByteBuffer &out, size_t offset = 0);
bool deserialize_page_header(const ByteBuffer &bytes, PageHeader &out, size_t offset = 0);
bool serialize_page_header_patch(const PageHeaderPatch &patch, ByteBuffer &out);
bool deserialize_page_header_patch(const ByteBuffer &bytes, PageHeaderPatch &out);
bool serialize_compensation_patch(const CompensationPatch &compensation_patch, ByteBuffer &out);
bool deserialize_compensation_patch(const ByteBuffer &bytes, CompensationPatch& compensation_patch);
bool serialize_checkpoint_patch(const CheckpointPatch &patch, ByteBuffer &out);
bool deserialize_checkpoint_patch(const ByteBuffer &bytes, CheckpointPatch &out);

bool serialize_checkpoint_marker(const CheckpointMarker &marker, ByteBuffer &out);

bool deserialize_checkpoint_marker(const ByteBuffer &bytes, CheckpointMarker &out);
bool serialize_log_record(const LogRecord &record, ByteBuffer &out);
bool deserialize_log_record(const ByteBuffer &bytes, LogRecord &out);
bool build_page_patch_record(txn_id tran_id, const PagePatch &patch, LogRecord &out);
bool build_db_metadata_record(txn_id tran_id, const DbMetadataPatch &patch, LogRecord &out);
bool build_page_header_record(txn_id tran_id, const PageHeaderPatch &patch, LogRecord &out);
bool build_compensation_patch_record(txn_id tran_id, const CompensationPatch &patch, LogRecord &out);
bool build_transaction_record(LogRecordType type, txn_id tran_id, LogRecord &out);
}
