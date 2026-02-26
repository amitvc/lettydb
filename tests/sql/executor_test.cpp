#include <gtest/gtest.h>
#include <cstdio>
#include "sql/executor.h"
#include "sql/parser.h"
#include "sql/lexer.h"
#include "storage/disk_manager.h"
#include "buffer/buffer_pool_manager.h"
#include "buffer/lru_replacer.h"
#include "storage/extent_manager.h"
#include "storage/iam_manager.h"
#include "catalog/catalog_manager.h"
#include "storage/table_manager.h"

namespace letty {

class ExecutorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::remove("test_executor.db");
    disk_manager = std::make_unique<DiskManager>("test_executor.db");
    bpm = std::make_unique<BufferPoolManager>(
        *disk_manager, 64, std::make_unique<LRUPageReplacer>());
    extent_manager = std::make_unique<ExtentManager>(*bpm);
    iam_manager = std::make_unique<IamManager>(*bpm, *extent_manager);
    catalog_manager = std::make_unique<CatalogManager>(*bpm, *iam_manager);
    catalog_manager->init();
    table_manager = std::make_unique<TableManager>(*bpm, *iam_manager, *catalog_manager);
    executor = std::make_unique<Executor>(*catalog_manager, *table_manager);
  }

  void TearDown() override {
    executor.reset();
    table_manager.reset();
    catalog_manager.reset();
    iam_manager.reset();
    extent_manager.reset();
    bpm.reset();
    disk_manager.reset();
    std::remove("test_executor.db");
  }

  ExecutionResult execute_sql(const std::string& sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    Parser parser(std::move(tokens));
    auto ast = parser.parse();
    return executor->execute(ast.get());
  }

  std::unique_ptr<DiskManager> disk_manager;
  std::unique_ptr<BufferPoolManager> bpm;
  std::unique_ptr<ExtentManager> extent_manager;
  std::unique_ptr<IamManager> iam_manager;
  std::unique_ptr<CatalogManager> catalog_manager;
  std::unique_ptr<TableManager> table_manager;
  std::unique_ptr<Executor> executor;
};

TEST_F(ExecutorTest, CreateTableSuccess) {
  auto result = execute_sql("CREATE TABLE users (id INT, name VARCHAR(50))");
  
  ASSERT_TRUE(result.success) << result.error_message;
  
  // Verify table exists
  auto table = catalog_manager->get_table("users");
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->name, "users");
  EXPECT_EQ(table->schema.get_columns().size(), 2);
}

TEST_F(ExecutorTest, CreateTableDuplicate) {
  execute_sql("CREATE TABLE test (id INT)");
  
  // Creating again should fail
  auto result = execute_sql("CREATE TABLE test (id INT)");
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error_message.empty());
}

TEST_F(ExecutorTest, InsertSingleRow) {
  execute_sql("CREATE TABLE products (id INT, price FLOAT)");
  
  auto result = execute_sql("INSERT INTO products VALUES (1, 19.99)");
  
  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_EQ(result.affected_rows, 1);
}

TEST_F(ExecutorTest, InsertMultipleRows) {
  execute_sql("CREATE TABLE items (id INT, name VARCHAR(20))");
  
  auto result = execute_sql("INSERT INTO items VALUES (1, 'Apple'), (2, 'Banana'), (3, 'Cherry')");
  
  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_EQ(result.affected_rows, 3);
}

TEST_F(ExecutorTest, InsertIntoNonExistentTable) {
  auto result = execute_sql("INSERT INTO nonexistent VALUES (1)");
  
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.error_message.find("does not exist") != std::string::npos);
}

