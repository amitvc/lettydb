//
// Created by Antigravity on 1/27/26.
//

#include "tuple.h"
#include <stdexcept>

namespace letty {

bool Tuple::serialize(const Schema& schema, char* buf, uint32_t buf_size, uint32_t* out_size) const {
  const auto& columns = schema.get_columns();

  if (values_.size() != columns.size()) {
    throw std::runtime_error("Tuple value count doesn't match schema column count");
  }

  // Calculate total size needed
  uint32_t total_size = 0;
  for (size_t i = 0; i < columns.size(); ++i) {
    const auto& col = columns[i];
    if (col.get_type() == DataType::VARCHAR) {
      const auto& str = std::get<std::string>(values_[i]);
      total_size += 2 + str.size();
    } else {
      total_size += col.get_length();
    }
  }

  *out_size = total_size;
  if (total_size > buf_size) {
    return false;  // Buffer too small
  }

  std::memset(buf, 0, total_size);

  // Serialize each value
  uint32_t offset = 0;
  for (size_t i = 0; i < columns.size(); ++i) {
    const auto& col = columns[i];
    const auto& value = values_[i];

    switch (col.get_type()) {
      case DataType::INTEGER: {
        int32_t int_val = std::get<int32_t>(value);
        std::memcpy(buf + offset, &int_val, sizeof(int32_t));
        offset += sizeof(int32_t);
        break;
      }
      case DataType::DOUBLE: {
        double dbl_val = std::get<double>(value);
        std::memcpy(buf + offset, &dbl_val, sizeof(double));
        offset += sizeof(double);
        break;
      }
      case DataType::BOOLEAN: {
        bool bool_val = std::get<bool>(value);
		buf[offset] = bool_val ? 1 : 0;
        offset += 1;
        break;
      }
      case DataType::VARCHAR:
      case DataType::DATE:
      case DataType::TIMESTAMP: {
        const auto& str = std::get<std::string>(value);
        if (col.get_type() == DataType::VARCHAR) {
          uint16_t len = static_cast<uint16_t>(str.size());
          std::memcpy(buf + offset, &len, sizeof(uint16_t));
          offset += sizeof(uint16_t);
          std::memcpy(buf + offset, str.data(), str.size());
          offset += str.size();
        } else {
          std::memcpy(buf + offset, str.data(), std::min(str.size(), static_cast<size_t>(col.get_length())));
          offset += col.get_length();
        }
        break;
      }
    }
  }

  return true;
}


Tuple Tuple::deserialize(const Schema& schema, const char* data, uint32_t size) {
  Tuple tuple;
  const auto& columns = schema.get_columns();
  
  uint32_t offset = 0;
  for (const auto& col : columns) {
    if (offset >= size) break;
    
    switch (col.get_type()) {
      case DataType::INTEGER: {
        int32_t int_val;
        std::memcpy(&int_val, data + offset, sizeof(int32_t));
        tuple.add_value(int_val);
        offset += sizeof(int32_t);
        break;
      }
      case DataType::DOUBLE: {
        double dbl_val;
        std::memcpy(&dbl_val, data + offset, sizeof(double));
        tuple.add_value(dbl_val);
        offset += sizeof(double);
        break;
      }
      case DataType::BOOLEAN: {
        bool bool_val = (data[offset] != 0);
        tuple.add_value(bool_val);
        offset += 1;
        break;
      }
      case DataType::VARCHAR: {
        // Variable length: 2-byte length prefix
        uint16_t len;
        std::memcpy(&len, data + offset, sizeof(uint16_t));
        offset += sizeof(uint16_t);
        std::string str(data + offset, len);
        tuple.add_value(str);
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
        tuple.add_value(str);
        offset += col.get_length();
        break;
      }
    }
  }
  
  return tuple;
}

} // namespace letty
