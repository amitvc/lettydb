#include <gtest/gtest.h>
#include "catalog/catalog_manager.h"
#include "storage/disk_manager.h"
#include "buffer/buffer_pool_manager.h"
#include "buffer/lru_replacer.h"
#include "storage/extent_manager.h"
#include "storage/iam_manager.h"
#include <cstdio>

namespace letty {

class CatalogBootstrapTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::remove("test_catalog.db");
    disk_manager = std::make_unique<DiskManager>("test_catalog.db");
    bpm = std::make_unique<BufferPoolManager>(
        *disk_manager, 64, std::make_unique<LRUPageReplacer>());
    extent_manager = std::make_unique<ExtentManager>(*bpm);
    iam_manager = std::make_unique<IamManager>(*bpm, *extent_manager);
    catalog_manager = std::make_unique<CatalogManager>(*bpm, *iam_manager);
  }

  void TearDown() override {
    std::remove("test_catalog.db");
  }

  std::unique_ptr<DiskManager> disk_manager;
  std::unique_ptr<BufferPoolManager> bpm;
  std::unique_ptr<ExtentManager> extent_manager;
  std::unique_ptr<IamManager> iam_manager;
  std::unique_ptr<CatalogManager> catalog_manager;
};

TEST_F(CatalogBootstrapTest, CheckSystemTablesExist) {
  // Should trigger bootstrap
  catalog_manager->init();
  
  // Verify sys_tables exists
  auto table_meta = catalog_manager->get_table("sys_tables");
  ASSERT_NE(table_meta, nullptr);
  EXPECT_EQ(table_meta->table_id, SYS_TABLES_TABLE_OID);
  EXPECT_EQ(table_meta->name, "sys_tables");
  
  // Verify sys_columns exists
  auto col_meta = catalog_manager->get_table("sys_columns");
  ASSERT_NE(col_meta, nullptr);
  EXPECT_EQ(col_meta->table_id, SYS_COLUMNS_TABLE_OID);
  EXPECT_EQ(col_meta->name, "sys_columns");
  
  // Verify we can find a non-existent table (should be null)
  auto no_table = catalog_manager->get_table("ghost_table");
  EXPECT_EQ(no_table, nullptr);
}

}