TEST_F(ExecutorTest, SelectAll) {
  execute_sql("CREATE TABLE users (id INT, name VARCHAR(30))");
  execute_sql("INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob')");
  
  auto result = execute_sql("SELECT * FROM users");
  
  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_EQ(result.rows.size(), 2);
  EXPECT_EQ(result.column_names.size(), 2);
  EXPECT_EQ(result.column_names[0], "id");
  EXPECT_EQ(result.column_names[1], "name");
  
  // Verify first row
  EXPECT_EQ(std::get<int32_t>(result.rows[0].get_value(0)), 1);
  EXPECT_EQ(std::get<std::string>(result.rows[0].get_value(1)), "Alice");
  
  // Verify second row
  EXPECT_EQ(std::get<int32_t>(result.rows[1].get_value(0)), 2);
  EXPECT_EQ(std::get<std::string>(result.rows[1].get_value(1)), "Bob");
}

TEST_F(ExecutorTest, SelectFromEmptyTable) {
  execute_sql("CREATE TABLE empty_table (id INT)");
  
  auto result = execute_sql("SELECT * FROM empty_table");
  
  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_EQ(result.rows.size(), 0);
}

TEST_F(ExecutorTest, SelectFromNonExistentTable) {
  auto result = execute_sql("SELECT * FROM nonexistent");
  
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.error_message.find("does not exist") != std::string::npos);
}

TEST_F(ExecutorTest, EndToEndWorkflow) {
  // Create table
  auto create_result = execute_sql("CREATE TABLE employees (id INT, name VARCHAR(40), salary FLOAT)");
  ASSERT_TRUE(create_result.success) << create_result.error_message;
  
  // Insert data
  auto insert_result = execute_sql("INSERT INTO employees VALUES (1, 'Alice', 50000.0)");
  ASSERT_TRUE(insert_result.success) << insert_result.error_message;
  
  insert_result = execute_sql("INSERT INTO employees VALUES (2, 'Bob', 60000.0)");
  ASSERT_TRUE(insert_result.success) << insert_result.error_message;
  
  // Select and verify
  auto select_result = execute_sql("SELECT * FROM employees");
  ASSERT_TRUE(select_result.success) << select_result.error_message;
  ASSERT_EQ(select_result.rows.size(), 2);
  
  // Verify data integrity
  bool found_alice = false, found_bob = false;
  for (const auto& row : select_result.rows) {
    std::string name = std::get<std::string>(row.get_value(1));
    if (name == "Alice") {
      found_alice = true;
      EXPECT_EQ(std::get<int32_t>(row.get_value(0)), 1);
      EXPECT_DOUBLE_EQ(std::get<double>(row.get_value(2)), 50000.0);
    } else if (name == "Bob") {
      found_bob = true;
      EXPECT_EQ(std::get<int32_t>(row.get_value(0)), 2);
      EXPECT_DOUBLE_EQ(std::get<double>(row.get_value(2)), 60000.0);
    }
  }
  EXPECT_TRUE(found_alice);
  EXPECT_TRUE(found_bob);
}

TEST_F(ExecutorTest, SelectSystemTables) {
  // sys_tables should have at least sys_tables and sys_columns
  auto result = execute_sql("SELECT * FROM sys_tables");
  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_GE(result.rows.size(), 2);  // sys_tables + sys_columns
  EXPECT_EQ(result.column_names.size(), 4);  // oid, name, iam_page_id, column_count

  // sys_columns should have columns for both system tables (4 + 5 = 9 total)
  auto col_result = execute_sql("SELECT * FROM sys_columns");
  ASSERT_TRUE(col_result.success) << col_result.error_message;
  EXPECT_GE(col_result.rows.size(), 9);
  EXPECT_EQ(col_result.column_names.size(), 5);  // table_oid, name, type, length, offset
}

TEST_F(ExecutorTest, SelectSystemTablesAfterCreate) {
  execute_sql("CREATE TABLE users (id INT, name VARCHAR(30))");

  auto result = execute_sql("SELECT * FROM sys_tables");
  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_GE(result.rows.size(), 3);  // sys_tables + sys_columns + users

  // Verify users columns show up in sys_columns (9 system + 2 user = 11)
  auto col_result = execute_sql("SELECT * FROM sys_columns");
  ASSERT_TRUE(col_result.success) << col_result.error_message;
  EXPECT_GE(col_result.rows.size(), 11);
}

} // namespace letty
