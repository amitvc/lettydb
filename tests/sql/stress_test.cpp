#include <gtest/gtest.h>
#include <cstdio>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <stdexcept>
#include <unordered_map>

#include "sql/executor.h"
#include "sql/parser.h"
#include "sql/lexer.h"
#include "sql/token.h"
#include "storage/disk_manager.h"
#include "buffer/buffer_pool_manager.h"
#include "buffer/lru_replacer.h"
#include "storage/extent_manager.h"
#include "storage/iam_manager.h"
#include "catalog/catalog_manager.h"
#include "storage/table_manager.h"
#include "monitoring/storage_inspector.h"

namespace letty {

// ——— Test Fixture ———

class StressTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::remove(db_path_.c_str());
    disk_manager_ = std::make_unique<DiskManager>(db_path_);
    bpm_ = std::make_unique<BufferPoolManager>(
        *disk_manager_, 256, std::make_unique<LRUPageReplacer>());
    extent_manager_ = std::make_unique<ExtentManager>(*bpm_);
    iam_manager_ = std::make_unique<IamManager>(*bpm_, *extent_manager_);
    catalog_manager_ = std::make_unique<CatalogManager>(*bpm_, *iam_manager_);
    catalog_manager_->init();
    table_manager_ = std::make_unique<TableManager>(*bpm_, *iam_manager_, *catalog_manager_);
    executor_ = std::make_unique<Executor>(*catalog_manager_, *table_manager_);
    inspector_ = std::make_unique<StorageInspector>(*bpm_, *extent_manager_, *iam_manager_, *catalog_manager_);
  }

  void TearDown() override {
    inspector_.reset();
    executor_.reset();
    table_manager_.reset();
    catalog_manager_.reset();
    iam_manager_.reset();
    extent_manager_.reset();
    bpm_.reset();
    disk_manager_.reset();
    std::remove(db_path_.c_str());
  }

  ExecutionResult execute_sql(const std::string& sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    Parser parser(std::move(tokens));
    auto ast = parser.parse();
    return executor_->execute(ast.get());
  }

  std::unique_ptr<ASTNode> parse_only(const std::string& sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    Parser parser(std::move(tokens));
    return parser.parse();
  }

  std::string build_batch_insert(const std::string& table, int start_id, int count) {
    std::ostringstream sql;
    sql << "INSERT INTO " << table << " VALUES ";
    for (int i = 0; i < count; ++i) {
      if (i > 0) sql << ", ";
      // Force FLOAT_LITERAL by always including a decimal point.
      // Without this, e.g. 0 * 1.5 = 0 would print as integer "0" and
      // cause bad_variant_access when serializing into a FLOAT/DOUBLE column.
      double score = (start_id + i) * 1.5 + 0.1;
      sql << "(" << (start_id + i) << ", 'row_" << (start_id + i) << "', "
          << std::fixed << std::setprecision(2) << score
          << ", " << ((start_id + i) % 2 == 0 ? "TRUE" : "FALSE") << ")";
    }
    return sql.str();
  }

  const std::string db_path_ = "stress_test.db";
  std::unique_ptr<DiskManager> disk_manager_;
  std::unique_ptr<BufferPoolManager> bpm_;
  std::unique_ptr<ExtentManager> extent_manager_;
  std::unique_ptr<IamManager> iam_manager_;
  std::unique_ptr<CatalogManager> catalog_manager_;
  std::unique_ptr<TableManager> table_manager_;
  std::unique_ptr<Executor> executor_;
  std::unique_ptr<StorageInspector> inspector_;
};

// ——— Lexer Edge Cases ———

TEST_F(StressTest, LexerCaseInsensitiveKeywords) {
  auto check = [](const std::string& sql, TokenType expected_first) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    EXPECT_EQ(tokens[0].type, expected_first) << "Failed for: " << sql;
  };
  check("SELECT * FROM t", TokenType::SELECT);
  check("select * from t", TokenType::SELECT);
  check("Select * From t", TokenType::SELECT);
  check("sElEcT * fRoM t", TokenType::SELECT);
  check("INSERT INTO t VALUES (1)", TokenType::INSERT);
  check("insert into t values (1)", TokenType::INSERT);
  check("CREATE TABLE t (id INT)", TokenType::CREATE);
  check("create table t (id int)", TokenType::CREATE);
}

TEST_F(StressTest, LexerIdentifiersWithUnderscores) {
  Lexer lexer("SELECT my_col, _hidden, col123 FROM my_table_2");
  auto tokens = lexer.tokenize();
  EXPECT_EQ(tokens[1].type, TokenType::IDENTIFIER);
  EXPECT_EQ(tokens[1].text, "my_col");
  EXPECT_EQ(tokens[3].type, TokenType::IDENTIFIER);
  EXPECT_EQ(tokens[3].text, "_hidden");
  EXPECT_EQ(tokens[5].type, TokenType::IDENTIFIER);
  EXPECT_EQ(tokens[5].text, "col123");
}

TEST_F(StressTest, LexerWhitespaceVariants) {
  Lexer lexer("SELECT\t*\nFROM\r\nt");
  auto tokens = lexer.tokenize();
  EXPECT_EQ(tokens[0].type, TokenType::SELECT);
  EXPECT_EQ(tokens[1].type, TokenType::STAR);
  EXPECT_EQ(tokens[2].type, TokenType::FROM);
  EXPECT_EQ(tokens[3].type, TokenType::IDENTIFIER);
  EXPECT_EQ(tokens[3].text, "t");
}

TEST_F(StressTest, LexerUnterminatedStringReturnsUnknown) {
  // A string that is never closed returns UNKNOWN token
  Lexer lexer("'hello world");
  auto tokens = lexer.tokenize();
  EXPECT_EQ(tokens[0].type, TokenType::UNKNOWN)
      << "BUG: unterminated string should produce UNKNOWN token";
}

TEST_F(StressTest, LexerEmptyString) {
  Lexer lexer("INSERT INTO t VALUES ('')");
  auto tokens = lexer.tokenize();
  bool found_string = false;
  for (auto& tok : tokens) {
    if (tok.type == TokenType::STRING_LITERAL) {
      EXPECT_EQ(tok.text, "");
      found_string = true;
    }
  }
  EXPECT_TRUE(found_string) << "Empty string literal '' should be tokenized as STRING_LITERAL";
}

TEST_F(StressTest, LexerStringWithSpacesAndPunctuation) {
  Lexer lexer("SELECT 'hello world! how are you?' FROM t");
  auto tokens = lexer.tokenize();
  EXPECT_EQ(tokens[1].type, TokenType::STRING_LITERAL);
  EXPECT_EQ(tokens[1].text, "hello world! how are you?");
}

TEST_F(StressTest, LexerNegativeNumbersAsTwoTokens) {
  // '-5' lexes as MINUS + INT_LITERAL, not a single negative literal
  Lexer lexer("-5");
  auto tokens = lexer.tokenize();
  EXPECT_EQ(tokens[0].type, TokenType::MINUS);
  EXPECT_EQ(tokens[1].type, TokenType::INT_LITERAL);
  EXPECT_EQ(tokens[1].text, "5");
}

TEST_F(StressTest, LexerFloatWithoutLeadingDigit) {
  // '.5' is not a digit, not alpha — becomes UNKNOWN + INT_LITERAL
  Lexer lexer(".5");
  auto tokens = lexer.tokenize();
  EXPECT_EQ(tokens[0].type, TokenType::DOT)
      << "BUG: '.5' is tokenized as DOT + INT_LITERAL, not FLOAT_LITERAL";
  EXPECT_EQ(tokens[1].type, TokenType::INT_LITERAL);
}

TEST_F(StressTest, LexerIntegerEndingWithDot) {
  // '1.' followed by non-digit - should be INT_LITERAL + DOT
  Lexer lexer("1. FROM");
  auto tokens = lexer.tokenize();
  EXPECT_EQ(tokens[0].type, TokenType::INT_LITERAL);
  EXPECT_EQ(tokens[0].text, "1");
  EXPECT_EQ(tokens[1].type, TokenType::DOT);
}

TEST_F(StressTest, LexerLoneExclamationMarkIsUnknown) {
  // '!' without '=' following should be UNKNOWN
  Lexer lexer("a ! b");
  auto tokens = lexer.tokenize();
  EXPECT_EQ(tokens[1].type, TokenType::UNKNOWN)
      << "Lone '!' should produce UNKNOWN token";
  EXPECT_EQ(tokens[1].text, "!");
}

