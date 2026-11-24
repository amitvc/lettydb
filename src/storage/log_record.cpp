#include "storage/log_record.h"

#include <cstring>
#include <limits>
#include <utility>
#include "page_utils.h"
#include "common/logger.h"


namespace letty {


  bool is_valid_log_record_type(uint16_t raw_type) {
	switch (static_cast<LogRecordType>(raw_type)) {
	  case LogRecordType::BEGIN:
	  case LogRecordType::PAGE_UPDATE:
	  case LogRecordType::DB_METADATA_UPDATE:
	  case LogRecordType::PAGE_HEADER_UPDATE:
	  case LogRecordType::COMMIT:
	  case LogRecordType::ABORT:
	  case LogRecordType::COMPENSATION:
	  case LogRecordType::END:
	  case LogRecordType::BEGIN_CHECKPOINT:
	  case LogRecordType::END_CHECKPOINT:
		return true;
	}
	return false;
  }

  bool serialize_page_patch(const PagePatch &payload, ByteBuffer &out) {
	if (payload.page_id == INVALID_PAGE_ID) {
	  LOG_WAL_ERROR("Page id cannot be invalid");
	  return false;
	}

	// Revisit this. Don't think we can support payloads larger than 4096 bytes right now.
	if (payload.before_payload.size() > std::numeric_limits<uint16_t>::max() ||
		payload.after_payload.size() > std::numeric_limits<uint16_t>::max()) {
	  LOG_WAL_ERROR("We dont support payloads greater than {}", std::numeric_limits<uint16_t>::max());
	  return false;
	}


	if (payload.offset + payload.before_payload.size() > PAGE_SIZE ||
		payload.offset + payload.after_payload.size() > PAGE_SIZE) {
	  LOG_WAL_ERROR("Payload size going beyond 4k which is what we support right now. bpl sz = {}, apl sz {}", payload.before_payload.size(), payload.after_payload.size());
	  return false;
	}

	size_t total_size = PAGE_PATCH_HEADER_SIZE + payload.before_payload.size() + payload.after_payload.size();
	out.resize(total_size);

	auto cursor = 0;
	write_le32(out.data() + cursor, static_cast<uint32_t>(payload.page_id));
	cursor += sizeof(uint32_t);
	write_le16(out.data() + cursor, payload.offset);
	cursor += sizeof(uint16_t);
	write_le16(out.data() + cursor, static_cast<uint16_t>(payload.before_payload.size()));
	cursor += sizeof(uint16_t);
	write_le16(out.data() + cursor, static_cast<uint16_t>(payload.after_payload.size()));
	cursor += sizeof(uint16_t);
	std::memcpy(out.data() + cursor, payload.before_payload.data(), payload.before_payload.size());
	cursor += payload.before_payload.size();
	std::memcpy(out.data() + cursor, payload.after_payload.data(), payload.after_payload.size());
	cursor += payload.after_payload.size(); // Adding for completeness.
	return true;
  }


  bool deserialize_page_patch(const ByteBuffer &bytes, PagePatch& out) {
	if (bytes.size() < PAGE_PATCH_HEADER_SIZE) {
	  LOG_WAL_ERROR("Payload too short to contain a PagePatch header");
	  return false;
	}
	auto cursor = 0;
	auto page_id = static_cast<page_id_t>(read_le32(bytes.data()));
	cursor += sizeof(uint32_t);
	uint16_t offset = read_le16(bytes.data() + cursor);
	cursor += sizeof(uint16_t);
	uint16_t before_payloadsize = read_le16(bytes.data() + cursor);
	cursor += sizeof(uint16_t);
	uint16_t after_payloadsize = read_le16(bytes.data() + cursor);
	cursor += sizeof(uint16_t);

	if (page_id == INVALID_PAGE_ID) {
	  LOG_WAL_ERROR("Decoded page id is invalid");
	  return false;
	}

	if (PAGE_PATCH_HEADER_SIZE + before_payloadsize + after_payloadsize != bytes.size()) {
	  LOG_WAL_ERROR("Declared before/after payload sizes do not match the actual buffer size");
	  return false;
	}

	out.page_id = page_id;
	out.offset = offset;
	out.before_payload.assign(bytes.begin() + PAGE_PATCH_HEADER_SIZE,
							   bytes.begin() + PAGE_PATCH_HEADER_SIZE + before_payloadsize);
	out.after_payload.assign(bytes.begin() + PAGE_PATCH_HEADER_SIZE + before_payloadsize,
							  bytes.begin() + PAGE_PATCH_HEADER_SIZE + before_payloadsize + after_payloadsize);

	return true;
  }

