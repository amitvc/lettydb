#pragma once

#include <string>
#include <utility>
#include "catalog/catalog_defs.h"

namespace letty {

/**
 * @class Column
 * @brief Describes a column in a database table.
 *
 * A column stores its name, data type, fixed tuple width, byte offset, and
 * nullability. Variable-length types use the explicitly supplied width.
 */
class Column {
 public:
  /**
   * @brief Creates a column with an explicitly supplied tuple width.
   * @param name Column name.
   * @param type Column data type.
   * @param length Column width in bytes in the serialized tuple.
   * @param offset Byte offset of the column in the serialized tuple.
   * @param nullable Whether the column may contain NULL values.
   */
  Column(std::string name, DataType type, uint16_t length, uint16_t offset, bool nullable = true)
      : name_(std::move(name)), type_(type), length_(length), offset_(offset), nullable_(nullable) {}

  /**
   * @brief Creates a nullable fixed-width column.
   * @param name Column name.
   * @param type Column data type.
   * @param offset Byte offset of the column in the serialized tuple.
   *
   * For variable-length types, the derived length is zero; use the other
   * constructor to provide the column's tuple width explicitly.
   */
  Column(std::string name, DataType type, uint16_t offset)
      : name_(std::move(name)), type_(type), offset_(offset), length_(fixed_length_of(type)), nullable_(true) {}

  /**
   * @brief Returns the serialized width of a fixed-width data type.
   * @param type Data type to inspect.
   * @return Width in bytes, or zero for variable-length and unknown types.
   */
  static constexpr uint16_t fixed_length_of(DataType type) {
	 switch (type) {
	   case DataType::INTEGER:   return 4;
	   case DataType::DOUBLE:    return 8;
	   case DataType::BOOLEAN:   return 1;
	   case DataType::DATE:      return 10;
	   case DataType::TIMESTAMP: return 19;
	   case DataType::VARCHAR:   return 0;
	 }
	 return 0;
  }

  /** @return Column name. */
  const std::string& get_name() const { return name_; }

  /** @return Column data type. */
  DataType get_type() const { return type_; }

  /** @return Column width in bytes in the serialized tuple. */
  uint16_t get_length() const { return length_; }

  /** @return Byte offset of the column in the serialized tuple. */
  uint16_t get_offset() const { return offset_; }

  /** @return True when the column may contain NULL values. */
  bool is_nullable() const { return nullable_; }

 private:
  std::string name_;
  DataType type_;
  uint16_t length_;
  uint16_t offset_;
  bool nullable_ = true;
};

}
