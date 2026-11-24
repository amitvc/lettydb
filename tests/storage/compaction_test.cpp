#include <gtest/gtest.h>
#include <filesystem>
#include <vector>

#include "buffer/buffer_pool_manager.h"
#include "buffer/lru_replacer.h"
#include "catalog/catalog_manager.h"
#include "catalog/column.h"
#include "catalog/schema.h"
#include "common/logger.h"
#include "storage/disk_manager.h"
#include "storage/log_manager.h"
#include "storage/page_allocator.h"
#include "storage/page_utils.h"
#include "storage/rollback_manager.h"
#include "storage/slotted_page.h"
#include "storage/storage_def.h"
#include "storage/table_manager.h"
#include "storage/table_scanner.h"
#include "storage/tuple.h"
#include "transaction/transaction_manager.h"

namespace letty {

TEST(CompactionTest, DeleteAllAndCompactFreesEmptyPages) {
  auto test_db_file = std::filesystem::temp_directory_path() / "compaction_test.db";

  try {
    DiskManager disk_manager(test_db_file.string());
    BufferPoolManager bpm(disk_manager, 64, std::make_unique<LRUPageReplacer>());
    LogManager log_manager(test_db_file.string() + ".wal");
    RollbackManager rollback_manager(log_manager, bpm, test_db_file.string());
    TransactionManager txn_manager(log_manager, rollback_manager);
    PageAllocator page_allocator(bpm, log_manager, txn_manager);
    CatalogManager catalog(bpm, page_allocator, txn_manager);
    catalog.init();

    Schema schema({
      Column("id", DataType::INTEGER, sizeof(int32_t), 0, false),
    });

    catalog.create_table("test", schema);
    auto meta = catalog.get_table("test");
    ASSERT_TRUE(meta.has_value());

    TableManager table_manager(bpm, catalog, log_manager, txn_manager, page_allocator);

    const int kRowCount = 500;
    uint64_t txn_id = txn_manager.begin();
    for (int i = 0; i < kRowCount; ++i) {
      std::vector<Value> values;
      values.push_back(static_cast<int32_t>(i));
      Tuple t(std::move(values));
      table_manager.insert_row(meta->get().name, t, txn_id);
    }
    txn_manager.commit(txn_id);

    int page_count_before = 0;
    page_id_t cur = meta->get().first_page_id;
    while (cur != INVALID_PAGE_ID) {
      auto page_header = load_page_value<PageHeader>(bpm, cur);
      ASSERT_TRUE(page_header.has_value());
      page_count_before++;
      cur = page_header->next_page_id;
    }
    EXPECT_GT(page_count_before, 1);

    uint64_t delete_txn_id = txn_manager.begin();
    TableScanner scanner = table_manager.scan_table(meta->get().name);
    int deleted = 0;
    while (scanner.next()) {
      table_manager.delete_row(meta->get(), scanner.current_page_id(), scanner.current_slot_id(), delete_txn_id);
      deleted++;
    }
    txn_manager.commit(delete_txn_id);
    EXPECT_EQ(deleted, kRowCount);

    CompactionStats stats = table_manager.compact_table(meta->get());
    EXPECT_GT(stats.pages_freed, 0u);

    int page_count_after = 0;
    cur = meta->get().first_page_id;
    while (cur != INVALID_PAGE_ID) {
      auto page_header = load_page_value<PageHeader>(bpm, cur);
      ASSERT_TRUE(page_header.has_value());
      page_count_after++;
      cur = page_header->next_page_id;
    }
    EXPECT_EQ(page_count_after, 1);

    TableScanner scanner2 = table_manager.scan_table(meta->get().name);
    int surviving = 0;
    while (scanner2.next()) surviving++;
    EXPECT_EQ(surviving, 0);

  } catch (...) {
    if (std::filesystem::exists(test_db_file)) {
      std::filesystem::remove(test_db_file);
    }
    std::filesystem::remove(test_db_file.string() + ".wal.0");
    throw;
  }

  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }
  std::filesystem::remove(test_db_file.string() + ".wal.0");
}