  bool serialize_db_metadata_state(const DbMetadataState &state, ByteBuffer &out, size_t offset) {
	if (offset + DB_METADATA_STATE_SIZE > out.size()) {
	  out.resize(offset + DB_METADATA_STATE_SIZE);
	}
	auto cursor = offset;
	write_le32(out.data() + cursor, static_cast<uint32_t>(state.free_page_list_head_page));
	cursor += sizeof(uint32_t);
	write_le32(out.data() + cursor, state.page_count);
	cursor += sizeof(uint32_t);
	write_le16(out.data() + cursor, state.table_count);
	cursor += sizeof(uint16_t);
	write_le16(out.data() + cursor, state.next_table_id);
	return true;
  }

  bool deserialize_db_metadata_state(const ByteBuffer &bytes, DbMetadataState &out, size_t offset) {
	if (offset + DB_METADATA_STATE_SIZE > bytes.size()) {
	  LOG_WAL_ERROR("Bytes too short to deserialize DbMetadataState");
	  return false;
	}
	auto cursor = offset;
	out.free_page_list_head_page = static_cast<page_id_t>(read_le32(bytes.data() + cursor));
	cursor += sizeof(uint32_t);
	out.page_count = read_le32(bytes.data() + cursor);
	cursor += sizeof(uint32_t);
	out.table_count = read_le16(bytes.data() + cursor);
	cursor += sizeof(uint16_t);
	out.next_table_id = read_le16(bytes.data() + cursor);
	return true;
  }

  bool serialize_db_metadata_patch(const DbMetadataPatch &patch, ByteBuffer &out) {
	out.resize(DB_METADATA_PATCH_SIZE);
	serialize_db_metadata_state(patch.before, out, 0);
	serialize_db_metadata_state(patch.after, out, DB_METADATA_STATE_SIZE);
	return true;
  }

  bool deserialize_db_metadata_patch(const ByteBuffer &bytes, DbMetadataPatch &out) {
	if (bytes.size() != DB_METADATA_PATCH_SIZE) {
	  LOG_WAL_ERROR("DbMetadataPatch size mismatch: expected {}, got {}", DB_METADATA_PATCH_SIZE, bytes.size());
	  return false;
	}
	if (!deserialize_db_metadata_state(bytes, out.before, 0) ||
	    !deserialize_db_metadata_state(bytes, out.after, DB_METADATA_STATE_SIZE)) {
	  return false;
	}
	return true;
  }

  bool serialize_page_header(const PageHeader &header, ByteBuffer &out, size_t offset) {
	if (offset + PAGE_HEADER_MUTABLE_SIZE > out.size()) {
	  out.resize(offset + PAGE_HEADER_MUTABLE_SIZE);
	}
	Byte *cursor = out.data() + offset;

	write_le32(cursor, static_cast<uint32_t>(header.next_page_id));
	cursor += sizeof(uint32_t);
	write_le32(cursor, static_cast<uint32_t>(header.prev_page_id));
	cursor += sizeof(uint32_t);
	write_le16(cursor, header.table_id);
	cursor += sizeof(uint16_t);
	*cursor = static_cast<Byte>(header.page_type);
	cursor += sizeof(uint8_t);
	*cursor = static_cast<Byte>(header.reserved);
	return true;
  }

  bool deserialize_page_header(const ByteBuffer &bytes, PageHeader &header, size_t offset) {
	if (offset + PAGE_HEADER_MUTABLE_SIZE > bytes.size()) {
	  LOG_WAL_ERROR("Bytes too short to deserialize PageHeader fields");
	  return false;
	}
	const Byte *cursor = bytes.data() + offset;
	header.next_page_id = static_cast<page_id_t>(read_le32(cursor));
	cursor += sizeof(uint32_t);
	header.prev_page_id = static_cast<page_id_t>(read_le32(cursor));
	cursor += sizeof(uint32_t);
	header.table_id = read_le16(cursor);
	cursor += sizeof(uint16_t);
	header.page_type = static_cast<PageType>(*cursor);
	cursor += sizeof(uint8_t);
	header.reserved = static_cast<uint8_t>(*cursor);
	return true;
  }