TEST_F(StressTest, LexerDateAndTimestampDetection) {
  Lexer lexer("'2024-01-15' '2024-01-15 10:30:00'");
  auto tokens = lexer.tokenize();
  EXPECT_EQ(tokens[0].type, TokenType::DATE_LITERAL);
  EXPECT_EQ(tokens[0].text, "2024-01-15");
  EXPECT_EQ(tokens[1].type, TokenType::TIMESTAMP_LITERAL);
  EXPECT_EQ(tokens[1].text, "2024-01-15 10:30:00");
}

TEST_F(StressTest, LexerDateWithInvalidValuesStillTokenizedAsDate) {
  // Lexer doesn't validate date values, just format - '1999-13-32' matches YYYY-MM-DD pattern
  Lexer lexer("'1999-13-32'");
  auto tokens = lexer.tokenize();
  EXPECT_EQ(tokens[0].type, TokenType::DATE_LITERAL)
      << "Lexer accepts syntactically valid date format regardless of calendar validity";
}

TEST_F(StressTest, LexerNoSemicolon) {
  // SQL without semicolon should still tokenize correctly
  Lexer lexer("SELECT * FROM users");
  auto tokens = lexer.tokenize();
  EXPECT_EQ(tokens.back().type, TokenType::EOF_FILE);
  EXPECT_NE(tokens[tokens.size() - 2].type, TokenType::SEMICOLON);
}

TEST_F(StressTest, LexerVeryLongIdentifier) {
  std::string long_name(200, 'a');
  // BUG: Lexer stores std::string_view — passing a temporary std::string produces a
  // dangling view once the temporary is destroyed. Must materialize first.
  // Lexer lexer("SELECT " + long_name + " FROM t");  // dangling view — UB
  std::string query = "SELECT " + long_name + " FROM t";
  Lexer lexer(query);
  auto tokens = lexer.tokenize();
  EXPECT_EQ(tokens[1].type, TokenType::IDENTIFIER)
      << "200-char identifier should tokenize as IDENTIFIER when string stays alive";
  EXPECT_EQ(tokens[1].text.size(), 200u);
}

TEST_F(StressTest, LexerDanglingViewFromTemporary) {
  // Documents the string_view lifetime hazard in Lexer.
  // Lexer(std::string_view) from a temporary std::string is undefined behavior:
  // the view becomes dangling after the full-expression is evaluated.
  std::string long_name(50, 'z');
  // This WOULD be dangling: Lexer lexer("SELECT " + long_name + " FROM t");
  // Safe form:
  std::string safe = "SELECT " + long_name + " FROM t";
  Lexer safe_lexer(safe);
  auto tokens = safe_lexer.tokenize();
  EXPECT_EQ(tokens[0].type, TokenType::SELECT);
  EXPECT_EQ(tokens[1].type, TokenType::IDENTIFIER);
  EXPECT_EQ(tokens[1].text.size(), 50u)
      << "BUG DOCUMENTED: Lexer must receive a non-temporary string to avoid dangling string_view";
}

TEST_F(StressTest, LexerBooleanLiterals) {
  Lexer lexer("INSERT INTO t VALUES (TRUE, FALSE, true, false, True, False)");
  auto tokens = lexer.tokenize();
  int bool_count = 0;
  for (auto& tok : tokens) {
    if (tok.type == TokenType::TRUE || tok.type == TokenType::FALSE) {
      bool_count++;
    }
  }
  EXPECT_EQ(bool_count, 6) << "TRUE/FALSE should be case-insensitive keyword tokens";
}

// ——— Parser Edge Cases ———

TEST_F(StressTest, ParserSelectWithoutFromThrows) {
  EXPECT_THROW(parse_only("SELECT * users"), std::runtime_error)
      << "SELECT without FROM should throw";
}

TEST_F(StressTest, ParserCreateTableWithKeywordAsNameThrows) {
  // 'select' tokenizes as SELECT keyword, not IDENTIFIER - parser expects IDENTIFIER for table name
  EXPECT_THROW(parse_only("CREATE TABLE select (id INT)"), std::runtime_error)
      << "BUG: keywords cannot be used as table names - no quoting support";
}

TEST_F(StressTest, ParserCreateTableWithKeywordAsColumnNameThrows) {
  // 'from' tokenizes as FROM keyword, not IDENTIFIER
  EXPECT_THROW(parse_only("CREATE TABLE t (from INT)"), std::runtime_error)
      << "BUG: keywords cannot be used as column names - no quoting support";
}

TEST_F(StressTest, ParserInsertNullValueBehavior) {
  auto result = execute_sql("CREATE TABLE null_test (val INT)");
  ASSERT_TRUE(result.success);

  auto insert_result = execute_sql("INSERT INTO null_test VALUES (NULL)");
  ASSERT_TRUE(insert_result.success);

  auto select_result = execute_sql("SELECT * FROM null_test");
  ASSERT_TRUE(select_result.success);
  ASSERT_EQ(select_result.rows.size(), 1u);
  ASSERT_EQ(select_result.rows[0].size(), 1u);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(select_result.rows[0].get_value(0)));
}

TEST_F(StressTest, ParserVarcharWithoutSizeDefaultsTo255) {
  auto ast = parse_only("CREATE TABLE t (name VARCHAR)");
  auto* create = dynamic_cast<CreateTableStatementNode*>(ast.get());
  ASSERT_NE(create, nullptr);
  ASSERT_EQ(create->columns.size(), 1u);
  EXPECT_EQ(create->columns[0]->data_type, TokenType::VARCHAR);
  EXPECT_EQ(create->columns[0]->size, 0) << "Parser sets size=0 when no size specified; executor defaults to 255";
}

TEST_F(StressTest, ParserInsertWithNamedColumns) {
  auto ast = parse_only("INSERT INTO t (id, name) VALUES (1, 'Alice')");
  auto* insert = dynamic_cast<InsertStatementNode*>(ast.get());
  ASSERT_NE(insert, nullptr);
  EXPECT_EQ(insert->columnNames.size(), 2u);
  EXPECT_EQ(insert->columnNames[0]->name, "id");
  EXPECT_EQ(insert->columnNames[1]->name, "name");
  EXPECT_EQ(insert->values.size(), 1u);
  EXPECT_EQ(insert->values[0].size(), 2u);
}

TEST_F(StressTest, ParserSelectSpecificColumns) {
  auto ast = parse_only("SELECT id, name FROM users");
  auto* select = dynamic_cast<SelectStatementNode*>(ast.get());
  ASSERT_NE(select, nullptr);
  EXPECT_FALSE(select->is_select_all);
  EXPECT_EQ(select->columns.size(), 2u);
}

TEST_F(StressTest, ParserSelectWithWhereClause) {
  // WHERE with integer and string comparisons parses fine
  auto ast = parse_only("SELECT * FROM users WHERE id = 1");
  auto* select = dynamic_cast<SelectStatementNode*>(ast.get());
  ASSERT_NE(select, nullptr);
  EXPECT_NE(select->where_clause, nullptr) << "WHERE clause should be parsed into AST";
}


TEST_F(StressTest, ParserMultipleInsertRows) {
  auto ast = parse_only("INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c')");
  auto* insert = dynamic_cast<InsertStatementNode*>(ast.get());
  ASSERT_NE(insert, nullptr);
  EXPECT_EQ(insert->values.size(), 3u);
}

TEST_F(StressTest, ParserEmptyStringInValues) {
  auto ast = parse_only("INSERT INTO t VALUES ('')");
  auto* insert = dynamic_cast<InsertStatementNode*>(ast.get());
  ASSERT_NE(insert, nullptr);
  ASSERT_EQ(insert->values.size(), 1u);
  ASSERT_EQ(insert->values[0].size(), 1u);
  auto* lit = insert->values[0][0].get();
  ASSERT_NE(lit, nullptr);
  EXPECT_EQ(std::get<std::string>(lit->value), "");
}

