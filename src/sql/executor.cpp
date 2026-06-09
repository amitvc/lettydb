#include "executor.h"

#include "token.h"
#include "common/db_exception.h"

namespace letty {

namespace {

bool is_numeric_value(const Value& value) {
  return std::holds_alternative<int32_t>(value) || std::holds_alternative<double>(value);
}

double numeric_value_as_double(const Value& value) {
  if (std::holds_alternative<int32_t>(value)) {
    return static_cast<double>(std::get<int32_t>(value));
  }

  return std::get<double>(value);
}

int compare_values(const Value& lhs, const Value& rhs) {
  if (is_numeric_value(lhs) && is_numeric_value(rhs)) {
    double left = numeric_value_as_double(lhs);
    double right = numeric_value_as_double(rhs);
    if (left < right) {
      return -1;
    }
    if (left > right) {
      return 1;
    }
    return 0;
  }

  if (std::holds_alternative<std::string>(lhs) && std::holds_alternative<std::string>(rhs)) {
    const auto& left = std::get<std::string>(lhs);
    const auto& right = std::get<std::string>(rhs);
    if (left < right) {
      return -1;
    }
    if (left > right) {
      return 1;
    }
    return 0;
  }

  if (std::holds_alternative<bool>(lhs) && std::holds_alternative<bool>(rhs)) {
    bool left = std::get<bool>(lhs);
    bool right = std::get<bool>(rhs);
    if (left == right) {
      return 0;
    }
    return left ? 1 : -1;
  }

  throw DbException(DbErrorCode::TypeMismatch, "SELECT: WHERE clause compares incompatible value types");
}

bool apply_comparison(const Value& lhs, const std::string& op, const Value& rhs) {
  if (std::holds_alternative<std::monostate>(lhs) || std::holds_alternative<std::monostate>(rhs)) {
    return false;
  }

  int cmp = compare_values(lhs, rhs);

  if (op == "=") {
    return cmp == 0;
  }
  if (op == "!=") {
    return cmp != 0;
  }
  if (op == "<") {
    return cmp < 0;
  }
  if (op == "<=") {
    return cmp <= 0;
  }
  if (op == ">") {
    return cmp > 0;
  }
  if (op == ">=") {
    return cmp >= 0;
  }

  throw DbException(DbErrorCode::InvalidArgument, "SELECT: unsupported WHERE operator '" + op + "'");
}

}  // namespace

Executor::Executor(CatalogManager& catalog_manager, TableManager& table_manager)
    : catalog_manager_(catalog_manager), table_manager_(table_manager) {}

ExecutionResult Executor::execute(ASTNode* node) {
  try {
    if (!node) {
      return ExecutionResult::error("Null AST node");
    }

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
  } catch (const DbException& e) {
    return ExecutionResult::error(e.what());
  }
}

ExecutionResult Executor::execute_create_table(CreateTableStatementNode* node) {
  if (!node->table_name) {
    return ExecutionResult::error("CREATE TABLE: missing table name");
  }
  
  std::string table_name = node->table_name->name;
  
  std::vector<Column> columns;
  uint16_t current_offset = 0;
  
  for (const auto& col_def : node->columns) {
    if (!col_def || !col_def->column_name) {
      return ExecutionResult::error("CREATE TABLE: invalid column definition");
    }
    
    std::string col_name = col_def->column_name->name;
    DataType data_type = token_type_to_data_type(col_def->data_type);
    
    uint16_t col_length;
    if (data_type == DataType::VARCHAR) {
      col_length = col_def->size > 0 ? col_def->size : 255;  // Default VARCHAR size
    } else {
      col_length = Column::fixed_length_of(data_type);
    }
    
    columns.emplace_back(col_name, data_type, col_length, current_offset, col_def->nullable);
    current_offset += col_length;
  }
  
  Schema schema(columns);
  
  catalog_manager_.create_table(table_name, schema);
  return ExecutionResult::ok();
}

std::optional<std::unordered_map<size_t, size_t>> Executor::build_column_index(
    const std::vector<std::unique_ptr<IdentifierNode>>& column_names,
    const std::vector<Column>& schema_columns,
    std::string& out_error) {
  // schema_column_pos -> value_column_pos
  std::unordered_map<size_t, size_t> idx;

  if (column_names.empty()) {
    for (size_t i = 0; i < schema_columns.size(); ++i) {
      idx[i] = i;
    }
    return idx;
  }

  if (column_names.size() > schema_columns.size()) {
    out_error = "INSERT: more columns specified than exist in table schema";
    return std::nullopt;
  }

  for (size_t vi = 0; vi < column_names.size(); ++vi) {
    const std::string& col_name = column_names[vi]->name;
    bool found = false;
    for (size_t si = 0; si < schema_columns.size(); ++si) {
      if (schema_columns[si].get_name() == col_name) {
        idx[si] = vi;
        found = true;
        break;
      }
    }
    if (!found) {
      out_error = "INSERT: unknown column '" + col_name + "'";
      return std::nullopt;
    }
  }

  if (idx.size() != column_names.size()) {
    out_error = "INSERT: duplicate column names in column list";
    return std::nullopt;
  }

  for (size_t si = 0; si < schema_columns.size(); ++si) {
    if (idx.find(si) == idx.end() && !schema_columns[si].is_nullable()) {
      out_error = "INSERT: column '" + schema_columns[si].get_name() + "' is NOT NULL but has no value";
      return std::nullopt;
    }
  }

  return idx;
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

  std::string index_error;
  auto schema_to_value_idx = build_column_index(node->columnNames, schema_columns, index_error);
  if (!schema_to_value_idx) {
    return ExecutionResult::error(index_error);
  }

  size_t expected_value_count = node->columnNames.empty() ? schema_columns.size() : node->columnNames.size();

  for (const auto& row_values : node->values) {
    if (row_values.size() != expected_value_count) {
      return ExecutionResult::error("INSERT: value count (" + std::to_string(row_values.size()) +
                                    ") doesn't match column count (" + std::to_string(expected_value_count) + ")");
    }

    Tuple tuple;
    for (size_t si = 0; si < schema_columns.size(); ++si) {
      auto it = schema_to_value_idx->find(si);
      Value val = (it == schema_to_value_idx->end()) ? Value{std::monostate{}} : literal_to_value(row_values[it->second].get());
      if (std::holds_alternative<std::monostate>(val) && !schema_columns[si].is_nullable()) {
        return ExecutionResult::error("INSERT: column '" + schema_columns[si].get_name() + "' is NOT NULL");
      }
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
  
  auto* table_meta = catalog_manager_.get_table(table_name);
  if (!table_meta) {
    return ExecutionResult::error("SELECT: table '" + table_name + "' does not exist");
  }
  
  ExecutionResult result;
  result.success = true;
  
  const auto& schema_columns = table_meta->schema.get_columns();
  std::vector<size_t> selected_column_indexes;
  if (node->is_select_all) {
    for (size_t i = 0; i < schema_columns.size(); ++i) {
      const auto& col = schema_columns[i];
      result.column_names.push_back(col.get_name());
      selected_column_indexes.push_back(i);
    }
  } else {
    for (const auto& sel_col : node->columns) {
      std::string column_name;
      if (auto* ident = dynamic_cast<IdentifierNode*>(sel_col.expression.get())) {
        column_name = ident->name;
      } else if (auto* qualified = dynamic_cast<QualifiedIdentifierNode*>(sel_col.expression.get())) {
        column_name = qualified->name->name;
      } else {
        return ExecutionResult::error("SELECT: unsupported select expression");
      }

      bool found = false;
      for (size_t i = 0; i < schema_columns.size(); ++i) {
        if (schema_columns[i].get_name() == column_name) {
          result.column_names.push_back(sel_col.alias.empty() ? column_name : sel_col.alias);
          selected_column_indexes.push_back(i);
          found = true;
          break;
        }
      }

      if (!found) {
        return ExecutionResult::error("SELECT: unknown column '" + column_name + "'");
      }
    }
  }
  

  TableScanner scanner = table_manager_.scan_table(table_name);
  while (scanner.next()) {
    Tuple tuple = Tuple::deserialize(table_meta->schema, scanner.get_tuple_data(), scanner.get_tuple_size());
    if (!tuple_matches_where(node, tuple, schema_columns)) {
      continue;
    }

    Tuple projected_tuple = project_tuple(tuple, selected_column_indexes);
    result.rows.push_back(std::move(projected_tuple));
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

std::optional<size_t> Executor::find_column_index(const std::vector<Column>& schema_columns,
										const std::string& column_name) {
  for (size_t i = 0; i < schema_columns.size(); ++i) {
	if (schema_columns[i].get_name() == column_name) {
	  return i;
	}
  }
  return std::nullopt;
}

Value Executor::evaluate_expression(const ExpressionNode* expression,
                                    const Tuple& tuple,
                                    const std::vector<Column>& schema_columns) {
  if (auto* literal = dynamic_cast<const LiteralNode*>(expression)) {
    return literal_to_value(literal);
  }

  if (auto* identifier = dynamic_cast<const IdentifierNode*>(expression)) {
    auto column_index = find_column_index(schema_columns, identifier->name);
    if (!column_index) {
      throw DbException(DbErrorCode::NotFound, "SELECT: unknown column '" + identifier->name + "'");
    }
    return tuple.get_value(*column_index);
  }

  if (auto* qualified = dynamic_cast<const QualifiedIdentifierNode*>(expression)) {
    auto column_index = find_column_index(schema_columns, qualified->name->name);
    if (!column_index) {
      throw DbException(DbErrorCode::NotFound, "SELECT: unknown column '" + qualified->name->name + "'");
    }
    return tuple.get_value(*column_index);
  }

  throw DbException(DbErrorCode::InvalidArgument, "SELECT: unsupported expression in WHERE clause");
}

bool Executor::evaluate_where_clause(const ExpressionNode* expression,
                                     const Tuple& tuple,
                                     const std::vector<Column>& schema_columns) {
  auto* binary = dynamic_cast<const BinaryOperationNode*>(expression);
  if (!binary) {
    Value value = evaluate_expression(expression, tuple, schema_columns);
    if (std::holds_alternative<bool>(value)) {
      return std::get<bool>(value);
    }
    throw DbException(DbErrorCode::TypeMismatch, "SELECT: WHERE clause expression must evaluate to BOOLEAN");
  }

  if (binary->op == "AND") {
    return evaluate_where_clause(binary->left.get(), tuple, schema_columns) &&
           evaluate_where_clause(binary->right.get(), tuple, schema_columns);
  }

  if (binary->op == "OR") {
    return evaluate_where_clause(binary->left.get(), tuple, schema_columns) ||
           evaluate_where_clause(binary->right.get(), tuple, schema_columns);
  }

  Value left = evaluate_expression(binary->left.get(), tuple, schema_columns);
  Value right = evaluate_expression(binary->right.get(), tuple, schema_columns);
  return apply_comparison(left, binary->op, right);
}

bool Executor::tuple_matches_where(const SelectStatementNode* node,
                                   const Tuple& tuple,
                                   const std::vector<Column>& schema_columns) {
  if (!node->where_clause) {
    return true;
  }

  return evaluate_where_clause(node->where_clause.get(), tuple, schema_columns);
}

Tuple Executor::project_tuple(const Tuple& tuple,
                              const std::vector<size_t>& selected_column_indexes) {
  Tuple projected_tuple;
  for (size_t index : selected_column_indexes) {
    projected_tuple.add_value(tuple.get_value(index));
  }
  return projected_tuple;
}

} // namespace letty