  bool serialize_page_header_patch(const PageHeaderPatch &patch, ByteBuffer &out) {
	if (patch.page_id == INVALID_PAGE_ID) {
	  LOG_WAL_ERROR("Page id cannot be invalid in PageHeaderPatch");
	  return false;
	}
	out.resize(PAGE_HEADER_PATCH_SIZE);
	write_le32(out.data(), static_cast<uint32_t>(patch.page_id));

	serialize_page_header(patch.before, out, sizeof(uint32_t));
	serialize_page_header(patch.after, out, sizeof(uint32_t) + PAGE_HEADER_MUTABLE_SIZE);
	return true;
  }

  bool deserialize_page_header_patch(const ByteBuffer &bytes, PageHeaderPatch &out) {
	if (bytes.size() != PAGE_HEADER_PATCH_SIZE) {
	  LOG_WAL_ERROR("PageHeaderPatch size mismatch: expected {}, got {}", PAGE_HEADER_PATCH_SIZE, bytes.size());
	  return false;
	}
	out.page_id = static_cast<page_id_t>(read_le32(bytes.data()));
	if (!deserialize_page_header(bytes, out.before, sizeof(uint32_t)) ||
	    !deserialize_page_header(bytes, out.after, sizeof(uint32_t) + PAGE_HEADER_MUTABLE_SIZE)) {
	  return false;
	}
	return true;
  }

  bool serialize_compensation_patch(const CompensationPatch &compensation_patch, std::vector<Byte> &out) {
	if (compensation_patch.page_id == INVALID_PAGE_ID) {
	  LOG_WAL_ERROR("Page id cannot be invalid");
	  return false;
	}

	if (compensation_patch.payload.size() > std::numeric_limits<uint16_t>::max()) {
	  LOG_WAL_ERROR("We dont support payloads greater than {}", std::numeric_limits<uint16_t>::max());
	  return false;
	}

	if (compensation_patch.offset + compensation_patch.payload.size() > DEFAULT_PAGE_SIZE) {
	  return false;
	}

	size_t total_size = COMPENSATION_PATCH_HEADER_SIZE + compensation_patch.payload.size();
	out.resize(total_size);

	Byte* cursor = out.data();
	write_le32(cursor, static_cast<uint32_t>(compensation_patch.page_id));
	cursor += sizeof(uint32_t);
	*cursor = static_cast<Byte>(compensation_patch.kind);
	cursor += sizeof(Byte);
	write_le16(cursor, compensation_patch.offset);
	cursor += sizeof(uint16_t);

	write_le64(cursor, static_cast<uint64_t>(compensation_patch.undo_next_lsn));
	cursor += sizeof(uint64_t);

	write_le16(cursor, static_cast<uint16_t>(compensation_patch.payload.size()));
	cursor += sizeof(uint16_t);

	std::memcpy(cursor, compensation_patch.payload.data(), compensation_patch.payload.size());
	return true;
  }