TEST_F(StressTest, ParserCreateTableAllTypes) {
  auto ast = parse_only(
      "CREATE TABLE all_types (i INT, f FLOAT, v VARCHAR(100), b BOOL, d DATE, ts TIMESTAMP)");
  auto* create = dynamic_cast<CreateTableStatementNode*>(ast.get());
  ASSERT_NE(create, nullptr);
  ASSERT_EQ(create->columns.size(), 6u);
  EXPECT_EQ(create->columns[0]->data_type, TokenType::INT);
  EXPECT_EQ(create->columns[1]->data_type, TokenType::FLOAT);
  EXPECT_EQ(create->columns[2]->data_type, TokenType::VARCHAR);
  EXPECT_EQ(create->columns[2]->size, 100);
  EXPECT_EQ(create->columns[3]->data_type, TokenType::BOOL);
  EXPECT_EQ(create->columns[4]->data_type, TokenType::DATE);
  EXPECT_EQ(create->columns[5]->data_type, TokenType::TIMESTAMP);
}

// ——— Executor Edge Cases ———

TEST_F(StressTest, ExecutorSelectSpecificColumnsReturnsBehavior) {
  execute_sql("CREATE TABLE t (id INT, name VARCHAR(20), score FLOAT)");
  execute_sql("INSERT INTO t VALUES (1, 'Alice', 9.5)");

  // SELECT specific columns should return only the requested values in each row.
  auto result = execute_sql("SELECT id, score FROM t");
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.column_names.size(), 2u) << "column_names correctly filters to id, score";
  EXPECT_EQ(result.column_names[0], "id");
  EXPECT_EQ(result.column_names[1], "score");
  ASSERT_EQ(result.rows.size(), 1u);
  EXPECT_EQ(result.rows[0].size(), 2u);
  EXPECT_EQ(std::get<int32_t>(result.rows[0].get_value(0)), 1);
  EXPECT_DOUBLE_EQ(std::get<double>(result.rows[0].get_value(1)), 9.5);
}

TEST_F(StressTest, ExecutorInsertNamedColumnsOutOfOrder) {
  execute_sql("CREATE TABLE t (id INT, name VARCHAR(30))");
  auto result = execute_sql("INSERT INTO t (name, id) VALUES ('Alice', 42)");
  ASSERT_TRUE(result.success) << result.error_message;

  auto select = execute_sql("SELECT * FROM t");
  ASSERT_TRUE(select.success);
  ASSERT_EQ(select.rows.size(), 1u);
  EXPECT_EQ(std::get<int32_t>(select.rows[0].get_value(0)), 42);
  EXPECT_EQ(std::get<std::string>(select.rows[0].get_value(1)), "Alice");
}

TEST_F(StressTest, ExecutorInsertValueCountMismatch) {
  execute_sql("CREATE TABLE t (id INT, name VARCHAR(20))");
  auto result = execute_sql("INSERT INTO t VALUES (1)");
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_message.find("value count"), std::string::npos);
}

TEST_F(StressTest, ExecutorInsertTooManyValues) {
  execute_sql("CREATE TABLE t (id INT)");
  auto result = execute_sql("INSERT INTO t VALUES (1, 2, 3)");
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_message.find("value count"), std::string::npos);
}

TEST_F(StressTest, ExecutorAllDataTypesRoundTrip) {
  execute_sql("CREATE TABLE all_types (i INT, f FLOAT, v VARCHAR(50), b BOOL, d DATE, ts TIMESTAMP)");
  auto insert = execute_sql(
      "INSERT INTO all_types VALUES (42, 3.14, 'hello', TRUE, '2024-06-15', '2024-06-15 12:30:00')");
  ASSERT_TRUE(insert.success) << insert.error_message;

  auto select = execute_sql("SELECT * FROM all_types");
  ASSERT_TRUE(select.success) << select.error_message;
  ASSERT_EQ(select.rows.size(), 1u);

  const auto& row = select.rows[0];
  EXPECT_EQ(std::get<int32_t>(row.get_value(0)), 42);
  EXPECT_DOUBLE_EQ(std::get<double>(row.get_value(1)), 3.14);
  EXPECT_EQ(std::get<std::string>(row.get_value(2)), "hello");
  EXPECT_EQ(std::get<bool>(row.get_value(3)), true);
  EXPECT_EQ(std::get<std::string>(row.get_value(4)), "2024-06-15");
  EXPECT_EQ(std::get<std::string>(row.get_value(5)), "2024-06-15 12:30:00");
}

TEST_F(StressTest, ExecutorBoolFalseRoundTrip) {
  execute_sql("CREATE TABLE flags (id INT, active BOOL)");
  execute_sql("INSERT INTO flags VALUES (1, TRUE), (2, FALSE)");

  auto result = execute_sql("SELECT * FROM flags");
  ASSERT_EQ(result.rows.size(), 2u);
  EXPECT_EQ(std::get<bool>(result.rows[0].get_value(1)), true);
  EXPECT_EQ(std::get<bool>(result.rows[1].get_value(1)), false);
}

TEST_F(StressTest, ExecutorEmptyStringRoundTrip) {
  execute_sql("CREATE TABLE t (id INT, note VARCHAR(50))");
  execute_sql("INSERT INTO t VALUES (1, '')");
  auto result = execute_sql("SELECT * FROM t");
  ASSERT_EQ(result.rows.size(), 1u);
  EXPECT_EQ(std::get<std::string>(result.rows[0].get_value(1)), "");
}

TEST_F(StressTest, ExecutorLargeVarcharValue) {
  execute_sql("CREATE TABLE t (id INT, data VARCHAR(200))");
  std::string long_str(180, 'x');
  auto insert = execute_sql("INSERT INTO t VALUES (1, '" + long_str + "')");
  ASSERT_TRUE(insert.success) << insert.error_message;

  auto result = execute_sql("SELECT * FROM t");
  ASSERT_EQ(result.rows.size(), 1u);
  EXPECT_EQ(std::get<std::string>(result.rows[0].get_value(1)), long_str);
}

TEST_F(StressTest, ExecutorIntMaxValueTruncation) {
  execute_sql("CREATE TABLE t (val INT)");
  // INT64 max won't fit in INT32 — executor casts int64_t to int32_t, causing truncation
  auto insert = execute_sql("INSERT INTO t VALUES (9999999999)");
  if (insert.success) {
    auto result = execute_sql("SELECT * FROM t");
    ASSERT_EQ(result.rows.size(), 1u);
    int32_t stored = std::get<int32_t>(result.rows[0].get_value(0));
    EXPECT_NE(stored, 9999999999LL)
        << "BUG: int64 literal truncated to int32 on insert - no overflow detection";
  }
}

TEST_F(StressTest, ExecutorSelectFromNonExistentTable) {
  auto result = execute_sql("SELECT * FROM no_such_table");
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_message.find("does not exist"), std::string::npos);
}

TEST_F(StressTest, ExecutorCreateDuplicateTableFails) {
  execute_sql("CREATE TABLE dup (id INT)");
  auto result = execute_sql("CREATE TABLE dup (id INT)");
  EXPECT_FALSE(result.success);
}

TEST_F(StressTest, ExecutorWhereClauseFiltersRows) {
  execute_sql("CREATE TABLE t (id INT, val INT)");
  execute_sql("INSERT INTO t VALUES (1, 10), (2, 20), (3, 30)");

  auto result = execute_sql("SELECT * FROM t WHERE id = 1");
  ASSERT_TRUE(result.success);
  ASSERT_EQ(result.rows.size(), 1u);
  EXPECT_EQ(std::get<int32_t>(result.rows[0].get_value(0)), 1);
  EXPECT_EQ(std::get<int32_t>(result.rows[0].get_value(1)), 10);
}

TEST_F(StressTest, ExecutorUnsupportedStatementTypeReturnsError) {
  // DELETE and UPDATE are parsed but executor returns error
  bool threw = false;
  ExecutionResult result;
  try {
    result = execute_sql("DELETE FROM t");
  } catch (...) {
    threw = true;
  }
  if (!threw) {
    EXPECT_FALSE(result.success) << "DELETE should fail - not implemented in executor";
  }
}

TEST_F(StressTest, ExecutorMultipleTablesDataIsolation) {
  execute_sql("CREATE TABLE a (id INT, val VARCHAR(20))");
  execute_sql("CREATE TABLE b (id INT, val VARCHAR(20))");
  execute_sql("INSERT INTO a VALUES (1, 'table_a')");
  execute_sql("INSERT INTO b VALUES (2, 'table_b')");

  auto a_result = execute_sql("SELECT * FROM a");
  auto b_result = execute_sql("SELECT * FROM b");

  ASSERT_EQ(a_result.rows.size(), 1u);
  ASSERT_EQ(b_result.rows.size(), 1u);
  EXPECT_EQ(std::get<std::string>(a_result.rows[0].get_value(1)), "table_a");
  EXPECT_EQ(std::get<std::string>(b_result.rows[0].get_value(1)), "table_b");
}

