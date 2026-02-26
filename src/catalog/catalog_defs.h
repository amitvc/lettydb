#pragma once

#include <cstddef>
#include <cstdint>

namespace letty {

/**
 * @brief OIDs for system tables
 */
constexpr uint32_t SYS_TABLES_TABLE_OID = 1;
constexpr uint32_t SYS_COLUMNS_TABLE_OID = 2;

// This is hard limit we have on table names, column names.
constexpr size_t MAX_NAME_LENGTH = 32;

enum class DataType : uint8_t {
  INTEGER = 0,
  DOUBLE  = 1,
  VARCHAR = 2,
  BOOLEAN = 3,
  DATE = 4,
  TIMESTAMP = 5
};

}