  bool deserialize_compensation_patch(const ByteBuffer &bytes, CompensationPatch& compensation_patch) {
	if (bytes.size() < COMPENSATION_PATCH_HEADER_SIZE) {
	  LOG_WAL_ERROR("Payload too short to contain a CompensationPatch header");
	  return false;
	}

	const Byte* cursor = bytes.data();
	auto page_id = static_cast<page_id_t>(read_le32(cursor));
	cursor += sizeof(uint32_t);
	auto kind = static_cast<CompensationKind>(*cursor);
	cursor += sizeof(Byte);
	uint16_t offset = read_le16(cursor);
	cursor += sizeof(uint16_t);
	auto undo_next_lsn = static_cast<lsn_t>(read_le64(cursor));
	cursor += sizeof(uint64_t);
	uint16_t payload_size = read_le16(cursor);
	cursor += sizeof(uint16_t);

	if (page_id == INVALID_PAGE_ID) {
	  LOG_WAL_ERROR("Decoded page id is invalid");
	  return false;
	}

	if (static_cast<uint8_t>(kind) > static_cast<uint8_t>(CompensationKind::PageHeaderRestore)) {
	  LOG_WAL_ERROR("Decoded CompensationKind {} is out of range", static_cast<uint8_t>(kind));
	  return false;
	}

	if (COMPENSATION_PATCH_HEADER_SIZE + payload_size != bytes.size()) {
	  LOG_WAL_ERROR("Declared payload size does not match the actual buffer size");
	  return false;
	}

	compensation_patch.page_id = page_id;
	compensation_patch.kind = kind;
	compensation_patch.offset = offset;
	compensation_patch.undo_next_lsn = undo_next_lsn;
	compensation_patch.payload.assign(bytes.begin() + COMPENSATION_PATCH_HEADER_SIZE,
									   bytes.begin() + COMPENSATION_PATCH_HEADER_SIZE + payload_size);

	return true;
  }

bool serialize_checkpoint_patch(const CheckpointPatch &patch, ByteBuffer &out) {
  size_t total_size = CHECKPOINT_PATCH_HEADER_SIZE +
	  patch.dirty_pages.size() * DIRTY_PAGE_ENTRY_SIZE +
	  patch.active_transactions.size() * ACTIVE_TXN_ENTRY_SIZE;
  out.resize(total_size);

  auto cursor = 0;
  write_le32(out.data() + cursor, static_cast<uint32_t>(patch.dirty_pages.size()));
  cursor += sizeof(uint32_t);
  write_le32(out.data() + cursor, static_cast<uint32_t>(patch.active_transactions.size()));
  cursor += sizeof(uint32_t);

  for (const auto &[page_id, rec_lsn] : patch.dirty_pages) {
	write_le32(out.data() + cursor, static_cast<uint32_t>(page_id));
	cursor += sizeof(uint32_t);
	write_le64(out.data() + cursor, static_cast<uint64_t>(rec_lsn));
	cursor += sizeof(uint64_t);
  }

  for (const auto &[txn, last_lsn] : patch.active_transactions) {
	write_le64(out.data() + cursor, static_cast<uint64_t>(txn));
	cursor += sizeof(uint64_t);
	write_le64(out.data() + cursor, static_cast<uint64_t>(last_lsn));
	cursor += sizeof(uint64_t);
  }

  return true;
}

bool deserialize_checkpoint_patch(const ByteBuffer &bytes, CheckpointPatch &out) {
  if (bytes.size() < CHECKPOINT_PATCH_HEADER_SIZE) {
	LOG_WAL_ERROR("Payload too short to contain a CheckpointPatch header");
	return false;
  }

  auto cursor = 0;
  uint32_t dirty_pages_count = read_le32(bytes.data() + cursor);
  cursor += sizeof(uint32_t);
  uint32_t active_txns_count = read_le32(bytes.data() + cursor);
  cursor += sizeof(uint32_t);

  size_t expected_size = CHECKPOINT_PATCH_HEADER_SIZE +
	  static_cast<size_t>(dirty_pages_count) * DIRTY_PAGE_ENTRY_SIZE +
	  static_cast<size_t>(active_txns_count) * ACTIVE_TXN_ENTRY_SIZE;
  if (expected_size != bytes.size()) {
	LOG_WAL_ERROR("Declared dirty_pages/active_txns counts do not match the actual buffer size");
	return false;
  }

  out.dirty_pages.clear();
  out.dirty_pages.reserve(dirty_pages_count);
  for (uint32_t i = 0; i < dirty_pages_count; ++i) {
	auto page_id = static_cast<page_id_t>(read_le32(bytes.data() + cursor));
	cursor += sizeof(uint32_t);
	auto rec_lsn = static_cast<lsn_t>(read_le64(bytes.data() + cursor));
	cursor += sizeof(uint64_t);
	out.dirty_pages.emplace_back(page_id, rec_lsn);
  }

  out.active_transactions.clear();
  out.active_transactions.reserve(active_txns_count);
  for (uint32_t i = 0; i < active_txns_count; ++i) {
	auto txn = static_cast<txn_id>(read_le64(bytes.data() + cursor));
	cursor += sizeof(uint64_t);
	auto last_lsn = static_cast<lsn_t>(read_le64(bytes.data() + cursor));
	cursor += sizeof(uint64_t);
	out.active_transactions.emplace_back(txn, last_lsn);
  }

  return true;
}

bool serialize_checkpoint_marker(const CheckpointMarker &marker, ByteBuffer &out) {
  out.resize(sizeof(CheckpointMarker));
  auto cursor = 0;

  std::memcpy(out.data() + cursor, marker.signature, sizeof(marker.signature));
  cursor += sizeof(marker.signature);

  write_le64(out.data() + cursor, static_cast<uint64_t>(marker.last_checkpoint_lsn));
  cursor += sizeof(marker.last_checkpoint_lsn);

  uint32_t crc = compute_crc32(out.data(), cursor);
  write_le32(out.data() + cursor, crc);

  return true;
}

bool deserialize_checkpoint_marker(const ByteBuffer &bytes, CheckpointMarker &out) {
  if (bytes.size() != sizeof(CheckpointMarker)) {
	LOG_WAL_ERROR("CheckpointMarker buffer must be exactly {} bytes, got {}", sizeof(CheckpointMarker), bytes.size());
	return false;
  }

  auto cursor = 0;
  char signature[18]{};
  std::memcpy(signature, bytes.data() + cursor, sizeof(signature));
  cursor += sizeof(signature);

  if (std::memcmp(signature, CHECKPOINT_MARKER_SIGNATURE, sizeof(signature)) != 0) {
	LOG_WAL_ERROR("Invalid CheckpointMarker signature");
	return false;
  }

  auto rec_lsn = static_cast<lsn_t>(read_le64(bytes.data() + cursor));
  cursor += sizeof(lsn_t);

  uint32_t stored_checksum = read_le32(bytes.data() + cursor);
  uint32_t computed_checksum = compute_crc32(bytes.data(), cursor);

  if (stored_checksum != computed_checksum) {
	LOG_WAL_ERROR("CheckpointMarker checksum mismatch: expected {}, got {}", stored_checksum, computed_checksum);
	return false;
  }

  std::memcpy(out.signature, signature, sizeof(out.signature));
  out.last_checkpoint_lsn = rec_lsn;
  out.checksum = stored_checksum;

  return true;
}

bool build_page_patch_record(txn_id tran_id, const PagePatch &patch, LogRecord &out) {
	ByteBuffer payload;
	if (!serialize_page_patch(patch, payload)) {
	  LOG_WAL_ERROR("Could not serialize page_patch record");
	  return false;
	}

	out.header.type = LogRecordType::PAGE_UPDATE;
	out.header.lsn = 0;
	out.header.prev_lsn = 0;
	out.header.tran_id = tran_id;
	out.header.payload_size = static_cast<uint32_t>(payload.size());
	out.header.checksum = 0;
	out.payload = std::move(payload);
	return true;
  }