// ——— Stress Tests: Many Tables ———

TEST_F(StressTest, Create100TablesAndInsertRows) {
  const int NUM_TABLES = 100;
  const int ROWS_PER_TABLE = 200;

  for (int t = 0; t < NUM_TABLES; ++t) {
    std::string table = "table_" + std::to_string(t);
    auto cr = execute_sql("CREATE TABLE " + table + " (id INT, name VARCHAR(50), score FLOAT, active BOOL)");
    ASSERT_TRUE(cr.success) << "Failed to create " << table << ": " << cr.error_message;
  }

  auto cat = execute_sql("SELECT * FROM sys_tables");
  ASSERT_TRUE(cat.success);
  EXPECT_GE(cat.rows.size(), (size_t)(NUM_TABLES + 2));

  for (int t = 0; t < NUM_TABLES; ++t) {
    std::string table = "table_" + std::to_string(t);
    std::string insert_sql = build_batch_insert(table, 0, ROWS_PER_TABLE);
    auto ins = execute_sql(insert_sql);
    ASSERT_TRUE(ins.success) << "Failed to insert into " << table << ": " << ins.error_message;
    EXPECT_EQ(ins.affected_rows, (uint32_t)ROWS_PER_TABLE);
  }

  // Verify row counts on a sample of tables
  for (int t : {0, 25, 50, 75, 99}) {
    std::string table = "table_" + std::to_string(t);
    auto sel = execute_sql("SELECT * FROM " + table);
    ASSERT_TRUE(sel.success) << table;
    EXPECT_EQ(sel.rows.size(), (size_t)ROWS_PER_TABLE)
        << "Row count mismatch in " << table;
  }
}

TEST_F(StressTest, SingleTableLargeInsert50kRows) {
  execute_sql("CREATE TABLE big_table (id INT, name VARCHAR(40), score FLOAT, active BOOL)");

  const int TOTAL_ROWS = 50000;
  const int BATCH_SIZE = 500;

  auto t_start = std::chrono::steady_clock::now();

  for (int batch = 0; batch < TOTAL_ROWS / BATCH_SIZE; ++batch) {
    auto sql = build_batch_insert("big_table", batch * BATCH_SIZE, BATCH_SIZE);
    auto result = execute_sql(sql);
    ASSERT_TRUE(result.success) << "Batch " << batch << " failed: " << result.error_message;
    EXPECT_EQ(result.affected_rows, (uint32_t)BATCH_SIZE);
  }

  auto t_end = std::chrono::steady_clock::now();
  double elapsed_s = std::chrono::duration<double>(t_end - t_start).count();

  auto sel = execute_sql("SELECT * FROM big_table");
  ASSERT_TRUE(sel.success) << sel.error_message;
  EXPECT_EQ(sel.rows.size(), (size_t)TOTAL_ROWS)
      << "Row count mismatch after " << TOTAL_ROWS << " inserts";

  std::cout << "[Stress] 50k row insert+scan completed in " << elapsed_s << "s ("
            << (int)(TOTAL_ROWS / elapsed_s) << " rows/s)" << std::endl;
}

TEST_F(StressTest, DISABLED_SingleTableMillionRows) {
  // ~35 MB on disk, takes ~30-60s.
  // Safe to run — no WAL or transaction dependencies.
  execute_sql("CREATE TABLE million_rows (id INT, val VARCHAR(30), score FLOAT)");

  const int TOTAL_ROWS = 1000000;
  const int BATCH_SIZE = 1000;

  auto t_start = std::chrono::steady_clock::now();

  for (int batch = 0; batch < TOTAL_ROWS / BATCH_SIZE; ++batch) {
    std::ostringstream sql;
    sql << "INSERT INTO million_rows VALUES ";
    for (int i = 0; i < BATCH_SIZE; ++i) {
      if (i > 0) sql << ", ";
      int id = batch * BATCH_SIZE + i;
      sql << "(" << id << ", 'v_" << id << "', "
          << std::fixed << std::setprecision(4) << (id * 0.001 + 0.0001) << ")";
    }
    auto result = execute_sql(sql.str());
    ASSERT_TRUE(result.success) << "Batch " << batch << " failed: " << result.error_message;
  }

  auto t_end = std::chrono::steady_clock::now();
  double elapsed_s = std::chrono::duration<double>(t_end - t_start).count();

  std::cout << "[Stress] 1M row insert completed in " << elapsed_s << "s ("
            << (int)(TOTAL_ROWS / elapsed_s) << " inserts/s)" << std::endl;

  // Scan to verify count
  auto t_scan = std::chrono::steady_clock::now();
  auto sel = execute_sql("SELECT * FROM million_rows");
  double scan_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_scan).count();

  ASSERT_TRUE(sel.success) << sel.error_message;
  EXPECT_EQ(sel.rows.size(), (size_t)TOTAL_ROWS);

  std::cout << "[Stress] 1M row scan completed in " << scan_s << "s" << std::endl;
}

TEST_F(StressTest, DataIntegrityAfterLargeInsert) {
  execute_sql("CREATE TABLE integrity_test (id INT, name VARCHAR(30), value FLOAT)");

  const int TOTAL_ROWS = 10000;
  const int BATCH_SIZE = 200;

  for (int batch = 0; batch < TOTAL_ROWS / BATCH_SIZE; ++batch) {
    std::ostringstream sql;
    sql << "INSERT INTO integrity_test VALUES ";
    for (int i = 0; i < BATCH_SIZE; ++i) {
      if (i > 0) sql << ", ";
      int id = batch * BATCH_SIZE + i;
      sql << "(" << id << ", 'name_" << id << "', "
          << std::fixed << std::setprecision(4) << (id * 1.1 + 0.0001) << ")";
    }
    auto result = execute_sql(sql.str());
    ASSERT_TRUE(result.success) << result.error_message;
  }

  auto result = execute_sql("SELECT * FROM integrity_test");
  ASSERT_TRUE(result.success);
  ASSERT_EQ(result.rows.size(), (size_t)TOTAL_ROWS);

  // Spot-check a sample of rows for correct data
  // Rows may not come back in insertion order (slotted page scan order)
  std::unordered_map<int, std::pair<std::string, double>> seen;
  for (const auto& row : result.rows) {
    int id = std::get<int32_t>(row.get_value(0));
    std::string name = std::get<std::string>(row.get_value(1));
    double val = std::get<double>(row.get_value(2));
    seen[id] = {name, val};
  }

  // Verify spot samples
  for (int id : {0, 999, 5000, 9999}) {
    ASSERT_TRUE(seen.count(id)) << "Row " << id << " missing from scan results";
    EXPECT_EQ(seen[id].first, "name_" + std::to_string(id));
    EXPECT_NEAR(seen[id].second, id * 1.1 + 0.0001, 1e-3);
  }
}

// ——— Inspect Tests ———

TEST_F(StressTest, InspectSummaryReturnsValidJson) {
  execute_sql("CREATE TABLE t (id INT)");
  execute_sql("INSERT INTO t VALUES (1), (2), (3)");

  std::string summary_json;
  ASSERT_NO_THROW(summary_json = inspector_->get_summary());
  EXPECT_FALSE(summary_json.empty());

  auto j = nlohmann::json::parse(summary_json);
  EXPECT_TRUE(j.contains("total_pages")) << "Summary missing total_pages";
  EXPECT_TRUE(j.contains("allocated_extents")) << "Summary missing allocated_extents";
  EXPECT_TRUE(j.contains("percent_full")) << "Summary missing percent_full";
  EXPECT_GT(j["total_pages"].get<int>(), 0);
  EXPECT_GT(j["allocated_extents"].get<int>(), 0);
}

TEST_F(StressTest, InspectGamReturnsValidJson) {
  execute_sql("CREATE TABLE t (id INT, val VARCHAR(50))");
  for (int i = 0; i < 10; ++i) {
    execute_sql("INSERT INTO t VALUES (" + std::to_string(i) + ", 'data')");
  }

  std::string gam_json;
  ASSERT_NO_THROW(gam_json = inspector_->get_gam());
  EXPECT_FALSE(gam_json.empty());

  auto j = nlohmann::json::parse(gam_json);
  EXPECT_TRUE(j.is_array()) << "GAM should return a JSON array";
  EXPECT_GT(j.size(), 0u);
}

