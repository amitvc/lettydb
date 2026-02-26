#pragma once

#include <variant>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include "catalog/schema.h"

namespace letty {

/**
 * @brief Variant type representing a column value.
 * 
 * Supports all data types defined in DataType enum:
 * - INTEGER: int32_t
 * - DOUBLE: double
 * - VARCHAR: std::string
 * - BOOLEAN: bool
 * - DATE: std::string (stored as "YYYY-MM-DD")
 * - TIMESTAMP: std::string (stored as "YYYY-MM-DD HH:MM:SS")
 */
using Value = std::variant<
    std::monostate,  // NULL value
    int32_t,         // INTEGER
    double,          // DOUBLE
    std::string,     // VARCHAR, DATE, TIMESTAMP
    bool             // BOOLEAN
>;

/**
 * @class Tuple
 * @brief A serializable, schema-aware representation of a single database table row
 *
 * @details
 * A Tuple is the fundamental unit of row storage in Letty DB. It encapsulates
 * an ordered sequence of `Value` objects (see Value typedef) that correspond
 * positionally to the columns defined in a `Schema`.
 * =========================================
 * MEMORY LAYOUT & SERIALIZATION FORMAT
 * ========================================
 * Tuples are serialized into a contiguous byte buffer for storage inside a
 * `SlottedPage`. The layout depends on the DataType of each column:
 *
 *   Fixed-length fields:
 *     Written directly as their native binary representation.
 *
 *       INTEGER   → 4 bytes  (int32_t,  little-endian)
 *       DOUBLE    → 8 bytes  (IEEE 754 double)
 *       BOOLEAN   → 1 byte   (0x00 = false, 0x01 = true)
 *       DATE      → 10 bytes (ASCII, "YYYY-MM-DD", no null terminator)
 *       TIMESTAMP → 19 bytes (ASCII, "YYYY-MM-DD HH:MM:SS", no null terminator)
 *
 *   Variable-length fields:
 *     Prefixed with a 2-byte little-endian length header, followed by
 *     the raw character data (no null terminator).
 *
 *       VARCHAR   → [uint16_t length | char data...]
 *
 *   NULL values:
 *     Represented in-memory as std::monostate. On disk, a NULL bitmap
 *     at the head of the serialized buffer tracks which columns are NULL,
 *     allowing fixed-length columns to skip their slot entirely.
 *
 * Example (schema: id INTEGER, name VARCHAR, active BOOLEAN):
 *
 *   In-memory values : [42, "alice", true]
 *   Serialized bytes : [2A 00 00 00] [05 00 61 6C 69 63 65] [01]
 *                       ^^ int32_t    ^^ len  ^^ "alice"     ^^ bool
 *
 */
class Tuple {
 public:

  Tuple() = default;

  /**
   * @brief Constructs a tuple from a list of values.
   * @param values The column values in schema order.
   */
  explicit Tuple(std::vector<Value> values) : values_(std::move(values)) {}

  /**
   * @brief Adds a value to the tuple.
   */
  void add_value(Value value) { values_.push_back(std::move(value)); }

  /**
   * @brief Gets a value at a specific index.
   */
  const Value& get_value(size_t index) const { return values_.at(index); }

  /**
   * @brief Returns the number of values in the tuple.
   */
  size_t size() const { return values_.size(); }

  /**
   * @brief Serializes the tuple into a caller-provided buffer.
   *
   * @param schema The table schema defining column types and offsets.
   * @param buf    Destination buffer (must be at least buf_size bytes).
   * @param buf_size Size of the destination buffer in bytes.
   * @param[out] out_size The size of the serialized data.
   * @return true if serialization succeeded, false if buffer too small.
   */
  bool serialize(const Schema& schema, char* buf, uint32_t buf_size, uint32_t* out_size) const;

  /**
   * @brief Deserializes a tuple from a raw byte buffer.
   * 
   * @param schema The table schema.
   * @param data Pointer to the raw data.
   * @param size Size of the data.
   * @return A Tuple with the deserialized values.
   */
  static Tuple deserialize(const Schema& schema, const char* data, uint32_t size);

 private:
  std::vector<Value> values_;
};

}
