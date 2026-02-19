//
// Created by Antigravity on 1/27/26.
//

#include <gtest/gtest.h>
#include <cstdio>
#include "storage/table_manager.h"
#include "storage/disk_manager.h"
#include "buffer/buffer_pool_manager.h"
#include "buffer/lru_replacer.h"
#include "storage/extent_manager.h"
#include "storage/iam_manager.h"
#include "catalog/catalog_manager.h"

namespace letty {

class TableManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::remove("test_table_manager.db");
    disk_manager = std::make_unique<DiskManager>("test_table_manager.db");
    bpm = std::make_unique<BufferPoolManager>(
        *disk_manager, 64, std::make_unique<LRUPageReplacer>());
    extent_manager = std::make_unique<ExtentManager>(*bpm);
    iam_manager = std::make_unique<IamManager>(*bpm, *extent_manager);
    catalog_manager = std::make_unique<CatalogManager>(*bpm, *iam_manager);
    catalog_manager->init();
    table_manager = std::make_unique<TableManager>(*bpm, *iam_manager, *catalog_manager);
  }

  void TearDown() override {
    table_manager.reset();
    catalog_manager.reset();
    iam_manager.reset();
    extent_manager.reset();
    bpm.reset();
    disk_manager.reset();
    std::remove("test_table_manager.db");
  }

  std::unique_ptr<DiskManager> disk_manager;
  std::unique_ptr<BufferPoolManager> bpm;
  std::unique_ptr<ExtentManager> extent_manager;
  std::unique_ptr<IamManager> iam_manager;
  std::unique_ptr<CatalogManager> catalog_manager;
  std::unique_ptr<TableManager> table_manager;
};

TEST_F(TableManagerTest, InsertAndScanRawData) {
  // Create a simple table
  std::vector<Column> columns;
  columns.emplace_back("id", DataType::INTEGER, 4, 0);
  columns.emplace_back("name", DataType::VARCHAR, 32, 4);
  Schema schema(columns);
  
  ASSERT_TRUE(catalog_manager->create_table("users", schema));
  
  // Insert raw data (simulating a simple row: id=1, name="Alice")
  struct TestRow {
    int32_t id;
    uint16_t name_len;
    char name[5];
  } __attribute__((packed));
  
  TestRow row1 = {1, 5, {'A', 'l', 'i', 'c', 'e'}};
  ASSERT_TRUE(table_manager->insert_row_raw("users", reinterpret_cast<char*>(&row1), sizeof(row1)));
  
  TestRow row2 = {2, 3, {'B', 'o', 'b'}};
  // Adjust size for Bob (shorter name)
  ASSERT_TRUE(table_manager->insert_row_raw("users", reinterpret_cast<char*>(&row2), 4 + 2 + 3));
  
  // Scan and count rows
  int count = 0;
  bool scan_result = table_manager->scan_table("users", [&count](const char* data, uint32_t size) {
    count++;
  });
  
  ASSERT_TRUE(scan_result);
  EXPECT_EQ(count, 2);
}

TEST_F(TableManagerTest, InsertAndScanWithTuple) {
  // Create a table with multiple types
  std::vector<Column> columns;
  columns.emplace_back("id", DataType::INTEGER, 4, 0);
  columns.emplace_back("score", DataType::DOUBLE, 8, 4);
  columns.emplace_back("active", DataType::BOOLEAN, 1, 12);
  Schema schema(columns);
  
  ASSERT_TRUE(catalog_manager->create_table("scores", schema));
  
  // Insert using Tuple
  Tuple tuple1;
  tuple1.add_value(int32_t(100));
  tuple1.add_value(double(95.5));
  tuple1.add_value(true);
  ASSERT_TRUE(table_manager->insert_row("scores", tuple1));
  
  Tuple tuple2;
  tuple2.add_value(int32_t(200));
  tuple2.add_value(double(87.3));
  tuple2.add_value(false);
  ASSERT_TRUE(table_manager->insert_row("scores", tuple2));
  
  // Scan and verify
  std::vector<Tuple> results;
  bool scan_result = table_manager->scan_table_tuples("scores", [&results](const Tuple& tuple) {
    results.push_back(tuple);
  });
  
  ASSERT_TRUE(scan_result);
  ASSERT_EQ(results.size(), 2);
  
  // Verify first row
  EXPECT_EQ(std::get<int32_t>(results[0].get_value(0)), 100);
  EXPECT_DOUBLE_EQ(std::get<double>(results[0].get_value(1)), 95.5);
  EXPECT_EQ(std::get<bool>(results[0].get_value(2)), true);
  
  // Verify second row
  EXPECT_EQ(std::get<int32_t>(results[1].get_value(0)), 200);
  EXPECT_DOUBLE_EQ(std::get<double>(results[1].get_value(1)), 87.3);
  EXPECT_EQ(std::get<bool>(results[1].get_value(2)), false);
}

TEST_F(TableManagerTest, ScanNonExistentTable) {
  bool result = table_manager->scan_table("nonexistent", [](const char*, uint32_t) {});
  EXPECT_FALSE(result);
}

TEST_F(TableManagerTest, InsertIntoNonExistentTable) {
  Tuple tuple;
  tuple.add_value(int32_t(1));
  
  EXPECT_FALSE(table_manager->insert_row("nonexistent", tuple));
}

TEST_F(TableManagerTest, MultipleInsertsFillPage) {
  // Create a table and insert many rows to verify page expansion works
  std::vector<Column> columns;
  columns.emplace_back("id", DataType::INTEGER, 4, 0);
  Schema schema(columns);
  
  ASSERT_TRUE(catalog_manager->create_table("numbers", schema));
  
  // Insert 100 rows
  for (int i = 0; i < 100; ++i) {
    Tuple tuple;
    tuple.add_value(int32_t(i));
    ASSERT_TRUE(table_manager->insert_row("numbers", tuple));
  }
  
  // Scan and verify count
  int count = 0;
  table_manager->scan_table_tuples("numbers", [&count](const Tuple& tuple) {
    count++;
  });
  
  EXPECT_EQ(count, 100);
}

} // namespace letty
