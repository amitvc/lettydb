#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <memory>
#include <cstring>

#include "buffer/buffer_pool_manager.h"
#include "buffer/lru_replacer.h"
#include "storage/disk_manager.h"

using namespace letty;

// ─── Mock DiskManager ──────────────────────────────────────────────

class MockDiskManager : public IDiskManager {
 public:
  MOCK_METHOD(IOResult, write_page, (page_id_t page_id, const char* data), (override));
  MOCK_METHOD(IOResult, read_page, (page_id_t page_id, char* data), (override));
  MOCK_METHOD(page_id_t, get_file_size_in_pages, (), (override));
};

// ─── Test Fixture ──────────────────────────────────────────────────

class BufferPoolManagerTest : public ::testing::Test {
 protected:
  static constexpr size_t POOL_SIZE = 4;

  MockDiskManager mock_disk_;

  std::unique_ptr<BufferPoolManager> CreateBPM() {
    return std::make_unique<BufferPoolManager>(
        mock_disk_, POOL_SIZE, std::make_unique<LRUPageReplacer>());
  }
};

// ─── Basic Fetch / Unpin ───────────────────────────────────────────

TEST_F(BufferPoolManagerTest, FetchPageLoadsFromDisk) {
  auto bpm = CreateBPM();

  EXPECT_CALL(mock_disk_, read_page(5, testing::_))
      .WillOnce([](page_id_t, char* data) {
        std::memcpy(data, "HELLO", 5);
        return IOResult::SUCCESS;
      });

  Page* page = bpm->fetch_page(5);
  ASSERT_NE(page, nullptr);
  EXPECT_EQ(page->get_page_id(), 5);
  EXPECT_EQ(page->get_pin_count(), 1);
  EXPECT_FALSE(page->is_dirty());
  EXPECT_EQ(std::memcmp(page->get_data(), "HELLO", 5), 0);

  bpm->unpin_page(5, false);
}

TEST_F(BufferPoolManagerTest, CacheHitReturnsSamePage) {
  auto bpm = CreateBPM();

  EXPECT_CALL(mock_disk_, read_page(5, testing::_))
      .Times(1)  // only ONE disk read
      .WillOnce([](page_id_t, char* data) {
        std::memcpy(data, "DATA", 4);
        return IOResult::SUCCESS;
      });

  Page* p1 = bpm->fetch_page(5);
  Page* p2 = bpm->fetch_page(5);  // cache hit

  EXPECT_EQ(p1, p2);
  EXPECT_EQ(p1->get_pin_count(), 2);

  bpm->unpin_page(5, false);
  bpm->unpin_page(5, false);
}

TEST_F(BufferPoolManagerTest, UnpinNonExistentPageReturnsFalse) {
  auto bpm = CreateBPM();
  EXPECT_FALSE(bpm->unpin_page(99, false));
}

TEST_F(BufferPoolManagerTest, UnpinSetsDirtyFlag) {
  auto bpm = CreateBPM();

  EXPECT_CALL(mock_disk_, read_page(1, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));

  Page* page = bpm->fetch_page(1);
  EXPECT_FALSE(page->is_dirty());

  bpm->unpin_page(1, true);  // mark dirty
  EXPECT_TRUE(page->is_dirty());
}

TEST_F(BufferPoolManagerTest, DirtyFlagSticksAcrossUnpins) {
  auto bpm = CreateBPM();

  EXPECT_CALL(mock_disk_, read_page(1, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));

  bpm->fetch_page(1);
  bpm->fetch_page(1);  // pin_count = 2

  bpm->unpin_page(1, true);   // dirty = true
  bpm->unpin_page(1, false);  // dirty should STAY true

  Page* page = bpm->fetch_page(1);
  EXPECT_TRUE(page->is_dirty());

  bpm->unpin_page(1, false);
}

// ─── Eviction ──────────────────────────────────────────────────────