  bool build_compensation_patch_record(txn_id tran_id, const CompensationPatch &patch, LogRecord &out) {
	ByteBuffer payload;
	if (!serialize_compensation_patch(patch, payload)) {
	  LOG_WAL_ERROR("Could not serialize compensation patch record");
	  return false;
	}

	out.header.type = LogRecordType::COMPENSATION;
	out.header.lsn = 0;
	out.header.prev_lsn = 0;
	out.header.tran_id = tran_id;
	out.header.payload_size = static_cast<uint32_t>(payload.size());
	out.header.checksum = 0;
	out.payload = std::move(payload);
	return true;
  }

  bool serialize_log_record(const LogRecord &record, ByteBuffer &out) {
	if (record.header.payload_size != record.payload.size()) {
	  LOG_WAL_ERROR("Size of record payload does not match the payload size in LogRecordHeader");
	  return false;
	}

	auto total_size = sizeof(LogRecordHeader) + record.payload.size();
	out.resize(total_size);

	auto cursor = sizeof(uint32_t);
	write_le16(out.data() + cursor, static_cast<uint16_t>(record.header.type));
	cursor += sizeof(uint16_t);
	write_le64(out.data() + cursor, record.header.lsn);
	cursor += sizeof(lsn_t);
	write_le64(out.data() + cursor, record.header.prev_lsn);
	cursor += sizeof(lsn_t);
	write_le64(out.data() + cursor, record.header.tran_id);
	cursor += sizeof(txn_id);
	write_le32(out.data() + cursor, record.header.payload_size);
	cursor += sizeof(uint32_t);

	if (!record.payload.empty()) {
	  std::memcpy(out.data() + cursor, record.payload.data(), record.payload.size());
	}

	uint32_t crc = compute_crc32(out.data() + sizeof(uint32_t), out.size() - sizeof(uint32_t));
	write_le32(out.data(), crc);
	return true;
  }