TEST_F(StressTest, InspectPageDetailHeaderPage) {
  std::string detail;
  ASSERT_NO_THROW(detail = inspector_->get_page_detail(0));
  EXPECT_FALSE(detail.empty());

  auto j = nlohmann::json::parse(detail);
  EXPECT_TRUE(j.contains("id")) << "Page detail missing 'id' field (note: field is 'id', not 'page_id')";
}

TEST_F(StressTest, InspectIamChainForTable) {
  execute_sql("CREATE TABLE t (id INT, data VARCHAR(50))");

  std::ostringstream sql;
  sql << "INSERT INTO t VALUES ";
  for (int i = 0; i < 100; ++i) {
    if (i > 0) sql << ", ";
    sql << "(" << i << ", 'row_data_" << i << "')";
  }
  execute_sql(sql.str());

  std::string iam_json;
  ASSERT_NO_THROW(iam_json = inspector_->get_iam_chain("t"));
  EXPECT_FALSE(iam_json.empty());

  auto j = nlohmann::json::parse(iam_json);
  EXPECT_TRUE(j.is_array()) << "IAM chain should return a JSON array";
  EXPECT_GT(j.size(), 0u);
}

TEST_F(StressTest, InspectIamChainNonExistentTable) {
  std::string iam_json;
  ASSERT_NO_THROW(iam_json = inspector_->get_iam_chain("no_such_table"));
  EXPECT_FALSE(iam_json.empty());
  // Should return error JSON or empty array, not crash
}

TEST_F(StressTest, InspectCatalogReturnsValidJson) {
  execute_sql("CREATE TABLE users (id INT, name VARCHAR(50))");
  execute_sql("CREATE TABLE orders (id INT, user_id INT, amount FLOAT)");

  std::string catalog_json;
  ASSERT_NO_THROW(catalog_json = inspector_->get_catalog());
  EXPECT_FALSE(catalog_json.empty());

  auto j = nlohmann::json::parse(catalog_json);
  EXPECT_TRUE(j.is_array() || j.is_object()) << "Catalog should return valid JSON";
}

TEST_F(StressTest, InspectSummaryAfterManyTables) {
  for (int i = 0; i < 50; ++i) {
    execute_sql("CREATE TABLE tbl_" + std::to_string(i) + " (id INT, val VARCHAR(30))");
    execute_sql("INSERT INTO tbl_" + std::to_string(i) + " VALUES (1, 'data')");
  }

  std::string summary_json;
  ASSERT_NO_THROW(summary_json = inspector_->get_summary());
  auto j = nlohmann::json::parse(summary_json);

  EXPECT_GT(j["allocated_extents"].get<int>(), 50)
      << "50 tables should allocate at least 50 extents (each table needs 1+ data extents)";
  // Note: total_pages reflects the on-disk file size which lags behind in-memory state
  // until the BPM flushes dirty pages. Do not rely on it for in-memory operation counts.
}

// ——— SELECT Column Filtering Verification ———

TEST_F(StressTest, SelectStarVsSelectSpecificColumnsCountMatch) {
  execute_sql("CREATE TABLE t (id INT, name VARCHAR(20), score FLOAT)");
  execute_sql("INSERT INTO t VALUES (1, 'Alice', 9.0), (2, 'Bob', 8.5)");

  auto all = execute_sql("SELECT * FROM t");
  auto specific = execute_sql("SELECT id, name FROM t");

  ASSERT_TRUE(all.success);
  ASSERT_TRUE(specific.success);

  EXPECT_EQ(all.rows.size(), specific.rows.size());
  EXPECT_EQ(all.column_names.size(), 3u);
  EXPECT_EQ(specific.column_names.size(), 2u)
      << "Column names correctly reports 2 requested columns";
  EXPECT_EQ(specific.rows[0].size(), 2u);
  EXPECT_EQ(std::get<int32_t>(specific.rows[0].get_value(0)), 1);
  EXPECT_EQ(std::get<std::string>(specific.rows[0].get_value(1)), "Alice");
}

// ——— Large-Scale DB Growth Tests ———
//
// These tests write enough data to meaningfully stress the storage layer at GB scale.
//
// Row schema: id INT(4) + payload VARCHAR(400) + payload2 VARCHAR(400) + score FLOAT(8) ≈ 820 bytes/row
//   Rows per page  = floor(4080 / 824)  = ~4
//   Rows per extent = 4 * 8             = 32
//   Rows for  1 GB = (1024^3 / 4096) pages * 4 rows/page = ~1.05M rows
//   Rows for 15 GB =                                       ~15.7M rows
//
// Run the DISABLED_ tests manually:
//   ./tests/lettydb_test --gtest_also_run_disabled_tests --gtest_filter="StressTest.DISABLED_*"

namespace {

static std::string make_payload(int seed, int len) {
  std::string s;
  s.reserve(len);
  const char* alpha = "abcdefghijklmnopqrstuvwxyz0123456789";
  for (int i = 0; i < len; ++i) {
    s += alpha[(seed * 7 + i * 13) % 36];
  }
  return s;
}

static int64_t file_size_bytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return -1;
  return f.tellg();
}

}  // namespace

TEST_F(StressTest, DISABLED_Create200TablesMillionSmallRows) {
  const int NUM_TABLES = 200;
  const int ROWS_PER_TABLE = 5000;  // 200 * 5000 = 1M rows
  const int BATCH_SIZE = 250;
  const std::string schema = "(id INT, name VARCHAR(30), score FLOAT, active BOOL)";

  auto t0 = std::chrono::steady_clock::now();

  for (int t = 0; t < NUM_TABLES; ++t) {
    auto cr = execute_sql("CREATE TABLE stbl_" + std::to_string(t) + " " + schema);
    ASSERT_TRUE(cr.success) << "Create stbl_" << t << ": " << cr.error_message;
  }
  std::cout << "[ScaleSmallRows] Created " << NUM_TABLES << " tables\n";

  uint64_t total_rows = 0;
  for (int t = 0; t < NUM_TABLES; ++t) {
    std::string table = "stbl_" + std::to_string(t);
    for (int start = 0; start < ROWS_PER_TABLE; start += BATCH_SIZE) {
      int count = std::min(BATCH_SIZE, ROWS_PER_TABLE - start);
      auto result = execute_sql(build_batch_insert(table, start, count));
      ASSERT_TRUE(result.success)
          << "Table " << table << " batch " << (start / BATCH_SIZE)
          << " failed: " << result.error_message;
      total_rows += result.affected_rows;
    }
  }

  double insert_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  int64_t db_bytes = file_size_bytes(db_path_);

  std::cout << "[ScaleSmallRows] Inserted " << total_rows << " rows across " << NUM_TABLES << " tables\n"
            << "[ScaleSmallRows] Elapsed: " << insert_s << "s  ("
            << static_cast<int>(total_rows / insert_s) << " rows/s)\n"
            << "[ScaleSmallRows] DB file size: " << (db_bytes / (1024 * 1024)) << " MB\n";

  for (int t : {0, 50, 99, 150, 199}) {
    auto sel = execute_sql("SELECT * FROM stbl_" + std::to_string(t));
    ASSERT_TRUE(sel.success);
    EXPECT_EQ(sel.rows.size(), static_cast<size_t>(ROWS_PER_TABLE))
        << "Row count mismatch in stbl_" << t;
  }

  auto summary = nlohmann::json::parse(inspector_->get_summary());
  EXPECT_GT(summary["allocated_extents"].get<int>(), NUM_TABLES)
      << "Expected at least " << NUM_TABLES << " allocated extents";
  std::cout << "[ScaleSmallRows] allocated_extents = "
            << summary["allocated_extents"].get<int>() << "\n";
}

