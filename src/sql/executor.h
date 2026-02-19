//
// Created by Antigravity on 1/27/26.
//

#pragma once

#include <string>
#include <vector>
#include <memory>
#include "ast.h"
#include "token.h"
#include "storage/tuple.h"
#include "storage/table_manager.h"
#include "catalog/catalog_manager.h"

namespace letty {

/**
 * @struct ExecutionResult
 * @brief Result of executing a SQL statement.
 * 
 * Contains success status, optional error message, result rows for SELECT,
 * and affected row count for INSERT/UPDATE/DELETE.
 */
struct ExecutionResult {
  bool success = false;
  std::string error_message;
  
  // For SELECT queries
  std::vector<Tuple> rows;
  std::vector<std::string> column_names;  // Column headers
  
  // For INSERT/UPDATE/DELETE
  uint32_t affected_rows = 0;
  
  // Factory methods for common results
  static ExecutionResult ok() {
    ExecutionResult r;
    r.success = true;
    return r;
  }
  
  static ExecutionResult ok(uint32_t affected) {
    ExecutionResult r;
    r.success = true;
    r.affected_rows = affected;
    return r;
  }
  
  static ExecutionResult error(const std::string& msg) {
    ExecutionResult r;
    r.success = false;
    r.error_message = msg;
    return r;
  }
};

/**
 * @class Executor
 * @brief Executes parsed SQL statements against the storage layer.
 * 
 * The Executor takes AST nodes produced by the Parser and executes them
 * using CatalogManager and TableManager. It handles:
 * - CREATE TABLE: Creates table schema in catalog
 * - INSERT: Parses values and inserts rows
 * - SELECT: Scans tables and returns results
 * 
 * Usage:
 * @code
 *   Parser parser("SELECT * FROM users");
 *   auto ast = parser.parse();
 *   Executor executor(catalog, table_manager);
 *   ExecutionResult result = executor.execute(ast.get());
 * @endcode
 */
class Executor {
 public:
  Executor(CatalogManager& catalog_manager, TableManager& table_manager);

  /**
   * @brief Executes any AST node by dispatching to the appropriate handler.
   * @param node The parsed AST node.
   * @return The execution result.
   */
  ExecutionResult execute(ASTNode* node);

 private:
  CatalogManager& catalog_manager_;
  TableManager& table_manager_;

  /**
   * @brief Executes a CREATE TABLE statement.
   */
  ExecutionResult execute_create_table(CreateTableStatementNode* node);

  /**
   * @brief Executes an INSERT statement.
   */
  ExecutionResult execute_insert(InsertStatementNode* node);

  /**
   * @brief Executes a SELECT statement (basic, no JOINs).
   */
  ExecutionResult execute_select(SelectStatementNode* node);

  /**
   * @brief Converts a TokenType (from parser) to a DataType (for storage).
   */
  DataType token_type_to_data_type(TokenType type);

  /**
   * @brief Converts an AST LiteralNode value to a storage Value.
   */
  Value literal_to_value(const LiteralNode* literal);
};

} // namespace letty
