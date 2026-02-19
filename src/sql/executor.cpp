//
// Created by Antigravity on 1/27/26.
//

#include "executor.h"
#include "token.h"
#include <iostream>

namespace letty {

Executor::Executor(CatalogManager& catalog_manager, TableManager& table_manager)
    : catalog_manager_(catalog_manager), table_manager_(table_manager) {}

ExecutionResult Executor::execute(ASTNode* node) {
  if (!node) {
    return ExecutionResult::error("Null AST node");
  }
  
  // Dispatch based on node type using dynamic_cast
  if (auto* create_table = dynamic_cast<CreateTableStatementNode*>(node)) {
    return execute_create_table(create_table);
  }
  
  if (auto* insert = dynamic_cast<InsertStatementNode*>(node)) {
    return execute_insert(insert);
  }
  
  if (auto* select = dynamic_cast<SelectStatementNode*>(node)) {
    return execute_select(select);
  }
  
  return ExecutionResult::error("Unsupported statement type");
}

ExecutionResult Executor::execute_create_table(CreateTableStatementNode* node) {
  if (!node->table_name) {
    return ExecutionResult::error("CREATE TABLE: missing table name");
  }
  
  std::string table_name = node->table_name->name;
  
  // Build Schema from column definitions
  std::vector<Column> columns;
  uint16_t current_offset = 0;
  
  for (const auto& col_def : node->columns) {
    if (!col_def || !col_def->name) {
      return ExecutionResult::error("CREATE TABLE: invalid column definition");
    }
    
    std::string col_name = col_def->name->name;
    DataType data_type = token_type_to_data_type(col_def->type);
    
    // Determine column length
    uint16_t col_length;
    if (data_type == DataType::VARCHAR) {
      col_length = col_def->size > 0 ? col_def->size : 255;  // Default VARCHAR size
    } else {
      col_length = Column::fixed_length_of(data_type);
    }
    
    columns.emplace_back(col_name, data_type, col_length, current_offset);
    current_offset += col_length;
  }
  
  Schema schema(columns);
  
  // Create the table
  if (!catalog_manager_.create_table(table_name, schema)) {
    return ExecutionResult::error("CREATE TABLE: failed to create table '" + table_name + "' (may already exist)");
  }
  
  return ExecutionResult::ok();
}

ExecutionResult Executor::execute_insert(InsertStatementNode* node) {
  if (!node->tableName) {
    return ExecutionResult::error("INSERT: missing table name");
  }
  
  std::string table_name = node->tableName->name;
  
  // Get table metadata
  auto* table_meta = catalog_manager_.get_table(table_name);
  if (!table_meta) {
    return ExecutionResult::error("INSERT: table '" + table_name + "' does not exist");
  }
  
  const Schema& schema = table_meta->schema;
  const auto& schema_columns = schema.get_columns();
  
  // Build all tuples first
  std::vector<Tuple> tuples;
  tuples.reserve(node->values.size());

  for (const auto& row_values : node->values) {
    if (row_values.size() != schema_columns.size()) {
      return ExecutionResult::error("INSERT: value count (" + std::to_string(row_values.size()) +
                                    ") doesn't match column count (" + std::to_string(schema_columns.size()) + ")");
    }

    Tuple tuple;
    for (size_t i = 0; i < row_values.size(); ++i) {
      Value val = literal_to_value(row_values[i].get());
      tuple.add_value(val);
    }
    tuples.push_back(std::move(tuple));
  }

  // Batch insert all tuples
  uint32_t inserted_count = table_manager_.insert_rows(table_name, tuples);
  if (inserted_count != tuples.size()) {
    return ExecutionResult::error("INSERT: failed to insert all rows into '" + table_name +
                                  "' (" + std::to_string(inserted_count) + "/" + std::to_string(tuples.size()) + ")");
  }

  return ExecutionResult::ok(inserted_count);
}

ExecutionResult Executor::execute_select(SelectStatementNode* node) {
  if (!node->from_clause || !node->from_clause->name) {
    return ExecutionResult::error("SELECT: missing FROM clause");
  }
  
  std::string table_name = node->from_clause->name->name;
  
  // Get table metadata
  auto* table_meta = catalog_manager_.get_table(table_name);
  if (!table_meta) {
    return ExecutionResult::error("SELECT: table '" + table_name + "' does not exist");
  }
  
  ExecutionResult result;
  result.success = true;
  
  // Get column names for result
  const auto& schema_columns = table_meta->schema.get_columns();
  if (node->is_select_all) {
    for (const auto& col : schema_columns) {
      result.column_names.push_back(col.get_name());
    }
  } else {
    for (const auto& sel_col : node->columns) {
      if (auto* ident = dynamic_cast<IdentifierNode*>(sel_col.expression.get())) {
        result.column_names.push_back(ident->name);
      } else {
        result.column_names.push_back("?");  // For expressions
      }
    }
  }
  
  // Scan the table
  bool scan_ok = table_manager_.scan_table_tuples(table_name, [&result](const Tuple& tuple) {
    result.rows.push_back(tuple);
  });
  
  if (!scan_ok) {
    return ExecutionResult::error("SELECT: failed to scan table '" + table_name + "'");
  }
  
  result.affected_rows = result.rows.size();
  return result;
}

DataType Executor::token_type_to_data_type(TokenType type) {
  switch (type) {
    case TokenType::INT:
      return DataType::INTEGER;
    case TokenType::FLOAT:
      return DataType::DOUBLE;
    case TokenType::VARCHAR:
      return DataType::VARCHAR;
    case TokenType::BOOL:
      return DataType::BOOLEAN;
    case TokenType::DATE:
      return DataType::DATE;
    case TokenType::TIMESTAMP:
      return DataType::TIMESTAMP;
    default:
      // Default to VARCHAR for unknown types
      return DataType::VARCHAR;
  }
}

Value Executor::literal_to_value(const LiteralNode* literal) {
  if (!literal) {
    return std::monostate{};  // NULL
  }
  
  return std::visit([](auto&& arg) -> Value {
    using T = std::decay_t<decltype(arg)>;
    if constexpr (std::is_same_v<T, int64_t>) {
      return static_cast<int32_t>(arg);  // Convert to int32_t for storage
    } else if constexpr (std::is_same_v<T, double>) {
      return arg;
    } else if constexpr (std::is_same_v<T, std::string>) {
      return arg;
    } else if constexpr (std::is_same_v<T, bool>) {
      return arg;
    } else if constexpr (std::is_same_v<T, SQLDate>) {
      // Convert date to string format
      char buf[16];
      snprintf(buf, sizeof(buf), "%04d-%02d-%02d", arg.year, arg.month, arg.day);
      return std::string(buf);
    } else if constexpr (std::is_same_v<T, SQLTimestamp>) {
      // Convert timestamp to string format
      char buf[32];
      snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", 
               arg.year, arg.month, arg.day, arg.hour, arg.minute, arg.second);
      return std::string(buf);
    } else {
      return std::monostate{};
    }
  }, literal->value);
}

} // namespace letty
