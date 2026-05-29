#include "tuple.h"
#include "common/db_exception.h"

namespace letty {

uint32_t null_bitmap_size(size_t column_count) {
  return static_cast<uint32_t>((column_count + 7) / 8); // ceil(column_count / 8)
}

bool is_null_bit_set(const uint8_t* bitmap, size_t index) {
  const size_t byte_index = index / 8;
  const size_t bit_index = index % 8;
  const uint8_t bit_mask = static_cast<uint8_t>(1u << bit_index);
  return (bitmap[byte_index] & bit_mask) != 0;
}

void set_null_bit(uint8_t* bitmap, size_t index) {
  const size_t byte_index = index / 8;
  const size_t bit_index = index % 8;
  const uint8_t bit_mask = static_cast<uint8_t>(1u << bit_index);
  bitmap[byte_index] |= bit_mask;
}

bool Tuple::serialize(const Schema &schema, char *buf, uint32_t buf_size, uint32_t *out_size) const {
  const auto &columns = schema.get_columns();

  if (values_.size() != columns.size()) {
	throw DbException(DbErrorCode::InvalidArgument, "Tuple value count doesn't match schema column count");
  }

  // Calculate total size needed
  uint32_t bitmap_size = null_bitmap_size(columns.size());
  uint32_t total_size = bitmap_size;
  for (size_t i = 0; i < columns.size(); ++i) {
	const auto &col = columns[i];
	if (col.get_type() == DataType::VARCHAR) {
	  const bool is_null = std::holds_alternative<std::monostate>(values_[i]);
	  size_t str_len = is_null ? 0 : std::get<std::string>(values_[i]).size();
	  total_size += 2 + str_len;
	} else {
	  total_size += col.get_length();
	}
  }

  *out_size = total_size;
  if (total_size > buf_size) {
	throw DbException(DbErrorCode::InvalidArgument,
					  "Tuple serialization requires " + std::to_string(total_size) +
					  " bytes, but buffer only has " + std::to_string(buf_size) + " bytes");
  }

  std::memset(buf, 0, bitmap_size);
  for (size_t i = 0; i < values_.size(); ++i) {
	if (std::holds_alternative<std::monostate>(values_[i])) {
	  set_null_bit(reinterpret_cast<uint8_t*>(buf), i);
	}
  }

  // Serialize each value
  uint32_t offset = bitmap_size;
  for (size_t i = 0; i < columns.size(); ++i) {
	const auto &col = columns[i];
	const auto &value = values_[i];

	const bool is_null = std::holds_alternative<std::monostate>(value);

	switch (col.get_type()) {
	  case DataType::INTEGER: {
		int32_t int_val = is_null ? 0 : std::get<int32_t>(value);
		std::memcpy(buf + offset, &int_val, sizeof(int32_t));
		offset += sizeof(int32_t);
		break;
	  }
	  case DataType::DOUBLE: {
		double dbl_val = is_null ? 0.0 : std::get<double>(value);
		std::memcpy(buf + offset, &dbl_val, sizeof(double));
		offset += sizeof(double);
		break;
	  }
	  case DataType::BOOLEAN: {
		buf[offset] = (!is_null && std::get<bool>(value)) ? 1 : 0;
		offset += 1;
		break;
	  }
	  case DataType::VARCHAR: {
		const auto &str = is_null ? "" : std::get<std::string>(value);
		uint16_t len = static_cast<uint16_t>(str.size());
		std::memcpy(buf + offset, &len, sizeof(uint16_t));
		offset += sizeof(uint16_t);
		std::memcpy(buf + offset, str.data(), str.size());
		offset += str.size();
		break;
	  }
	  case DataType::DATE:
	  case DataType::TIMESTAMP: {
		const auto &str = is_null ? "" : std::get<std::string>(value);
		std::memcpy(buf + offset, str.data(), std::min(str.size(), static_cast<size_t>(col.get_length())));
		offset += col.get_length();
		break;
	  }
	}
  }

  return true;
}

Tuple Tuple::deserialize(const Schema &schema, const char *data, uint32_t size) {
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
	if (offset >= size) {
	  throw DbException(DbErrorCode::Corruption, "Tuple data truncated before all columns were read");
	}
	const bool is_null = is_null_bit_set(null_bitmap, i);

	switch (col.get_type()) {
	  case DataType::INTEGER: {
		int32_t int_val;
		std::memcpy(&int_val, data + offset, sizeof(int32_t));
		tuple.add_value(is_null ? Value{std::monostate{}} : Value{int_val});
		offset += sizeof(int32_t);
		break;
	  }
	  case DataType::DOUBLE: {
		double dbl_val;
		std::memcpy(&dbl_val, data + offset, sizeof(double));
		tuple.add_value(is_null ? Value{std::monostate{}} : Value{dbl_val});
		offset += sizeof(double);
		break;
	  }
	  case DataType::BOOLEAN: {
		bool bool_val = (data[offset] != 0);
		tuple.add_value(is_null ? Value{std::monostate{}} : Value{bool_val});
		offset += 1;
		break;
	  }
	  case DataType::VARCHAR: {
		// Variable length: 2-byte length prefix
		uint16_t len;
		std::memcpy(&len, data + offset, sizeof(uint16_t));
		offset += sizeof(uint16_t);
		std::string str(data + offset, len);
		tuple.add_value(is_null ? Value{std::monostate{}} : Value{str});
		offset += len;
		break;
	  }
	  case DataType::DATE:
	  case DataType::TIMESTAMP: {
		// Fixed-length string
		std::string str(data + offset, col.get_length());
		// Trim null padding
		size_t end = str.find('\0');
		if (end != std::string::npos) str.resize(end);
		tuple.add_value(is_null ? Value{std::monostate{}} : Value{str});
		offset += col.get_length();
		break;
	  }
	}
  }

  return tuple;
}

}
