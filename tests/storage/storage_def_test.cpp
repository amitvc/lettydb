#include "storage/storage_def.h"
#include "storage/slotted_page.h"

#include <gtest/gtest.h>
#include <cstring>
#include "storage/config.h"

namespace letty {

TEST(StorageDefTest, PageHeaderDefaultsAndSize) {
  EXPECT_EQ(sizeof(PageHeader), 20u);

  PageHeader header;
  EXPECT_EQ(header.page_lsn, 0u);
  EXPECT_EQ(header.next_page_id, INVALID_PAGE_ID);
  EXPECT_EQ(header.prev_page_id, INVALID_PAGE_ID);
  EXPECT_EQ(header.table_id, 0u);
  EXPECT_EQ(header.page_type, PageType::INVALID);
  EXPECT_EQ(header.reserved, 0u);
}

TEST(StorageDefTest, DatabaseMetadataDefaultsAndSize) {
  EXPECT_EQ(sizeof(DatabaseMetadata), 33u);

  DatabaseMetadata meta;
  std::memcpy(meta.signature, "LETTYDB\0", 8);
  EXPECT_STREQ(meta.signature, "LETTYDB");
  EXPECT_EQ(meta.version, 1u);
  EXPECT_EQ(meta.page_size, DEFAULT_PAGE_SIZE);
  EXPECT_EQ(meta.table_count, 0u);
  EXPECT_EQ(meta.next_table_id, 100u);
  EXPECT_EQ(meta.page_count, 1u);
  EXPECT_EQ(meta.free_page_list_head_page, INVALID_PAGE_ID);
  EXPECT_EQ(meta.sys_tables_first_page, 1);
  EXPECT_EQ(meta.sys_columns_first_page, 2);
}

TEST(StorageDefTest, SlotSizeAndDefaults) {
  EXPECT_EQ(sizeof(Slot), 4u);

  Slot slot{120, 48};
  EXPECT_EQ(slot.offset, 120u);
  EXPECT_EQ(slot.length, 48u);
}

}  // namespace letty
