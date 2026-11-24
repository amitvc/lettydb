#include <gtest/gtest.h>
#include "catalog/catalog_manager.h"
#include "storage/disk_manager.h"
#include "buffer/buffer_pool_manager.h"
#include "buffer/lru_replacer.h"
#include "storage/log_manager.h"
#include "transaction/transaction_manager.h"
#include "common/db_exception.h"
#include <cstdio>
#include <vector>

namespace letty {

class CreateTableTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::remove("test_catalog_create.db");
    disk_manager = std::make_unique<DiskManager>("test_catalog_create.db");
    bpm = std::make_unique<BufferPoolManager>(
        *disk_manager, 64, std::make_unique<LRUPageReplacer>());
    log_manager = std::make_unique<LogManager>("test_catalog_create.db.wal");
    txn_manager = std::make_unique<TransactionManager>(*log_manager);
    page_allocator = std::make_unique<PageAllocator>(*bpm, *log_manager, *txn_manager);
    catalog_manager = std::make_unique<CatalogManager>(*bpm, *page_allocator, *txn_manager);
    catalog_manager->init();
  }

  void TearDown() override {
    std::remove("test_catalog_create.db");
    std::remove("test_catalog_create.db.wal.0");
  }

  std::unique_ptr<DiskManager> disk_manager;
  std::unique_ptr<BufferPoolManager> bpm;
  std::unique_ptr<LogManager> log_manager;
  std::unique_ptr<TransactionManager> txn_manager;
  std::unique_ptr<PageAllocator> page_allocator;
  std::unique_ptr<CatalogManager> catalog_manager;
};

TEST_F(CreateTableTest, CreateAndGetTable) {
  // Define a schema
  std::vector<Column> columns;
  columns.emplace_back("id", DataType::INTEGER, 4, 0);
  columns.emplace_back("username", DataType::VARCHAR, 32, 4);
  Schema schema(columns);

  // Create table
  bool success = catalog_manager->create_table("users", schema);
  EXPECT_TRUE(success);

  // Get table metadata
  auto table_meta = catalog_manager->get_table("users");
  ASSERT_TRUE(table_meta.has_value());
  EXPECT_EQ(table_meta->get().name, "users");
  EXPECT_GT(table_meta->get().table_id, 0); // Should be valid table_id

  // Verify columns loaded correctly
  auto& loaded_cols = table_meta->get().schema.get_columns();
  ASSERT_EQ(loaded_cols.size(), 2);
  
  EXPECT_EQ(loaded_cols[0].get_name(), "id");
  EXPECT_EQ(loaded_cols[0].get_type(), DataType::INTEGER);
  EXPECT_EQ(loaded_cols[0].get_length(), 4);
  EXPECT_EQ(loaded_cols[0].get_offset(), 0);
  
  EXPECT_EQ(loaded_cols[1].get_name(), "username");
  EXPECT_EQ(loaded_cols[1].get_type(), DataType::VARCHAR);
  EXPECT_EQ(loaded_cols[1].get_length(), 32);
  EXPECT_EQ(loaded_cols[1].get_offset(), 4);
  
  // Verify duplicates are rejected
  EXPECT_THROW({
    catalog_manager->create_table("users", schema);
  }, DbException);
}

} // namespace letty