  bool deserialize_log_record(const ByteBuffer &bytes, LogRecord &out) {
	if (bytes.size() < sizeof(LogRecordHeader)) {
	  LOG_WAL_ERROR("Buffer too short to contain a LogRecordHeader");
	  return false;
	}

	auto cursor = 0;
	uint32_t stored_checksum = read_le32(bytes.data() + cursor);
	cursor += sizeof(uint32_t);
	uint16_t raw_type = read_le16(bytes.data() + cursor);
	cursor += sizeof(uint16_t);
	auto lsn = read_le64(bytes.data() + cursor);
	cursor += sizeof(lsn_t);
	auto prev_lsn = read_le64(bytes.data() + cursor);
	cursor += sizeof(lsn_t);
	auto tran_id = read_le64(bytes.data() + cursor);
	cursor += sizeof(txn_id);
	uint32_t payload_size = read_le32(bytes.data() + cursor);
	cursor += sizeof(uint32_t);

	if (!is_valid_log_record_type(raw_type)) {
	  LOG_WAL_ERROR("Decoded LogRecordType is invalid: {}", raw_type);
	  return false;
	}

	if (sizeof(LogRecordHeader) + payload_size != bytes.size()) {
	  LOG_WAL_ERROR("Declared payload_size ({}) does not match buffer size ({})",
					payload_size, bytes.size());
	  return false;
	}

	uint32_t computed_checksum = compute_crc32(bytes.data() + sizeof(uint32_t), bytes.size() - sizeof(uint32_t));
	if (computed_checksum != stored_checksum) {
	  LOG_WAL_ERROR("LogRecord checksum mismatch: expected {}, got {}. Record type {}",
					stored_checksum, computed_checksum, raw_type);
	  return false;
	}

	out.header.checksum = stored_checksum;
	out.header.type = static_cast<LogRecordType>(raw_type);
	out.header.lsn = lsn;
	out.header.prev_lsn = prev_lsn;
	out.header.tran_id = tran_id;
	out.header.payload_size = payload_size;
	out.payload.assign(bytes.begin() + sizeof(LogRecordHeader), bytes.end());
	return true;
  }

  bool build_db_metadata_record(txn_id tran_id, const DbMetadataPatch &patch, LogRecord &out) {
	ByteBuffer payload;
	if (!serialize_db_metadata_patch(patch, payload)) {
	  LOG_WAL_ERROR("Could not serialize db metadata patch");
	  return false;
	}
	out.header.type = LogRecordType::DB_METADATA_UPDATE;
	out.header.lsn = 0;
	out.header.prev_lsn = 0;
	out.header.tran_id = tran_id;
	out.header.payload_size = static_cast<uint32_t>(payload.size());
	out.header.checksum = 0;
	out.payload = std::move(payload);
	return true;
  }

  bool build_page_header_record(txn_id tran_id, const PageHeaderPatch &patch, LogRecord &out) {
	ByteBuffer payload;
	if (!serialize_page_header_patch(patch, payload)) {
	  LOG_WAL_ERROR("Could not serialize page header patch");
	  return false;
	}
	out.header.type = LogRecordType::PAGE_HEADER_UPDATE;
	out.header.lsn = 0;
	out.header.prev_lsn = 0;
	out.header.tran_id = tran_id;
	out.header.payload_size = static_cast<uint32_t>(payload.size());
	out.header.checksum = 0;
	out.payload = std::move(payload);
	return true;
  }

  bool build_transaction_record(LogRecordType type, txn_id tran_id, LogRecord &out) {
	if (type == LogRecordType::PAGE_UPDATE || type == LogRecordType::COMPENSATION ||
	    type == LogRecordType::DB_METADATA_UPDATE || type == LogRecordType::PAGE_HEADER_UPDATE) {
	  LOG_WAL_ERROR("{} is not a transaction record. Aborting writing wal record for it",
					static_cast<uint16_t>(type));
	  return false;
	}
	out.header.type = type;
	out.header.lsn = 0;
	out.header.prev_lsn = 0;
	out.header.tran_id = tran_id;
	out.header.payload_size = 0;
	out.header.checksum = 0;
	out.payload.clear();
	return true;
  }
}
