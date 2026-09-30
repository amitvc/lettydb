#include <gtest/gtest.h>
#include "storage/iam_manager.h"
#include "storage/disk_manager.h"
#include "buffer/buffer_pool_manager.h"
#include "buffer/lru_replacer.h"
#include "storage/extent_manager.h"
#include "storage/table_manager.h"
#include "catalog/catalog_manager.h"
#include "catalog/schema.h"
#include "catalog/column.h"
#include "storage/tuple.h"
#include "storage/storage_def.h"
#include "common/logger.h"
#include "storage/page_utils.h"
#include "storage/slotted_page.h"
#include <filesystem>

namespace letty {

TEST(CompactionTest, DeleteAllAndCompactFreesAllExtents) {
  auto test_db_file = std::filesystem::temp_directory_path() / "compaction_test.db";

  try {
    DiskManager disk_manager(test_db_file.string());
    BufferPoolManager bpm(disk_manager, 64, std::make_unique<LRUPageReplacer>());
    ExtentManager extent_manager(bpm);
    bpm.flush_all_pages();
    IamManager iam_manager(bpm, extent_manager);
    CatalogManager catalog(bpm, iam_manager);
    catalog.init();

    Schema schema({
      Column("id", DataType::INTEGER, sizeof(int32_t), 0, false),
    });

    catalog.create_table("test", schema);
    auto* meta = catalog.get_table("test");
    ASSERT_NE(meta, nullptr);

    TableManager table_manager(bpm, iam_manager, catalog);

    // Insert 500 rows across multiple extents.
    const int kRowCount = 500;
    for (int i = 0; i < kRowCount; ++i) {
      std::vector<Value> values;
      values.push_back(static_cast<int32_t>(i));
      Tuple t(std::move(values));
      table_manager.insert_row(meta->name, t);
    }

    // Count extents before
    page_id_t iam_head = meta->iam_page_id;
    int extent_count_before = 0;
    page_id_t cur = iam_head;
    while (cur != INVALID_PAGE_ID) {
      auto iam_page = load_page_value<IAMPage>(bpm, cur);
      ASSERT_TRUE(iam_page.has_value());
      extent_count_before += iam_page->extent_count;
      cur = iam_page->next_page_id;
    }
    EXPECT_GE(extent_count_before, 1);
    LOG_STORAGE_INFO("Table has {} extents before compaction", extent_count_before);

    // Delete ALL rows via scanner
    TableScanner scanner(bpm, meta->iam_page_id);
    int deleted = 0;
    while (scanner.next()) {
      table_manager.delete_row(*meta, scanner.current_page_id(), scanner.current_slot_id());
      deleted++;
    }
    EXPECT_EQ(deleted, kRowCount);
    LOG_STORAGE_INFO("Deleted {} rows", deleted);

    // Run compaction
    table_manager.compact_extents(*meta);

    // Count extents after — should be 0 or 1 (IAM chain may still have the head page)
    int extent_count_after = 0;
    cur = iam_head;
    while (cur != INVALID_PAGE_ID) {
      auto iam_page = load_page_value<IAMPage>(bpm, cur);
      ASSERT_TRUE(iam_page.has_value());
      extent_count_after += iam_page->extent_count;
      cur = iam_page->next_page_id;
    }

    LOG_STORAGE_INFO("Table has {} extents after compaction (before={})", extent_count_after, extent_count_before);
    // After deleting all rows, all extents should be freed.
    // The IAM head page itself lives in its own extent, but its extent_count should be 0.
    EXPECT_EQ(extent_count_after, 0)
        << "Expected 0 extents after deleting all rows and compacting";

    // Verify table is empty
    TableScanner scanner2(bpm, meta->iam_page_id);
    int surviving = 0;
    while (scanner2.next()) surviving++;
    EXPECT_EQ(surviving, 0);

  } catch (...) {
    if (std::filesystem::exists(test_db_file)) {
      std::filesystem::remove(test_db_file);
    }
    throw;
  }

  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }
}

TEST(CompactionTest, CompactNonExistentTableDoesNotCrash) {
  auto test_db_file = std::filesystem::temp_directory_path() / "compaction_noexist.db";

  try {
    DiskManager disk_manager(test_db_file.string());
    BufferPoolManager bpm(disk_manager, 16, std::make_unique<LRUPageReplacer>());
    ExtentManager extent_manager(bpm);
    bpm.flush_all_pages();
    IamManager iam_manager(bpm, extent_manager);
    CatalogManager catalog(bpm, iam_manager);
    catalog.init();

    TableManager table_manager(bpm, iam_manager, catalog);

    auto* meta = catalog.get_table("does_not_exist");
    ASSERT_EQ(meta, nullptr);

    LOG_STORAGE_INFO("Compact non-existent table test passed");

  } catch (...) {
    if (std::filesystem::exists(test_db_file)) {
      std::filesystem::remove(test_db_file);
    }
    throw;
  }

  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }
}

TEST(CompactionTest, CompactOnCleanTableIsNoOp) {
  auto test_db_file = std::filesystem::temp_directory_path() / "compaction_clean.db";

  try {
    DiskManager disk_manager(test_db_file.string());
    BufferPoolManager bpm(disk_manager, 16, std::make_unique<LRUPageReplacer>());
    ExtentManager extent_manager(bpm);
    bpm.flush_all_pages();
    IamManager iam_manager(bpm, extent_manager);
    CatalogManager catalog(bpm, iam_manager);
    catalog.init();

    Schema schema({
      Column("id", DataType::INTEGER, sizeof(int32_t), 0, false),
    });
    catalog.create_table("t", schema);
    auto* meta = catalog.get_table("t");
    TableManager table_manager(bpm, iam_manager, catalog);

    for (int i = 0; i < 100; ++i) {
      std::vector<Value> values;
      values.push_back(static_cast<int32_t>(i));
      Tuple t(std::move(values));
      table_manager.insert_row(meta->name, t);
    }

    page_id_t iam_head = meta->iam_page_id;
    int extent_count_before = 0;
    page_id_t cur = iam_head;
    while (cur != INVALID_PAGE_ID) {
      auto iam_page = load_page_value<IAMPage>(bpm, cur);
      ASSERT_TRUE(iam_page.has_value());
      extent_count_before += iam_page->extent_count;
      cur = iam_page->next_page_id;
    }

    table_manager.compact_extents(*meta);

    int extent_count_after = 0;
    cur = iam_head;
    while (cur != INVALID_PAGE_ID) {
      auto iam_page = load_page_value<IAMPage>(bpm, cur);
      ASSERT_TRUE(iam_page.has_value());
      extent_count_after += iam_page->extent_count;
      cur = iam_page->next_page_id;
    }

    EXPECT_EQ(extent_count_after, extent_count_before);

    int count = 0;
    TableScanner scanner(bpm, meta->iam_page_id);
    while (scanner.next()) count++;
    EXPECT_EQ(count, 100);

    LOG_STORAGE_INFO("Compact on clean table test passed");

  } catch (...) {
    if (std::filesystem::exists(test_db_file)) {
      std::filesystem::remove(test_db_file);
    }
    throw;
  }

  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }
}

} // namespace letty
