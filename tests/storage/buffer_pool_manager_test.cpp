#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <memory>
#include <cstring>

#include "buffer/buffer_pool_manager.h"
#include "buffer/lru_replacer.h"
#include "storage/disk_manager.h"

using namespace letty;


class MockDiskManager : public IDiskManager {
 public:
  MOCK_METHOD(IOResult, write_page, (page_id_t page_id, const char* data), (override));
  MOCK_METHOD(IOResult, read_page, (page_id_t page_id, char* data), (override));
  MOCK_METHOD(page_id_t, get_file_size_in_pages, (), (override));
  MOCK_METHOD(bool, sync, (), (override));
};


class BufferPoolManagerTest : public ::testing::Test {
 protected:
  static constexpr size_t POOL_SIZE = 4;

  MockDiskManager mock_disk_;

  std::unique_ptr<BufferPoolManager> CreateBPM() {
    return std::make_unique<BufferPoolManager>(
        mock_disk_, POOL_SIZE, std::make_unique<LRUPageReplacer>());
  }
};


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

  EXPECT_TRUE(bpm->flush_all_pages());
}

TEST_F(BufferPoolManagerTest, FlushAllPagesReturnsFalseAndKeepsFailedPageDirty) {
  auto bpm = CreateBPM();

  EXPECT_CALL(mock_disk_, read_page(1, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));

  Page* page = bpm->fetch_page(1);
  ASSERT_NE(page, nullptr);
  bpm->unpin_page(1, true);

  EXPECT_CALL(mock_disk_, write_page(1, testing::_))
      .WillOnce(testing::Return(IOResult::WRITE_ERROR))
      .WillRepeatedly(testing::Return(IOResult::SUCCESS));

  EXPECT_FALSE(bpm->flush_all_pages());
  EXPECT_TRUE(page->is_dirty());
}

TEST_F(BufferPoolManagerTest, CheckpointReturnsTrueWhenFlushAndSyncSucceed) {
  auto bpm = CreateBPM();

  EXPECT_CALL(mock_disk_, read_page(1, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));

  Page* page = bpm->fetch_page(1);
  ASSERT_NE(page, nullptr);
  bpm->unpin_page(1, true);

  EXPECT_CALL(mock_disk_, write_page(1, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));
  EXPECT_CALL(mock_disk_, sync())
      .Times(testing::AtLeast(1))
      .WillRepeatedly(testing::Return(true));

  EXPECT_TRUE(bpm->checkpoint());
}

TEST_F(BufferPoolManagerTest, CheckpointReturnsFalseWhenSyncFails) {
  auto bpm = CreateBPM();

  EXPECT_CALL(mock_disk_, sync())
      .Times(testing::AtLeast(1))
      .WillRepeatedly(testing::Return(false));

  EXPECT_FALSE(bpm->checkpoint());
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

  EXPECT_TRUE(bpm->flush_all_pages());

  auto stats = bpm->get_cache_stats();
  EXPECT_EQ(stats.flushes, 3);
}

// ─── PageGuard RAII Tests ──────────────────────────────────────────

TEST_F(BufferPoolManagerTest, PageGuardUnpinsOnDestruction) {
  auto bpm = CreateBPM();

  EXPECT_CALL(mock_disk_, read_page(10, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));

  {
    PageGuard guard = bpm->fetch_page_guard(10);
    ASSERT_TRUE(guard.is_valid());
    EXPECT_EQ(guard.get_page_id(), 10);
    EXPECT_EQ(guard.get_page()->get_pin_count(), 1);
  }

  // After guard scope exits, page should be unpinned (pin count 0)
  Page* page = bpm->fetch_page(10);
  ASSERT_NE(page, nullptr);
  EXPECT_EQ(page->get_pin_count(), 1);
  bpm->unpin_page(10, false);
}

TEST_F(BufferPoolManagerTest, PageGuardMarksDirtyExplicitly) {
  auto bpm = CreateBPM();

  EXPECT_CALL(mock_disk_, read_page(12, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));
  EXPECT_CALL(mock_disk_, write_page(12, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));

  {
    PageGuard guard = bpm->fetch_page_guard(12);
    char* data = guard.get_data();
    EXPECT_FALSE(guard.is_dirty());
    std::memcpy(data, "GUARD", 5);
    guard.mark_dirty();
    EXPECT_TRUE(guard.is_dirty());
  }

  EXPECT_TRUE(bpm->flush_page(12));
}

