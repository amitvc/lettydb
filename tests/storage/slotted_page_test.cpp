#include <gtest/gtest.h>
#include "storage/slotted_page.h"
#include "storage/storage_def.h"

namespace letty {

class SlottedPageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::memset(buffer, 0, PAGE_SIZE);
  }

  char buffer[PAGE_SIZE];
};

TEST_F(SlottedPageTest, Initialization) {
  SlottedPage page = SlottedPage::init(buffer);

  EXPECT_EQ(page.get_num_slots(), 0);
  EXPECT_GT(page.get_free_space(), 4000);
  EXPECT_LT(page.get_free_space(), 4096);
}

TEST_F(SlottedPageTest, InsertAndGetTuple) {
  SlottedPage page = SlottedPage::init(buffer);

  std::string data1 = "Hello World";
  auto slot_id = page.insert_tuple(data1.c_str(), data1.size() + 1);

  ASSERT_TRUE(slot_id.has_value());
  EXPECT_EQ(page.get_num_slots(), 1);

  uint32_t size;
  const char* retrieved_data = page.get_tuple(*slot_id, &size);

  ASSERT_NE(retrieved_data, nullptr);
  EXPECT_EQ(size, data1.size() + 1);
  EXPECT_STREQ(retrieved_data, data1.c_str());
}

TEST_F(SlottedPageTest, MultipleInserts) {
  SlottedPage page = SlottedPage::init(buffer);

  std::vector<std::string> strings = {"One", "Two", "Three", "Four"};
  std::vector<uint16_t> slot_ids;

  for (const auto& s : strings) {
    auto id = page.insert_tuple(s.c_str(), s.size() + 1);
    ASSERT_TRUE(id.has_value());
    slot_ids.push_back(*id);
  }

  EXPECT_EQ(page.get_num_slots(), 4);

  for (int i = 0; i < 4; ++i) {
    uint32_t size;
    const char* data = page.get_tuple(slot_ids[i], &size);
    EXPECT_STREQ(data, strings[i].c_str());
  }
}

TEST_F(SlottedPageTest, PageFull) {
  SlottedPage page = SlottedPage::init(buffer);

  char huge_data[4000];
  std::memset(huge_data, 'A', 4000);

  auto id1 = page.insert_tuple(huge_data, 4000);
  ASSERT_TRUE(id1.has_value());

  // Should fail — no space left
  auto id2 = page.insert_tuple(huge_data, 100);
  EXPECT_FALSE(id2.has_value());
}

TEST_F(SlottedPageTest, SlotReuse) {
  SlottedPage page = SlottedPage::init(buffer);

  auto id1 = page.insert_tuple("Tuple 1", 8);
  auto id2 = page.insert_tuple("Tuple 2", 8);
  auto id3 = page.insert_tuple("Tuple 3", 8);

  ASSERT_TRUE(id1.has_value());
  ASSERT_TRUE(id2.has_value());
  ASSERT_TRUE(id3.has_value());
  EXPECT_EQ(*id1, 0);
  EXPECT_EQ(*id2, 1);
  EXPECT_EQ(*id3, 2);
  EXPECT_EQ(page.get_num_slots(), 3);

  // Simulate deletion by zeroing slot 1's length directly
  // (delete_tuple was removed — this tests the slot-reuse path)
  auto* slots = reinterpret_cast<Slot*>(buffer + sizeof(SlottedPageHeader));
  slots[1].length = 0;

  // Insert should reuse slot 1
  auto id4 = page.insert_tuple("Tuple 4", 8);
  ASSERT_TRUE(id4.has_value());
  EXPECT_EQ(*id4, 1);

  EXPECT_EQ(page.get_num_slots(), 3); // Should not increase

  uint32_t size;
  const char* data = page.get_tuple(*id4, &size);
  EXPECT_STREQ(data, "Tuple 4");
}

}