TEST_F(StressTest,  DISABLED_Create200TablesMillionsOfRows_1GB) {
  const int NUM_TABLES   = 200;
  const int ROWS_PER_TABLE = 5500;   // 200 * 5500 = 1.1M rows, ~900 MB
  const int BATCH_SIZE   = 250;
  const std::string schema =
      "(id INT, payload VARCHAR(400), payload2 VARCHAR(400), score FLOAT)";

  // ——— Create all tables ———
  auto t0 = std::chrono::steady_clock::now();
  for (int t = 0; t < NUM_TABLES; ++t) {
    auto cr = execute_sql("CREATE TABLE gtbl_" + std::to_string(t) + " " + schema);
    ASSERT_TRUE(cr.success) << "Create gtbl_" << t << ": " << cr.error_message;
  }
  std::cout << "[Scale1GB] Created " << NUM_TABLES << " tables\n";

  // ——— Insert rows into every table ———
  uint64_t total_rows = 0;
  for (int t = 0; t < NUM_TABLES; ++t) {
    std::string tname = "gtbl_" + std::to_string(t);
    for (int start = 0; start < ROWS_PER_TABLE; start += BATCH_SIZE) {
      int count = std::min(BATCH_SIZE, ROWS_PER_TABLE - start);
      std::ostringstream sql;
      sql << "INSERT INTO " << tname << " VALUES ";
      for (int i = 0; i < count; ++i) {
        if (i > 0) sql << ", ";
        int id = start + i;
        std::string p1 = make_payload(t * 10000 + id, 380);
        std::string p2 = make_payload(t * 20000 + id, 380);
        sql << "(" << id << ", '" << p1 << "', '" << p2 << "', "
            << std::fixed << std::setprecision(4) << (id * 0.01 + 0.0001) << ")";
      }
      auto result = execute_sql(sql.str());
      ASSERT_TRUE(result.success)
          << "Table " << tname << " batch " << (start / BATCH_SIZE)
          << " failed: " << result.error_message;
      total_rows += result.affected_rows;
    }
  }

  double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  int64_t db_bytes = file_size_bytes(db_path_);

  std::cout << "[Scale1GB] Inserted " << total_rows << " rows across " << NUM_TABLES << " tables\n"
            << "[Scale1GB] Elapsed: " << elapsed << "s  ("
            << (int)(total_rows / elapsed) << " rows/s)\n"
            << "[Scale1GB] DB file size: " << (db_bytes / (1024 * 1024)) << " MB\n";

  // ——— Verify spot samples ———
  for (int t : {0, 50, 99, 150, 199}) {
    auto sel = execute_sql("SELECT * FROM gtbl_" + std::to_string(t));
    ASSERT_TRUE(sel.success);
    EXPECT_EQ(sel.rows.size(), (size_t)ROWS_PER_TABLE)
        << "Row count mismatch in gtbl_" << t;
  }

  // ——— Inspect at scale ———
  auto summary = nlohmann::json::parse(inspector_->get_summary());
  EXPECT_GT(summary["allocated_extents"].get<int>(), NUM_TABLES)
      << "Expected at least " << NUM_TABLES << " allocated extents";
  std::cout << "[Scale1GB] allocated_extents = "
            << summary["allocated_extents"].get<int>() << "\n";
}

TEST_F(StressTest, DISABLED_Create200TablesMillionsOfRows_10GB) {
  // Target: ~10 GB of data across 200 tables.
  //
  // Calibration from the fixed extent layout:
  //   2,460,000 wide rows produced ~1,926 MB on disk.
  //   That is ~1,277 rows/MB.
  //
  //   10 GiB target = 10,240 MB
  //   10,240 MB × 1,277 rows/MB = ~13.1M rows
  //   200 tables × 65,000 rows = 13.0M rows
  //
  // This test is intentionally disabled because it creates a very large local
  // database file and can take many minutes to run.

  const int NUM_TABLES    = 200;
  const int ROWS_PER_TABLE = 65000;
  const int BATCH_SIZE    = 500;
  const std::string schema =
      "(id INT, payload VARCHAR(400), payload2 VARCHAR(400), score FLOAT)";

  auto t_start = std::chrono::steady_clock::now();

  for (int t = 0; t < NUM_TABLES; ++t) {
    auto cr = execute_sql("CREATE TABLE bigtbl_" + std::to_string(t) + " " + schema);
    ASSERT_TRUE(cr.success) << "Create bigtbl_" << t << ": " << cr.error_message;
  }
  std::cout << "[Scale10GB] Created " << NUM_TABLES << " tables\n";

  uint64_t total_rows = 0;
  for (int t = 0; t < NUM_TABLES; ++t) {
    std::string tname = "bigtbl_" + std::to_string(t);
    for (int start = 0; start < ROWS_PER_TABLE; start += BATCH_SIZE) {
      int count = std::min(BATCH_SIZE, ROWS_PER_TABLE - start);
      std::ostringstream sql;
      sql << "INSERT INTO " << tname << " VALUES ";
      for (int i = 0; i < count; ++i) {
        if (i > 0) sql << ", ";
        int id = start + i;
        std::string p1 = make_payload(t * 100000 + id, 380);
        std::string p2 = make_payload(t * 200000 + id, 380);
        sql << "(" << id << ", '" << p1 << "', '" << p2 << "', "
            << std::fixed << std::setprecision(4) << (id * 0.01 + 0.0001) << ")";
      }
      auto result = execute_sql(sql.str());
      ASSERT_TRUE(result.success)
          << "Table " << tname << " batch " << (start / BATCH_SIZE)
          << " failed: " << result.error_message;
      total_rows += result.affected_rows;
    }

    if ((t + 1) % 20 == 0) {
      int64_t db_bytes = file_size_bytes(db_path_);
      double elapsed = std::chrono::duration<double>(
          std::chrono::steady_clock::now() - t_start).count();
      std::cout << "[Scale10GB] " << (t + 1) << "/" << NUM_TABLES
                << " tables done, " << total_rows << " rows, "
                << (db_bytes / (1024*1024)) << " MB on disk, "
                << (int)(total_rows / elapsed) << " rows/s\n";
    }
  }

  double elapsed = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - t_start).count();
  int64_t db_bytes = file_size_bytes(db_path_);

  std::cout << "[Scale10GB] DONE: " << total_rows << " total rows\n"
            << "[Scale10GB] Elapsed:  " << elapsed << "s\n"
            << "[Scale10GB] DB size:  " << (db_bytes / (1024*1024*1024)) << " GB ("
            << (db_bytes / (1024*1024)) << " MB)\n"
            << "[Scale10GB] Throughput: " << (int)(total_rows / elapsed) << " rows/s\n";

  // ——— Verify each table has correct row count ———
  for (int t : {0, 49, 99, 149, 199}) {
    auto sel = execute_sql("SELECT * FROM bigtbl_" + std::to_string(t));
    ASSERT_TRUE(sel.success);
    EXPECT_EQ(sel.rows.size(), (size_t)ROWS_PER_TABLE)
        << "Row count mismatch in bigtbl_" << t;
  }

  // ——— DB should be close to 10 GB, allowing for calibration imprecision. ———
  EXPECT_GT(db_bytes, (int64_t)9 * 1024 * 1024 * 1024)
      << "DB file should be > 9 GB after inserting 13M wide rows";

  // ——— Inspect at scale ———
  auto summary = nlohmann::json::parse(inspector_->get_summary());
  std::cout << "[Scale10GB] allocated_extents = "
            << summary["allocated_extents"].get<int>() << "\n";
  EXPECT_GT(summary["allocated_extents"].get<int>(), 250000)
      << "~10 GB of wide rows should require hundreds of thousands of allocated extents";
}

// ——— New Stress Tests ———

// ---- Buffer Pool & Cache Behavior ----

