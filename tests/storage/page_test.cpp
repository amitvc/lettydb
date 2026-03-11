#include <gtest/gtest.h>
#include "storage/page.h"

using namespace letty;

TEST(PageTest, DefaultConstructedState) {
  Page page;
  EXPECT_EQ(page.get_page_id(), INVALID_PAGE_ID);
  EXPECT_EQ(page.get_pin_count(), 0);
  EXPECT_FALSE(page.is_dirty());
}

TEST(PageTest, DataIsZeroInitialized) {
  Page page;
  const char* data = page.get_data();
  for (int i = 0; i < PAGE_SIZE; ++i) {
    EXPECT_EQ(data[i], 0) << "Byte " << i << " was not zero";
  }
}

TEST(PageTest, DataIsWritableAndReadable) {
  Page page;
  char* data = page.get_data();

  // Write a pattern
  const char* message = "LETTYDB";
  std::memcpy(data, message, 7);

  // Read it back
  EXPECT_EQ(std::memcmp(page.get_data(), "LETTYDB", 7), 0);
}

TEST(PageTest, DataBufferIsPageAligned) {
  Page page;
  auto addr = reinterpret_cast<std::uintptr_t>(page.get_data());
  EXPECT_EQ(addr % PAGE_SIZE, 0) << "data_ buffer is not aligned to PAGE_SIZE";
}
