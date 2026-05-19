#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <cstring>
#include <memory>

#include "storage/page.h"
#include "buffer/buffer_pool_manager.h"
#include "buffer/lru_replacer.h"
#include "storage/disk_manager.h"

using namespace letty;

class MockDiskManager : public IDiskManager {
 public:
  MOCK_METHOD(IOResult, write_page, (page_id_t page_id, const char* data), (override));
  MOCK_METHOD(IOResult, read_page, (page_id_t page_id, char* data), (override));
  MOCK_METHOD(page_id_t, get_file_size_in_pages, (), (override));
};

class PageTest : public ::testing::Test {
 protected:
  MockDiskManager mock_disk_;

  std::unique_ptr<BufferPoolManager> make_bpm(size_t pool_size = 4) {
    EXPECT_CALL(mock_disk_, get_file_size_in_pages()).WillRepeatedly(::testing::Return(0));
    EXPECT_CALL(mock_disk_, write_page(::testing::_, ::testing::_))
        .WillRepeatedly(::testing::Return(IOResult::SUCCESS));
    return std::make_unique<BufferPoolManager>(
        mock_disk_, pool_size,
        std::make_unique<LRUPageReplacer>());
  }
};

TEST_F(PageTest, DefaultConstructedState) {
  Page page;
  EXPECT_EQ(page.get_page_id(), INVALID_PAGE_ID);
  EXPECT_EQ(page.get_pin_count(), 0);
  EXPECT_FALSE(page.is_dirty());
}

TEST_F(PageTest, DataIsZeroInitialized) {
  auto bpm = make_bpm();
  Page* page = bpm->new_page(0);
  ASSERT_NE(page, nullptr);
  const char* data = page->get_data();
  for (int i = 0; i < PAGE_SIZE; ++i) {
    EXPECT_EQ(data[i], 0) << "Byte " << i << " was not zero";
  }
  bpm->unpin_page(0, false);
}

TEST_F(PageTest, DataIsWritableAndReadable) {
  auto bpm = make_bpm();
  Page* page = bpm->new_page(0);
  ASSERT_NE(page, nullptr);
  char* data = page->get_data();

  const char* message = "LETTYDB";
  std::memcpy(data, message, 7);

  EXPECT_EQ(std::memcmp(page->get_data(), "LETTYDB", 7), 0);
  bpm->unpin_page(0, false);
}

TEST_F(PageTest, DataBufferIsPageAligned) {
  auto bpm = make_bpm();
  Page* page = bpm->new_page(0);
  ASSERT_NE(page, nullptr);
  void* data = page->get_data();
  size_t space = PAGE_SIZE;
  void* aligned = std::align(PAGE_SIZE, 1, data, space);
  EXPECT_EQ(aligned, page->get_data()) << "data_ buffer is not aligned to PAGE_SIZE";
  bpm->unpin_page(0, false);
}