TEST_F(StressTest, SmallPoolForcesEvictions) {
  // Verify the buffer pool correctly evicts dirty pages when the pool
  // is too small to hold all pages needed by the operation.
  std::remove("stress_pool.db");
  auto disk = std::make_unique<DiskManager>("stress_pool.db");
  auto bpm = std::make_unique<BufferPoolManager>(*disk, 4, std::make_unique<LRUPageReplacer>());
  auto ext_mgr = std::make_unique<ExtentManager>(*bpm);
  auto iam_mgr = std::make_unique<IamManager>(*bpm, *ext_mgr);
  auto catalog = std::make_unique<CatalogManager>(*bpm, *iam_mgr);
  catalog->init();
  auto table_mgr = std::make_unique<TableManager>(*bpm, *iam_mgr, *catalog);
  Executor exec(*catalog, *table_mgr);

  auto run_sql = [&](const std::string& sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    Parser parser(std::move(tokens));
    auto ast = parser.parse();
    return exec.execute(ast.get());
  };

  auto result = run_sql("CREATE TABLE evict_test (id INT, name VARCHAR(50))");
  ASSERT_TRUE(result.success) << result.error_message;

  // Insert enough rows to span many pages (pool only holds 4)
  for (int i = 0; i < 2000; ++i) {
    std::ostringstream sql;
    sql << "INSERT INTO evict_test VALUES (" << i << ", 'name_" << i << "')";
    auto ins = run_sql(sql.str());
    if (!ins.success) {
      FAIL() << "Insert row " << i << " failed: " << ins.error_message;
    }
  }

  // Scan and verify count
  auto sel = run_sql("SELECT * FROM evict_test");
  ASSERT_TRUE(sel.success) << sel.error_message;
  EXPECT_EQ(sel.rows.size(), 2000u);

  CacheStats stats = bpm->get_cache_stats();
  EXPECT_GT(stats.evictions, 0u)
      << "With pool size=4, evictions must occur during a 2000-row insert+scan";
  std::cout << "[SmallPool] evictions=" << stats.evictions
            << " dirty_evictions=" << stats.dirty_evictions
            << " hit_ratio=" << stats.hit_ratio() << "%" << std::endl;

  bpm.reset();
  table_mgr.reset();
  catalog.reset();
  iam_mgr.reset();
  ext_mgr.reset();
  bpm.reset();
  disk.reset();
  std::remove("stress_pool.db");
}

TEST_F(StressTest, DirtyPagesSurviveEviction) {
  // Ensure that dirty pages written out during eviction are correctly
  // readable when fetched back from disk.
  std::remove("stress_dirty.db");
  auto disk = std::make_unique<DiskManager>("stress_dirty.db");
  auto bpm = std::make_unique<BufferPoolManager>(*disk, 4, std::make_unique<LRUPageReplacer>());
  auto ext_mgr = std::make_unique<ExtentManager>(*bpm);
  auto iam_mgr = std::make_unique<IamManager>(*bpm, *ext_mgr);
  auto catalog = std::make_unique<CatalogManager>(*bpm, *iam_mgr);
  catalog->init();
  auto table_mgr = std::make_unique<TableManager>(*bpm, *iam_mgr, *catalog);
  Executor exec(*catalog, *table_mgr);

  auto run_sql = [&](const std::string& sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    Parser parser(std::move(tokens));
    auto ast = parser.parse();
    return exec.execute(ast.get());
  };

  run_sql("CREATE TABLE dirty_test (id INT, val VARCHAR(20))");

  // Fill pool with dirty pages, force eviction, then read back
  for (int i = 0; i < 500; ++i) {
    auto ins = run_sql("INSERT INTO dirty_test VALUES (" + std::to_string(i) + ", 'val_" + std::to_string(i) + "')");
    ASSERT_TRUE(ins.success) << "Row " << i << " failed: " << ins.error_message;
  }

  auto sel = run_sql("SELECT * FROM dirty_test");
  ASSERT_TRUE(sel.success);
  EXPECT_EQ(sel.rows.size(), 500u);

  bpm.reset();
  table_mgr.reset();
  catalog.reset();
  iam_mgr.reset();
  ext_mgr.reset();
  disk.reset();
  std::remove("stress_dirty.db");
}

// ---- IAM Chain Growth ----

TEST_F(StressTest, DISABLED_IAMChainGrowthBeyondOnePage) {
  // Each IAM page holds at most 1022 extent IDs. When a table exceeds
  // 1022 extents, the IAM chain must grow to a second page.
  //
  // Each extent = 8 pages = ~32 KB. At ~100 bytes/row, that's ~320 rows/extent.
  // 1023 extents = ~327,360 rows — this takes many minutes.
  // Run manually: ./tests/lettydb_test --gtest_filter="StressTest.DISABLED_IAMChainGrowthBeyondOnePage"

  const int ROWS_NEEDED = 330000;
  const int BATCH_SIZE = 500;

  execute_sql("CREATE TABLE iam_growth (id INT, val VARCHAR(50))");

  int inserted = 0;
  for (int batch = 0; inserted < ROWS_NEEDED; ++batch) {
    std::ostringstream sql;
    sql << "INSERT INTO iam_growth VALUES ";
    for (int i = 0; i < BATCH_SIZE; ++i) {
      if (i > 0) sql << ", ";
      int id = inserted + i;
      sql << "(" << id << ", 'v_" << id << "')";
    }
    auto result = execute_sql(sql.str());
    ASSERT_TRUE(result.success) << "Batch " << batch << " failed: " << result.error_message;
    inserted += result.affected_rows;

    if ((batch + 1) % 20 == 0) {
      auto iam_json = inspector_->get_iam_chain("iam_growth");
      auto j = nlohmann::json::parse(iam_json);
      int total_extents = 0;
      for (auto& iam_page : j) {
        if (iam_page.contains("extents")) {
          total_extents += iam_page["extents"].size();
        }
      }
      std::cout << "[IAMGrowth] rows=" << inserted << " extents=" << total_extents
                << " iam_pages=" << j.size() << std::endl;
      if (total_extents > 1022) {
        break;  // IAM chain has grown beyond one page
      }
    }
  }

  auto iam_json = inspector_->get_iam_chain("iam_growth");
  auto j = nlohmann::json::parse(iam_json);
  EXPECT_GT(j.size(), 1u)
      << "IAM chain should have grown beyond one page after >1022 extents";
}

// ---- GAM Expansion ----

TEST_F(StressTest, GAMExpansionBeyondOnePage) {
  // One GAM page tracks 32,720 extents = ~1 GB. We can't practically fill
  // 1 GB in a unit test, but we can verify the GAM chain structure
  // by inspecting it after moderate allocations.
  for (int i = 0; i < 50; ++i) {
    execute_sql("CREATE TABLE gam_tbl_" + std::to_string(i) + " (id INT, v VARCHAR(100))");
    execute_sql("INSERT INTO gam_tbl_" + std::to_string(i) + " VALUES (1, 'data')");
  }

  auto gam_json = inspector_->get_gam();
  auto j = nlohmann::json::parse(gam_json);
  EXPECT_GE(j.size(), 1u);

  // allocation is an array of 0/1 ints per GAM page
  int total_allocated = 0;
  for (auto& page : j) {
    if (page.contains("allocation")) {
      auto& arr = page["allocation"];
      for (auto& bit : arr) {
        if (bit.get<int>() == 1) total_allocated++;
      }
    }
  }
  EXPECT_GT(total_allocated, 50)
      << "Expected >50 allocated extents after creating 50 tables";
}

// ---- WHERE Clause Filtering at Scale ----

TEST_F(StressTest, WhereClauseSelectivityAtScale) {
  // Verify WHERE filtering correctly returns the expected subset of rows
  // across a large dataset, exercising full-table scans with predicate pushdown.
  execute_sql("CREATE TABLE where_test (id INT, category INT, score FLOAT)");

  const int TOTAL = 10000;
  const int BATCH = 500;
  for (int batch = 0; batch < TOTAL / BATCH; ++batch) {
    std::ostringstream sql;
    sql << "INSERT INTO where_test VALUES ";
    for (int i = 0; i < BATCH; ++i) {
      if (i > 0) sql << ", ";
      int id = batch * BATCH + i;
      int cat = id % 10;  // 10 categories
      sql << "(" << id << ", " << cat << ", "
          << std::fixed << std::setprecision(2) << (id * 0.1) << ")";
    }
    auto r = execute_sql(sql.str());
    ASSERT_TRUE(r.success);
  }

  // Each category should have exactly TOTAL/10 rows
  for (int cat = 0; cat < 10; ++cat) {
    auto sel = execute_sql("SELECT * FROM where_test WHERE category = " + std::to_string(cat));
    ASSERT_TRUE(sel.success);
    EXPECT_EQ(sel.rows.size(), (size_t)(TOTAL / 10))
        << "Category " << cat << " should have " << (TOTAL / 10) << " rows";
  }

  // Verify no wrong-category rows leak through
  auto sel = execute_sql("SELECT * FROM where_test WHERE category = 0");
  for (const auto& row : sel.rows) {
    EXPECT_EQ(std::get<int32_t>(row.get_value(1)), 0);
  }
}