TEST_F(BufferPoolManagerTest, EvictsLRUWhenPoolFull) {
  auto bpm = CreateBPM();

  // Fill all 4 frames
  for (int i = 0; i < 4; ++i) {
    EXPECT_CALL(mock_disk_, read_page(i, testing::_))
        .WillOnce(testing::Return(IOResult::SUCCESS));
    Page* p = bpm->fetch_page(i);
    bpm->unpin_page(i, false);
  }

  // Fetch page 10 — must evict page 0 (oldest unpinned)
  EXPECT_CALL(mock_disk_, read_page(10, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));

  Page* page = bpm->fetch_page(10);
  ASSERT_NE(page, nullptr);
  EXPECT_EQ(page->get_page_id(), 10);

  bpm->unpin_page(10, false);
}

TEST_F(BufferPoolManagerTest, DirtyEvictionWritesToDisk) {
  auto bpm = CreateBPM();

  // Fill all 4 frames, mark page 0 as dirty
  for (int i = 0; i < 4; ++i) {
    EXPECT_CALL(mock_disk_, read_page(i, testing::_))
        .WillOnce([i](page_id_t, char* data) {
          data[0] = static_cast<char>(i);
          return IOResult::SUCCESS;
        });
    bpm->fetch_page(i);
    bpm->unpin_page(i, i == 0);  // only page 0 is dirty
  }

  // Evicting page 0 should trigger a write
  EXPECT_CALL(mock_disk_, write_page(0, testing::_))
      .Times(1)
      .WillOnce(testing::Return(IOResult::SUCCESS));
  EXPECT_CALL(mock_disk_, read_page(10, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));

  Page* page = bpm->fetch_page(10);
  ASSERT_NE(page, nullptr);

  bpm->unpin_page(10, false);
}

TEST_F(BufferPoolManagerTest, PoolFullAllPinnedReturnsNullptr) {
  auto bpm = CreateBPM();

  // Fill all 4 frames and keep them pinned
  for (int i = 0; i < 4; ++i) {
    EXPECT_CALL(mock_disk_, read_page(i, testing::_))
        .WillOnce(testing::Return(IOResult::SUCCESS));
    bpm->fetch_page(i);
    // don't unpin!
  }

  // 5th fetch should fail — no evictable frames
  Page* page = bpm->fetch_page(10);
  EXPECT_EQ(page, nullptr);

  // Clean up pins
  for (int i = 0; i < 4; ++i) {
    bpm->unpin_page(i, false);
  }
}

// ─── Pin Count ─────────────────────────────────────────────────────

TEST_F(BufferPoolManagerTest, PinCountTracksCorrectly) {
  auto bpm = CreateBPM();

  EXPECT_CALL(mock_disk_, read_page(1, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));

  Page* page = bpm->fetch_page(1);
  EXPECT_EQ(page->get_pin_count(), 1);

  bpm->fetch_page(1);
  EXPECT_EQ(page->get_pin_count(), 2);

  bpm->fetch_page(1);
  EXPECT_EQ(page->get_pin_count(), 3);

  bpm->unpin_page(1, false);
  EXPECT_EQ(page->get_pin_count(), 2);

  bpm->unpin_page(1, false);
  EXPECT_EQ(page->get_pin_count(), 1);

  bpm->unpin_page(1, false);
  EXPECT_EQ(page->get_pin_count(), 0);
}

// ─── New Page ──────────────────────────────────────────────────────

TEST_F(BufferPoolManagerTest, NewPageCreatesInPool) {
  auto bpm = CreateBPM();

  Page* page = bpm->new_page(42);
  ASSERT_NE(page, nullptr);
  EXPECT_EQ(page->get_page_id(), 42);
  EXPECT_EQ(page->get_pin_count(), 1);
  EXPECT_TRUE(page->is_dirty());  // new page is dirty (not on disk yet)

  // Data should be zeroed
  for (int i = 0; i < PAGE_SIZE; ++i) {
    EXPECT_EQ(page->get_data()[i], 0) << "Byte " << i << " not zero";
  }

  bpm->unpin_page(42, false);
}