TEST_F(BufferPoolManagerTest, PageGuardMoveSemantics) {
  auto bpm = CreateBPM();

  EXPECT_CALL(mock_disk_, read_page(15, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));

  PageGuard guard2;
  {
    PageGuard guard1 = bpm->fetch_page_guard(15);
    EXPECT_TRUE(guard1.is_valid());
    guard2 = std::move(guard1);
    EXPECT_FALSE(guard1.is_valid());
    EXPECT_TRUE(guard2.is_valid());
    EXPECT_EQ(guard2.get_page_id(), 15);
  }

  EXPECT_TRUE(guard2.is_valid());
  EXPECT_EQ(guard2.get_page()->get_pin_count(), 1);
  guard2.drop();
  EXPECT_FALSE(guard2.is_valid());
}

TEST_F(BufferPoolManagerTest, TrackDirtiedPageRecordsLSN) {
  auto bpm = CreateBPM();

  constexpr lsn_t LSN_100 = 100;
  bpm->update_dirtied_page_lsn(42, LSN_100);

  auto dpt = bpm->get_dirty_page_table();
  EXPECT_EQ(dpt.size(), 1);
  EXPECT_EQ(dpt.at(42), LSN_100);
}

TEST_F(BufferPoolManagerTest, TrackDirtiedPageFirstWinsSemantics) {
  auto bpm = CreateBPM();

  constexpr lsn_t LSN_100 = 100;
  constexpr lsn_t LSN_200 = 200;

  bpm->update_dirtied_page_lsn(42, LSN_100);
  bpm->update_dirtied_page_lsn(42, LSN_200);

  auto dpt = bpm->get_dirty_page_table();
  EXPECT_EQ(dpt.size(), 1);
  EXPECT_EQ(dpt.at(42), LSN_100);
}

TEST_F(BufferPoolManagerTest, FlushPageRemovesDPTEntry) {
  auto bpm = CreateBPM();

  EXPECT_CALL(mock_disk_, read_page(5, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));

  Page* page = bpm->fetch_page(5);
  bpm->unpin_page(5, true);

  constexpr lsn_t LSN_100 = 100;
  bpm->update_dirtied_page_lsn(5, LSN_100);

  auto dpt_before = bpm->get_dirty_page_table();
  EXPECT_EQ(dpt_before.size(), 1);
  EXPECT_EQ(dpt_before.at(5), LSN_100);

  EXPECT_CALL(mock_disk_, write_page(5, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));

  bpm->flush_page(5);

  auto dpt_after = bpm->get_dirty_page_table();
  EXPECT_EQ(dpt_after.size(), 0);  // entry removed after flush
}

TEST_F(BufferPoolManagerTest, FlushAllPagesRemovesAllDPTEntries) {
  auto bpm = CreateBPM();

  for (int i = 0; i < 3; ++i) {
    EXPECT_CALL(mock_disk_, read_page(i, testing::_))
        .WillOnce(testing::Return(IOResult::SUCCESS));
    Page* page = bpm->fetch_page(i);
    bpm->unpin_page(i, true);
  }

  bpm->update_dirtied_page_lsn(0, 100);
  bpm->update_dirtied_page_lsn(1, 200);
  bpm->update_dirtied_page_lsn(2, 300);

  EXPECT_EQ(bpm->get_dirty_page_table().size(), 3);

  EXPECT_CALL(mock_disk_, write_page(testing::_, testing::_))
      .Times(3)
      .WillRepeatedly(testing::Return(IOResult::SUCCESS));

  bpm->flush_all_pages();

  auto dpt = bpm->get_dirty_page_table();
  EXPECT_EQ(dpt.size(), 0);  // all entries removed
}

TEST_F(BufferPoolManagerTest, DeletePageRemovesDPTEntry) {
  auto bpm = CreateBPM();

  EXPECT_CALL(mock_disk_, read_page(7, testing::_))
      .WillOnce(testing::Return(IOResult::SUCCESS));

  bpm->fetch_page(7);
  bpm->unpin_page(7, false);

  bpm->update_dirtied_page_lsn(7, 500);
  EXPECT_EQ(bpm->get_dirty_page_table().size(), 1);

  bpm->delete_page(7);

  auto dpt = bpm->get_dirty_page_table();
  EXPECT_EQ(dpt.size(), 0);  // entry removed on delete
}