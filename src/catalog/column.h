#pragma once

#include <string>
#include <utility>
#include "catalog/catalog_defs.h"

namespace letty {

/**
 * @class Column
 * @brief Represents the metadata of a column.
 */
class Column {
 public:
  Column(std::string name, DataType type, uint16_t length, uint16_t offset)
      : name_(std::move(name)), type_(type), length_(length), offset_(offset) {}
      
  // Helper for fixed length types where length is implied
  Column(std::string name, DataType type, uint16_t offset)
      : name_(std::move(name)), type_(type), offset_(offset), length_(fixed_length_of(type)) {

  }

  static constexpr uint16_t fixed_length_of(DataType t) {
	switch (t) {
	  case DataType::INTEGER:   return 4;
	  case DataType::DOUBLE:    return 8;
	  case DataType::BOOLEAN:   return 1;
	  case DataType::DATE:      return 12; // 3 ints
	  case DataType::TIMESTAMP: return 24; // 6 ints
		// If you have VARCHAR / VARBINARY / TEXT etc:
	  case DataType::VARCHAR:   return 0;  // variable-length
	}
	return 0; // or throw/assert for unknown enum values
  }

  const std::string& get_name() const { return name_; }
  DataType get_type() const { return type_; }
  uint16_t get_length() const { return length_; }
  uint16_t get_offset() const { return offset_; }

 private:
  std::string name_;
  DataType type_;
  uint16_t length_;
  uint16_t offset_;
};

}