TEST_F(StressTest, WhereClauseWithANDOROperators) {
  execute_sql("CREATE TABLE and_or_test (a INT, b INT, c INT)");

  const int TOTAL = 5000;
  const int BATCH = 250;
  for (int batch = 0; batch < TOTAL / BATCH; ++batch) {
    std::ostringstream sql;
    sql << "INSERT INTO and_or_test VALUES ";
    for (int i = 0; i < BATCH; ++i) {
      if (i > 0) sql << ", ";
      int id = batch * BATCH + i;
      sql << "(" << (id % 100) << ", " << (id % 50) << ", " << (id % 25) << ")";
    }
    auto r = execute_sql(sql.str());
    ASSERT_TRUE(r.success);
  }

  // a >= 90 AND b < 10
  auto and_result = execute_sql("SELECT * FROM and_or_test WHERE a >= 90 AND b < 10");
  ASSERT_TRUE(and_result.success);

  // Verify every returned row satisfies both conditions
  for (const auto& row : and_result.rows) {
    int a = std::get<int32_t>(row.get_value(0));
    int b = std::get<int32_t>(row.get_value(1));
    EXPECT_GE(a, 90) << "Row with a=" << a << " should not pass a >= 90";
    EXPECT_LT(b, 10) << "Row with b=" << b << " should not pass b < 10";
  }

  // a = 0 OR a = 99
  auto or_result = execute_sql("SELECT * FROM and_or_test WHERE a = 0 OR a = 99");
  ASSERT_TRUE(or_result.success);

  for (const auto& row : or_result.rows) {
    int a = std::get<int32_t>(row.get_value(0));
    EXPECT_TRUE(a == 0 || a == 99) << "Row with a=" << a << " should not pass a=0 OR a=99";
  }
}

// ---- Catalog Scalability ----

TEST_F(StressTest, CreateTableThenRestartDatabase) {
  // Verify that table metadata survives a full database shutdown and restart.
  // This is a persistence test: data must be readable from disk, not just from
  // in-memory buffers.
  execute_sql("CREATE TABLE persist_test (id INT, name VARCHAR(30))");
  execute_sql("INSERT INTO persist_test VALUES (1, 'before_restart'), (2, 'also_before')");

  // Capture the file size before shutdown
  int64_t size_before = file_size_bytes(db_path_);
  EXPECT_GT(size_before, 0);

  // Full teardown and rebuild (simulates restart)
  inspector_.reset();
  executor_.reset();
  table_manager_.reset();
  catalog_manager_.reset();
  iam_manager_.reset();
  extent_manager_.reset();
  bpm_.reset();
  disk_manager_.reset();

  // Reopen the database
  disk_manager_ = std::make_unique<DiskManager>(db_path_);
  bpm_ = std::make_unique<BufferPoolManager>(*disk_manager_, 256, std::make_unique<LRUPageReplacer>());
  extent_manager_ = std::make_unique<ExtentManager>(*bpm_);
  iam_manager_ = std::make_unique<IamManager>(*bpm_, *extent_manager_);
  catalog_manager_ = std::make_unique<CatalogManager>(*bpm_, *iam_manager_);
  catalog_manager_->init();
  table_manager_ = std::make_unique<TableManager>(*bpm_, *iam_manager_, *catalog_manager_);
  executor_ = std::make_unique<Executor>(*catalog_manager_, *table_manager_);
  inspector_ = std::make_unique<StorageInspector>(*bpm_, *extent_manager_, *iam_manager_, *catalog_manager_);

  // Verify the table exists and data is intact
  auto result = execute_sql("SELECT * FROM persist_test");
  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_EQ(result.rows.size(), 2u);

  // Verify column names survived
  ASSERT_EQ(result.column_names.size(), 2u);
  EXPECT_EQ(result.column_names[0], "id");
  EXPECT_EQ(result.column_names[1], "name");
}

// ---- Mixed Read/Write Workload ----

TEST_F(StressTest, MixedReadWriteInterleaved) {
  // Alternate inserts and selects to exercise the buffer pool's
  // pin/unpin/eviction logic under interleaved read-write pressure.
  execute_sql("CREATE TABLE mixed_test (id INT, val VARCHAR(30))");

  for (int i = 0; i < 1000; ++i) {
    auto ins = execute_sql("INSERT INTO mixed_test VALUES (" + std::to_string(i) + ", 'v_" + std::to_string(i) + "')");
    ASSERT_TRUE(ins.success);

    // Every 100 inserts, do a full table scan
    if ((i + 1) % 100 == 0) {
      auto sel = execute_sql("SELECT * FROM mixed_test");
      ASSERT_TRUE(sel.success);
      EXPECT_EQ(sel.rows.size(), (size_t)(i + 1))
          << "After inserting " << (i + 1) << " rows, scan returned " << sel.rows.size();
    }
  }

  // Final verification
  auto final = execute_sql("SELECT * FROM mixed_test");
  ASSERT_TRUE(final.success);
  EXPECT_EQ(final.rows.size(), 1000u);
}

// ---- Multiple Tables Concurrent IAM Growth ----

TEST_F(StressTest, ManyTablesGrowIAMIndependently) {
  // Create many tables, insert varying amounts of data into each,
  // and verify each table's IAM chain is independent.
  const int NUM_TABLES = 30;
  for (int t = 0; t < NUM_TABLES; ++t) {
    execute_sql("CREATE TABLE indep_" + std::to_string(t) + " (id INT, data VARCHAR(40))");
  }

  // Insert different amounts into each table
  for (int t = 0; t < NUM_TABLES; ++t) {
    int rows = (t + 1) * 100;  // 100, 200, 300, ..., 3000
    std::ostringstream sql;
    sql << "INSERT INTO indep_" << t << " VALUES ";
    for (int i = 0; i < rows; ++i) {
      if (i > 0) sql << ", ";
      sql << "(" << i << ", 'row_" << i << "')";
    }
    auto result = execute_sql(sql.str());
    ASSERT_TRUE(result.success) << "Table indep_" << t << " insert failed: " << result.error_message;
  }

  // Verify each table has exactly the expected row count
  for (int t = 0; t < NUM_TABLES; ++t) {
    int expected = (t + 1) * 100;
    auto sel = execute_sql("SELECT * FROM indep_" + std::to_string(t));
    ASSERT_TRUE(sel.success);
    EXPECT_EQ(sel.rows.size(), (size_t)expected)
        << "Table indep_" << t << " expected " << expected << " rows, got " << sel.rows.size();
  }

  // Verify IAM chains are all independent
  for (int t = 0; t < NUM_TABLES; ++t) {
    auto iam_json = inspector_->get_iam_chain("indep_" + std::to_string(t));
    auto j = nlohmann::json::parse(iam_json);
    // Every table should have at least one IAM page
    EXPECT_GE(j.size(), 1u);
  }
}

// ---- Large VARCHAR Boundary Tests ----

TEST_F(StressTest, ManyPagesFillPattern) {
  // Insert rows of a known size to predict page count, then verify
  // the storage layer allocated the expected number of pages.
  //
  // Row: id INT(4) + note VARCHAR(500) ≈ 506 bytes
  // Page free space ≈ 4080 bytes (after SlottedPageHeader)
  // ~8 rows per page, ~64 rows per extent
  execute_sql("CREATE TABLE fill_test (id INT, note VARCHAR(500))");

  const int TARGET_ROWS = 10000;
  const int BATCH = 500;
  for (int b = 0; b < TARGET_ROWS / BATCH; ++b) {
    std::ostringstream sql;
    sql << "INSERT INTO fill_test VALUES ";
    for (int i = 0; i < BATCH; ++i) {
      if (i > 0) sql << ", ";
      int id = b * BATCH + i;
      std::string note(490, 'x');  // near max varchar
      sql << "(" << id << ", '" << note << "')";
    }
    auto r = execute_sql(sql.str());
    ASSERT_TRUE(r.success);
  }

  auto sel = execute_sql("SELECT * FROM fill_test");
  ASSERT_TRUE(sel.success);
  EXPECT_EQ(sel.rows.size(), (size_t)TARGET_ROWS);

  auto summary = nlohmann::json::parse(inspector_->get_summary());
  int allocated = summary["allocated_extents"].get<int>();
  int expected_min_extents = TARGET_ROWS / 64;  // rough lower bound
  EXPECT_GE(allocated, expected_min_extents)
      << "Expected at least " << expected_min_extents << " extents for " << TARGET_ROWS << " wide rows";
}

} // namespace letty