TEST_F(BufferPoolManagerTest, NewPageDuplicateReturnsNullptr) {
  auto bpm = CreateBPM();

  Page* p1 = bpm->new_page(42);
  ASSERT_NE(p1, nullptr);

  Page* p2 = bpm->new_page(42);  // duplicate
  EXPECT_EQ(p2, nullptr);

  bpm->unpin_page(42, false);
}

// ─── Flush ─────────────────────────────────────────────────────────

TEST_F(BufferPoolManagerTest, FlushPageWritesDirtyToDisk) {
  auto bpm = CreateBPM();

  EXPECT_CALL(mock_disk_, read_page(1, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));

  Page* page = bpm->fetch_page(1);
  std::memcpy(page->get_data(), "MODIFIED", 8);
  bpm->unpin_page(1, true);

  EXPECT_CALL(mock_disk_, write_page(1, testing::_))
      .Times(1)
      .WillOnce(testing::Return(IOResult::SUCCESS));

  EXPECT_TRUE(bpm->flush_page(1));
  EXPECT_FALSE(page->is_dirty());  // cleared after flush
}

TEST_F(BufferPoolManagerTest, FlushPageNonExistentReturnsFalse) {
  auto bpm = CreateBPM();
  EXPECT_FALSE(bpm->flush_page(99));
}

TEST_F(BufferPoolManagerTest, FlushCleanPageSkipsWrite) {
  auto bpm = CreateBPM();

  EXPECT_CALL(mock_disk_, read_page(1, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));
  EXPECT_CALL(mock_disk_, write_page(testing::_, testing::_)).Times(0);

  bpm->fetch_page(1);
  bpm->unpin_page(1, false);  // not dirty

  EXPECT_TRUE(bpm->flush_page(1));  // should NOT write
}

TEST_F(BufferPoolManagerTest, FlushAllPagesWritesAllDirty) {
  auto bpm = CreateBPM();

  // Create 3 pages: pages 0,1 dirty, page 2 clean
  for (int i = 0; i < 3; ++i) {
    EXPECT_CALL(mock_disk_, read_page(i, testing::_))
        .WillOnce(testing::Return(IOResult::SUCCESS));
    bpm->fetch_page(i);
    bpm->unpin_page(i, i < 2);  // 0,1 dirty; 2 clean
  }

  EXPECT_CALL(mock_disk_, write_page(0, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));
  EXPECT_CALL(mock_disk_, write_page(1, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));
  EXPECT_CALL(mock_disk_, write_page(2, testing::_)).Times(0);

  bpm->flush_all_pages();
}

// ─── Delete Page ───────────────────────────────────────────────────

TEST_F(BufferPoolManagerTest, DeletePageFreesFrame) {
  auto bpm = CreateBPM();

  EXPECT_CALL(mock_disk_, read_page(1, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));

  bpm->fetch_page(1);
  bpm->unpin_page(1, false);

  EXPECT_TRUE(bpm->delete_page(1));

  // Page 1 should no longer be in the pool — fetching it reads from disk again
  EXPECT_CALL(mock_disk_, read_page(1, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));

  Page* page = bpm->fetch_page(1);
  ASSERT_NE(page, nullptr);

  bpm->unpin_page(1, false);
}

TEST_F(BufferPoolManagerTest, DeletePinnedPageFails) {
  auto bpm = CreateBPM();

  EXPECT_CALL(mock_disk_, read_page(1, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));

  bpm->fetch_page(1);  // pinned

  EXPECT_FALSE(bpm->delete_page(1));  // can't delete while pinned

  bpm->unpin_page(1, false);
}

TEST_F(BufferPoolManagerTest, DeleteNonExistentPageReturnsFalse) {
  auto bpm = CreateBPM();
  EXPECT_FALSE(bpm->delete_page(99));
}

// ─── Destructor ────────────────────────────────────────────────────

TEST_F(BufferPoolManagerTest, DestructorFlushesDirtyPages) {
  auto bpm = CreateBPM();

  EXPECT_CALL(mock_disk_, read_page(1, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));

  bpm->fetch_page(1);
  bpm->unpin_page(1, true);  // dirty

  // When bpm is destroyed, it should flush dirty pages
  EXPECT_CALL(mock_disk_, write_page(1, testing::_))
      .Times(1)
      .WillOnce(testing::Return(IOResult::SUCCESS));

  bpm.reset();  // triggers destructor
}

