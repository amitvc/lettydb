#pragma once

#include <vector>
#include <optional>
#include "catalog/column.h"

namespace letty {

/**
 * @class Schema
 * @brief Represents the schema of a table (a collection of columns).
 *
 * Column order in the Schema matters — tuples are serialized in the same order columns appear here.
 */
class Schema {
 public:
  Schema() = default;
  explicit Schema(std::vector<Column> columns) : columns_(std::move(columns)) {}
  inline const std::vector<Column> &get_columns() const { return columns_; }

 private:
  std::vector<Column> columns_;
};
}
