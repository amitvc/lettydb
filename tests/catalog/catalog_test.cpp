#include <gtest/gtest.h>
#include "catalog/catalog_manager.h"
#include "storage/disk_manager.h"
#include "buffer/buffer_pool_manager.h"
#include "buffer/lru_replacer.h"
#include "storage/log_manager.h"
#include "transaction/transaction_manager.h"
#include <cstdio>

namespace letty {

class CatalogBootstrapTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::remove("test_catalog.db");
    disk_manager = std::make_unique<DiskManager>("test_catalog.db");
    bpm = std::make_unique<BufferPoolManager>(
        *disk_manager, 64, std::make_unique<LRUPageReplacer>());
    log_manager = std::make_unique<LogManager>("test_catalog.db.wal");
    txn_manager = std::make_unique<TransactionManager>(*log_manager);

	page_allocator = std::make_unique<PageAllocator>(*bpm, *log_manager, *txn_manager);
    catalog_manager = std::make_unique<CatalogManager>(*bpm, *page_allocator, *txn_manager);
  }

  void TearDown() override {
    std::remove("test_catalog.db");
    std::remove("test_catalog.db.wal.0");
  }

  std::unique_ptr<DiskManager> disk_manager;
  std::unique_ptr<BufferPoolManager> bpm;
  std::unique_ptr<LogManager> log_manager;
  std::unique_ptr<TransactionManager> txn_manager;
  std::unique_ptr<PageAllocator> page_allocator;
  std::unique_ptr<CatalogManager> catalog_manager;
};

TEST_F(CatalogBootstrapTest, CheckSystemTablesExist) {
  // Should trigger bootstrap
  catalog_manager->init();
  
  // Verify sys_tables exists
  auto table_meta = catalog_manager->get_table("sys_tables");
  ASSERT_TRUE(table_meta.has_value());
  EXPECT_EQ(table_meta->get().table_id, SYS_TABLES_TABLE_OID);
  EXPECT_EQ(table_meta->get().name, "sys_tables");

  // Verify sys_columns exists
  auto col_meta = catalog_manager->get_table("sys_columns");
  ASSERT_TRUE(col_meta.has_value());
  EXPECT_EQ(col_meta->get().table_id, SYS_COLUMNS_TABLE_OID);
  EXPECT_EQ(col_meta->get().name, "sys_columns");

  // Verify we can find a non-existent table (should be nullopt)
  auto no_table = catalog_manager->get_table("ghost_table");
  EXPECT_FALSE(no_table.has_value());
}

}