// ─── Cache Stats / Observability ───────────────────────────────────

TEST_F(BufferPoolManagerTest, HitRatioAfterRepeatedFetches) {
  auto bpm = CreateBPM();

  EXPECT_CALL(mock_disk_, read_page(1, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));

  // First fetch = miss
  Page* page = bpm->fetch_page(1);
  bpm->unpin_page(1, false);

  // 9 more fetches = all hits
  for (int i = 0; i < 9; ++i) {
    page = bpm->fetch_page(1);
    bpm->unpin_page(1, false);
  }

  auto stats = bpm->get_cache_stats();
  EXPECT_EQ(stats.hits, 9);
  EXPECT_EQ(stats.misses, 1);
  EXPECT_DOUBLE_EQ(stats.hit_ratio(), 90.0);
}

TEST_F(BufferPoolManagerTest, ResetStatsZeroesAllCounters) {
  auto bpm = CreateBPM();

  EXPECT_CALL(mock_disk_, read_page(1, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));

  bpm->fetch_page(1);
  bpm->unpin_page(1, false);

  auto stats = bpm->get_cache_stats();
  EXPECT_GT(stats.misses, 0u);

  bpm->reset_cache_stats();
  stats = bpm->get_cache_stats();
  EXPECT_EQ(stats.hits, 0);
  EXPECT_EQ(stats.misses, 0);
  EXPECT_EQ(stats.evictions, 0);
  EXPECT_EQ(stats.dirty_evictions, 0);
  EXPECT_EQ(stats.flushes, 0);
}

TEST_F(BufferPoolManagerTest, DirtyEvictionIsTracked) {
  auto bpm = CreateBPM();  // pool_size = 4

  // Fill pool with pages 0-3
  for (page_id_t i = 0; i < 4; ++i) {
    EXPECT_CALL(mock_disk_, read_page(i, testing::_))
        .WillOnce(testing::Return(IOResult::SUCCESS));
    bpm->fetch_page(i);
    bpm->unpin_page(i, i == 0);  // page 0 is dirty
  }

  // Fetch page 10 — triggers eviction of page 0 (dirty)
  EXPECT_CALL(mock_disk_, read_page(10, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));
  EXPECT_CALL(mock_disk_, write_page(0, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));

  Page* page = bpm->fetch_page(10);
  ASSERT_NE(page, nullptr);
  bpm->unpin_page(10, false);

  auto stats = bpm->get_cache_stats();
  EXPECT_EQ(stats.evictions, 1);
  EXPECT_EQ(stats.dirty_evictions, 1);
}

TEST_F(BufferPoolManagerTest, FlushCountIsTracked) {
  auto bpm = CreateBPM();

  EXPECT_CALL(mock_disk_, read_page(1, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));

  bpm->fetch_page(1);
  bpm->unpin_page(1, true);  // dirty

  EXPECT_CALL(mock_disk_, write_page(1, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));

  bpm->flush_page(1);

  auto stats = bpm->get_cache_stats();
  EXPECT_EQ(stats.flushes, 1);
}

TEST_F(BufferPoolManagerTest, FlushAllCountsEachDirtyPage) {
  auto bpm = CreateBPM();

  for (page_id_t i = 0; i < 3; ++i) {
    EXPECT_CALL(mock_disk_, read_page(i, testing::_))
        .WillOnce(testing::Return(IOResult::SUCCESS));
    EXPECT_CALL(mock_disk_, write_page(i, testing::_))
        .WillOnce(testing::Return(IOResult::SUCCESS));
    bpm->fetch_page(i);
    bpm->unpin_page(i, true);  // all dirty
  }

  bpm->flush_all_pages();

  auto stats = bpm->get_cache_stats();
  EXPECT_EQ(stats.flushes, 3);
}
