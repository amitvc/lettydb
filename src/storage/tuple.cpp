#include "tuple.h"
#include "common/db_exception.h"
#include "page_utils.h"

namespace letty {

uint32_t null_bitmap_size(size_t column_count) {
  return static_cast<uint32_t>((column_count + 7) / 8); // ceil(column_count / 8)
}

bool is_null_bit_set(const Byte * bitmap, size_t index) {
  const size_t byte_index = index / 8;
  const size_t bit_index = index % 8;
  const Byte bit_mask = static_cast<Byte>(1u << bit_index);
  return (bitmap[byte_index] & bit_mask) != 0;
}

void set_null_bit(Byte * bitmap, size_t index) {
  const size_t byte_index = index / 8;
  const size_t bit_index = index % 8;
  const Byte bit_mask = static_cast<Byte>(1u << bit_index);
  bitmap[byte_index] |= bit_mask;
}

uint32_t Tuple::serialize(const Schema &schema, Byte *buf, uint32_t buf_size) const {
  const auto &columns = schema.get_columns();

  if (values_.size() != columns.size()) {
	throw DbException(DbErrorCode::InvalidArgument, "Tuple value count doesn't match schema column count");
  }

  /**
   * It uses 2 phase approach to save on space for null columns
   * It builds a null bitmap first for columns who have null values
   * and then in 2nd phase writes only those column values which are non null.
   */
  uint32_t bitmap_size = null_bitmap_size(columns.size());
  uint32_t total_size = bitmap_size;
  for (size_t i = 0; i < columns.size(); ++i) {
	const auto &col = columns[i];
	const bool is_null = std::holds_alternative<std::monostate>(values_[i]);
	if (is_null) {
	  continue;
	}
	if (col.get_type() == DataType::VARCHAR) {
	  size_t str_len = std::get<std::string>(values_[i]).size();
	  total_size += 2 + str_len;
	} else {
	  total_size += col.get_length();
	}
  }

  if (total_size > buf_size) {
	throw DbException(DbErrorCode::InvalidArgument,
					  "Tuple serialization requires " + std::to_string(total_size) +
					  " bytes, but buffer only has " + std::to_string(buf_size) + " bytes");
  }

  std::memset(buf, 0, bitmap_size);
  for (size_t i = 0; i < values_.size(); ++i) {
	if (std::holds_alternative<std::monostate>(values_[i])) {
	  set_null_bit(buf, i);
	}
  }

  // Serialize each value
  uint32_t offset = bitmap_size;
  for (size_t i = 0; i < columns.size(); ++i) {
	const auto &col = columns[i];
	const auto &value = values_[i];

	auto is_null = std::holds_alternative<std::monostate>(value);

	if (is_null) {
	  continue; // We don't write anything for null columns values
	}

	switch (col.get_type()) {
	  case DataType::TINYINT: {
		buf[offset] = static_cast<Byte>(std::get<int8_t>(value));
		offset += sizeof(int8_t);
		break;
	  }
	  case DataType::SMALLINT: {
		write_le16(buf + offset, static_cast<uint16_t>(std::get<int16_t>(value)));
		offset += sizeof(int16_t);
		break;
	  }
	  case DataType::INTEGER: {
		write_le32(buf + offset, static_cast<uint32_t>(std::get<int32_t>(value)));
		offset += sizeof(int32_t);
		break;
	  }
	  case DataType::BIGINT: {
		write_le64(buf + offset, static_cast<uint64_t>(std::get<int64_t>(value)));
		offset += sizeof(int64_t);
		break;
	  }
	  case DataType::DOUBLE: {
		auto dbl_val = std::get<double>(value);
		uint64_t dbl_bits;
		std::memcpy(&dbl_bits, &dbl_val, sizeof(dbl_bits));
		write_le64(buf + offset, dbl_bits);
		offset += sizeof(double);
		break;
	  }
	  case DataType::BOOLEAN: {
		buf[offset] = std::get<bool>(value);
		offset += 1;
		break;
	  }
	  case DataType::VARCHAR: {
		const auto &str = std::get<std::string>(value);
		auto len = static_cast<uint16_t>(str.size());
		write_le16(buf + offset, len);
		offset += sizeof(uint16_t);
		std::memcpy(buf + offset, str.data(), str.size());
		offset += str.size();
		break;
	  }
	  case DataType::DATE:
	  case DataType::TIMESTAMP: {
		const auto &str = std::get<std::string>(value);
		std::memcpy(buf + offset, str.data(), std::min(str.size(), static_cast<size_t>(col.get_length())));
		offset += col.get_length();
		break;
	  }
	}
  }

  return total_size;
}

Tuple Tuple::deserialize(const Schema &schema, const Byte *data, uint32_t size) {
  Tuple tuple;
  const auto &columns = schema.get_columns();

  uint32_t bitmap_size = null_bitmap_size(columns.size());
  if (size < bitmap_size) {
	throw DbException(DbErrorCode::Corruption, "Tuple data too short to contain null bitmap");
  }

  const uint8_t* null_bitmap = reinterpret_cast<const uint8_t*>(data);
  uint32_t offset = bitmap_size;
  for (size_t i = 0; i < columns.size(); ++i) {
	const auto &col = columns[i];
	const bool is_null = is_null_bit_set(null_bitmap, i);

	if (is_null) {
	  tuple.add_value(Value{std::monostate()});
	  continue;
	}

	if (offset >= size) {
	  throw DbException(DbErrorCode::Corruption, "Tuple data truncated before all columns were read");
	}

	switch (col.get_type()) {
	  case DataType::TINYINT: {
		int8_t val = static_cast<int8_t>(data[offset]);
		tuple.add_value(Value{val});
		offset += sizeof(int8_t);
		break;
	  }
	  case DataType::SMALLINT: {
		int16_t val = static_cast<int16_t>(read_le16(data + offset));
		tuple.add_value(Value{val});
		offset += sizeof(int16_t);
		break;
	  }
	  case DataType::INTEGER: {
		int32_t int_val = static_cast<int32_t>(read_le32(data + offset));
		tuple.add_value(Value{int_val});
		offset += sizeof(int32_t);
		break;
	  }
	  case DataType::BIGINT: {
		int64_t val = static_cast<int64_t>(read_le64(data + offset));
		tuple.add_value(Value{val});
		offset += sizeof(int64_t);
		break;
	  }
	  case DataType::DOUBLE: {
		uint64_t dbl_bits = read_le64(data + offset);
		double dbl_val;
		std::memcpy(&dbl_val, &dbl_bits, sizeof(dbl_val));
		tuple.add_value(Value{dbl_val});
		offset += sizeof(double);
		break;
	  }
	  case DataType::BOOLEAN: {
		bool bool_val = (data[offset] != 0);
		tuple.add_value(Value{bool_val});
		offset += 1;
		break;
	  }
	  case DataType::VARCHAR: {
		// Variable length: 2-byte length prefix
		uint16_t len = read_le16(data + offset);
		offset += sizeof(uint16_t);
		std::string str(reinterpret_cast<const char*>(data + offset), len);
		tuple.add_value(Value{str});
		offset += len;
		break;
	  }
	  case DataType::DATE:
	  case DataType::TIMESTAMP: {
		// Fixed-length string
		std::string str(reinterpret_cast<const char*>(data + offset), col.get_length());
		// Trim null padding
		size_t end = str.find('\0');
		if (end != std::string::npos) str.resize(end);
		tuple.add_value(Value{str});
		offset += col.get_length();
		break;
	  }
	}
  }

  return tuple;
}

}