TEST(CompactionTest, CompactNonExistentTableDoesNotCrash) {
  auto test_db_file = std::filesystem::temp_directory_path() / "compaction_noexist.db";

  try {
    DiskManager disk_manager(test_db_file.string());
    BufferPoolManager bpm(disk_manager, 16, std::make_unique<LRUPageReplacer>());
    LogManager log_manager(test_db_file.string() + ".wal");
    RollbackManager rollback_manager(log_manager, bpm, test_db_file.string());
    TransactionManager txn_manager(log_manager, rollback_manager);
    PageAllocator page_allocator(bpm, log_manager, txn_manager);
    CatalogManager catalog(bpm, page_allocator, txn_manager);
    catalog.init();

    TableManager table_manager(bpm, catalog, log_manager, txn_manager, page_allocator);

    auto meta = catalog.get_table("does_not_exist");
    EXPECT_FALSE(meta.has_value());

  } catch (...) {
    if (std::filesystem::exists(test_db_file)) {
      std::filesystem::remove(test_db_file);
    }
    std::filesystem::remove(test_db_file.string() + ".wal.0");
    throw;
  }

  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }
  std::filesystem::remove(test_db_file.string() + ".wal.0");
}

TEST(CompactionTest, CompactOnCleanTableIsNoOp) {
  auto test_db_file = std::filesystem::temp_directory_path() / "compaction_clean.db";

  try {
    DiskManager disk_manager(test_db_file.string());
    BufferPoolManager bpm(disk_manager, 16, std::make_unique<LRUPageReplacer>());
    LogManager log_manager(test_db_file.string() + ".wal");
    RollbackManager rollback_manager(log_manager, bpm, test_db_file.string());
    TransactionManager txn_manager(log_manager, rollback_manager);
    PageAllocator page_allocator(bpm, log_manager, txn_manager);
    CatalogManager catalog(bpm, page_allocator, txn_manager);
    catalog.init();

    Schema schema({
      Column("id", DataType::INTEGER, sizeof(int32_t), 0, false),
    });
    catalog.create_table("t", schema);
    auto meta = catalog.get_table("t");
    ASSERT_TRUE(meta.has_value());
    TableManager table_manager(bpm, catalog, log_manager, txn_manager, page_allocator);

    uint64_t txn_id = txn_manager.begin();
    for (int i = 0; i < 100; ++i) {
      std::vector<Value> values;
      values.push_back(static_cast<int32_t>(i));
      Tuple t(std::move(values));
      table_manager.insert_row(meta->get().name, t, txn_id);
    }
    txn_manager.commit(txn_id);

    int page_count_before = 0;
    page_id_t cur = meta->get().first_page_id;
    while (cur != INVALID_PAGE_ID) {
      auto page_header = load_page_value<PageHeader>(bpm, cur);
      ASSERT_TRUE(page_header.has_value());
      page_count_before++;
      cur = page_header->next_page_id;
    }

    CompactionStats stats = table_manager.compact_table(meta->get());
    EXPECT_EQ(stats.pages_freed, 0u);
    EXPECT_EQ(stats.tuples_migrated, 0u);

    int page_count_after = 0;
    cur = meta->get().first_page_id;
    while (cur != INVALID_PAGE_ID) {
      auto page_header = load_page_value<PageHeader>(bpm, cur);
      ASSERT_TRUE(page_header.has_value());
      page_count_after++;
      cur = page_header->next_page_id;
    }

    EXPECT_EQ(page_count_after, page_count_before);

    int count = 0;
    TableScanner scanner = table_manager.scan_table(meta->get().name);
    while (scanner.next()) count++;
    EXPECT_EQ(count, 100);

  } catch (...) {
    if (std::filesystem::exists(test_db_file)) {
      std::filesystem::remove(test_db_file);
    }
    std::filesystem::remove(test_db_file.string() + ".wal.0");
    throw;
  }

  if (std::filesystem::exists(test_db_file)) {
    std::filesystem::remove(test_db_file);
  }
  std::filesystem::remove(test_db_file.string() + ".wal.0");
}

} // namespace letty
