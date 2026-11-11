#include "storage/log_manager.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <thread>

#include "common/db_exception.h"
#include "storage/config.h"

namespace letty {

// Test harness to access protected methods
class TestableLogManager : public LogManager {
 public:
  using LogManager::LogManager;
  using LogManager::list_segment_ids;
  using LogManager::find_latest_segment;
};

class LogManagerTest : public ::testing::Test {
 protected:
  static constexpr uint32_t TEST_SEGMENT_SIZE = 1024;

  void SetUp() override {
    test_dir_ = "test_wal_dir_" + std::to_string(test_counter_++);
    std::filesystem::remove_all(test_dir_);
    std::filesystem::create_directories(test_dir_);
    test_prefix_ = (test_dir_ / "letty.wal").string();
  }

  void TearDown() override {
    std::filesystem::remove_all(test_dir_);
  }

  std::string segment_path(uint32_t seg_id) const {
    return test_prefix_ + "." + std::to_string(seg_id);
  }

  void write_raw_segment(uint32_t seg_id, const std::string& content) {
    std::ofstream out(segment_path(seg_id), std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
  }

  void write_valid_segment(uint32_t seg_id, size_t payload_bytes) {
    std::string content(WAL_SIGNATURE, WAL_SIGNATURE_SIZE);
    content.append(payload_bytes, 'X');
    write_raw_segment(seg_id, content);
  }

  std::filesystem::path test_dir_;
  std::string test_prefix_;
  static int test_counter_;
};

int LogManagerTest::test_counter_ = 0;

TEST_F(LogManagerTest, CreatesSegmentZeroOnFreshDirectory) {
  ASSERT_FALSE(std::filesystem::exists(segment_path(0)));

  LogManager log_mgr(test_prefix_, TEST_SEGMENT_SIZE);

  EXPECT_TRUE(std::filesystem::exists(segment_path(0)));
  EXPECT_EQ(std::filesystem::file_size(segment_path(0)), WAL_SIGNATURE_SIZE);
  EXPECT_EQ(log_mgr.get_current_segment_id(), 0u);
  EXPECT_EQ(log_mgr.get_next_lsn(), WAL_SIGNATURE_SIZE);
  EXPECT_EQ(log_mgr.get_flushed_lsn(), WAL_SIGNATURE_SIZE);

  std::ifstream in(segment_path(0), std::ios::binary);
  char header[WAL_SIGNATURE_SIZE]{};
  in.read(header, WAL_SIGNATURE_SIZE);
  EXPECT_EQ(in.gcount(), static_cast<std::streamsize>(WAL_SIGNATURE_SIZE));
  EXPECT_EQ(std::memcmp(header, WAL_SIGNATURE, WAL_SIGNATURE_SIZE), 0);
}

TEST_F(LogManagerTest, ReopensExistingSingleSegment) {
  write_valid_segment(0, 42);
  size_t expected_size = WAL_SIGNATURE_SIZE + 42;

  LogManager log_mgr(test_prefix_, TEST_SEGMENT_SIZE);

  EXPECT_EQ(log_mgr.get_current_segment_id(), 0u);
  EXPECT_EQ(log_mgr.get_next_lsn(), expected_size);
  EXPECT_EQ(log_mgr.get_flushed_lsn(), expected_size);
}

TEST_F(LogManagerTest, DiscoversLatestSegmentAcrossMultipleSegments) {
  write_valid_segment(0, 900);
  write_valid_segment(1, 850);
  write_valid_segment(2, 112);

  LogManager log_mgr(test_prefix_, TEST_SEGMENT_SIZE);

  lsn_t expected_lsn = (2ULL * TEST_SEGMENT_SIZE) + WAL_SIGNATURE_SIZE + 112;
  EXPECT_EQ(log_mgr.get_current_segment_id(), 2u);
  EXPECT_EQ(log_mgr.get_next_lsn(), expected_lsn);
  EXPECT_EQ(log_mgr.get_flushed_lsn(), expected_lsn);
}

TEST_F(LogManagerTest, DiscoversLatestSegmentAfterOldSegmentsDeleted) {
  write_valid_segment(3, 56);
  write_valid_segment(4, 192);

  LogManager log_mgr(test_prefix_, TEST_SEGMENT_SIZE);

  lsn_t expected_lsn = (4ULL * TEST_SEGMENT_SIZE) + WAL_SIGNATURE_SIZE + 192;
  EXPECT_FALSE(std::filesystem::exists(segment_path(0)));
  EXPECT_EQ(log_mgr.get_current_segment_id(), 4u);
  EXPECT_EQ(log_mgr.get_next_lsn(), expected_lsn);
  EXPECT_EQ(log_mgr.get_flushed_lsn(), expected_lsn);
}

TEST_F(LogManagerTest, IgnoresUnrelatedFilesInDirectory) {
  write_valid_segment(0, 16);

  std::ofstream((test_dir_ / "letty.wal.tmp").string()).put('a');
  std::ofstream((test_dir_ / "letty.wal.abc").string()).put('b');
  std::ofstream((test_dir_ / "other.wal.99").string()).put('c');

  LogManager log_mgr(test_prefix_, TEST_SEGMENT_SIZE);

  EXPECT_EQ(log_mgr.get_current_segment_id(), 0u);
  EXPECT_EQ(log_mgr.get_next_lsn(), WAL_SIGNATURE_SIZE + 16);
}

TEST_F(LogManagerTest, ThrowsOnCorruptSegmentHeader) {
  write_raw_segment(0, "BADMAGIC");

  try {
    LogManager log_mgr(test_prefix_, TEST_SEGMENT_SIZE);
    FAIL() << "Expected DbException for corrupt WAL header";
  } catch (const DbException& ex) {
    EXPECT_EQ(ex.code(), DbErrorCode::Corruption);
  }
}

TEST_F(LogManagerTest, ThrowsOnTruncatedSegmentHeader) {
  write_raw_segment(0, "LETT");

  try {
    LogManager log_mgr(test_prefix_, TEST_SEGMENT_SIZE);
    FAIL() << "Expected DbException for truncated WAL header";
  } catch (const DbException& ex) {
    EXPECT_EQ(ex.code(), DbErrorCode::Corruption);
  }
}

TEST_F(LogManagerTest, AppendFlushAndReadRecordAt) {
  LogManager log_mgr(test_prefix_, TEST_SEGMENT_SIZE);

  LogRecord begin_rec{};
  ASSERT_TRUE(build_transaction_record(LogRecordType::BEGIN, 1, begin_rec));
  lsn_t begin_lsn = log_mgr.append_record(begin_rec);
  EXPECT_EQ(begin_lsn, WAL_SIGNATURE_SIZE);
  EXPECT_EQ(begin_rec.header.lsn, begin_lsn);
  EXPECT_EQ(log_mgr.get_flushed_lsn(), WAL_SIGNATURE_SIZE);

  PagePatch patch_input{5, 128, {1, 2, 3, 4}, {5, 6, 7, 8}};
  LogRecord patch_rec{};
  ASSERT_TRUE(build_page_patch_record(1, patch_input, patch_rec));
  patch_rec.header.prev_lsn = begin_lsn;

  lsn_t patch_lsn = log_mgr.append_record(patch_rec);
  EXPECT_GT(patch_lsn, begin_lsn);

  EXPECT_TRUE(log_mgr.flush_to_lsn(patch_lsn));
  EXPECT_EQ(log_mgr.get_flushed_lsn(), log_mgr.get_next_lsn());

  auto read_begin = log_mgr.read_record_at(begin_lsn);
  ASSERT_TRUE(read_begin.has_value());
  EXPECT_EQ(read_begin->header.lsn, begin_lsn);
  EXPECT_EQ(read_begin->header.tran_id, 1u);
  EXPECT_EQ(read_begin->header.type, LogRecordType::BEGIN);

  auto read_patch = log_mgr.read_record_at(patch_lsn);
  ASSERT_TRUE(read_patch.has_value());
  EXPECT_EQ(read_patch->header.lsn, patch_lsn);
  EXPECT_EQ(read_patch->header.prev_lsn, begin_lsn);
  EXPECT_EQ(read_patch->header.type, LogRecordType::PAGE_UPDATE);

  PagePatch decoded_patch{};
  ASSERT_TRUE(deserialize_page_patch(read_patch->payload, decoded_patch));
  EXPECT_EQ(decoded_patch.page_id, 5);
  EXPECT_EQ(decoded_patch.offset, 128);
  EXPECT_EQ(decoded_patch.before_payload, (ByteBuffer{1, 2, 3, 4}));
  EXPECT_EQ(decoded_patch.after_payload, (ByteBuffer{5, 6, 7, 8}));
}

TEST_F(LogManagerTest, SegmentRolloverAndReadRecordsAcrossSegments) {
  static constexpr uint32_t SMALL_SEGMENT = 128;
  LogManager log_mgr(test_prefix_, SMALL_SEGMENT);

  std::vector<lsn_t> assigned_lsns;
  for (uint64_t i = 1; i <= 5; ++i) {
    LogRecord rec{};
    ASSERT_TRUE(build_transaction_record(LogRecordType::BEGIN, i, rec));
    lsn_t lsn = log_mgr.append_record(rec);
    ASSERT_NE(lsn, INVALID_LSN);
    assigned_lsns.push_back(lsn);
  }

  EXPECT_GT(log_mgr.get_current_segment_id(), 0u);
  EXPECT_TRUE(std::filesystem::exists(segment_path(0)));
  EXPECT_TRUE(std::filesystem::exists(segment_path(1)));

  for (size_t i = 0; i < assigned_lsns.size(); ++i) {
    auto rec = log_mgr.read_record_at(assigned_lsns[i]);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->header.lsn, assigned_lsns[i]);
    EXPECT_EQ(rec->header.tran_id, i + 1);
  }

  std::vector<LogRecord> all_records = log_mgr.read_records();
  ASSERT_EQ(all_records.size(), 5u);
  for (size_t i = 0; i < all_records.size(); ++i) {
    EXPECT_EQ(all_records[i].header.lsn, assigned_lsns[i]);
    EXPECT_EQ(all_records[i].header.tran_id, i + 1);
  }
}

TEST_F(LogManagerTest, TruncateResetsSegmentsAndLsn) {
  static constexpr uint32_t SMALL_SEGMENT = 128;
  LogManager log_mgr(test_prefix_, SMALL_SEGMENT);

  for (uint64_t i = 1; i <= 5; ++i) {
    LogRecord rec{};
    ASSERT_TRUE(build_transaction_record(LogRecordType::BEGIN, i, rec));
    ASSERT_NE(log_mgr.append_record(rec), INVALID_LSN);
  }
  ASSERT_GT(log_mgr.get_current_segment_id(), 0u);

  EXPECT_TRUE(log_mgr.truncate());
  EXPECT_EQ(log_mgr.get_current_segment_id(), 0u);
  EXPECT_EQ(log_mgr.get_next_lsn(), WAL_SIGNATURE_SIZE);
  EXPECT_EQ(log_mgr.get_flushed_lsn(), WAL_SIGNATURE_SIZE);
  EXPECT_TRUE(std::filesystem::exists(segment_path(0)));
  EXPECT_FALSE(std::filesystem::exists(segment_path(1)));
  EXPECT_TRUE(log_mgr.read_records().empty());
}

TEST_F(LogManagerTest, LazyTimerFlushesAfterInterval) {
  const uint32_t FLUSH_INTERVAL = 1;
  // Use a 1ms flush interval to keep the test fast.
  LogManager log_mgr(test_prefix_, TEST_SEGMENT_SIZE, FLUSH_INTERVAL);

  LogRecord rec{};
  ASSERT_TRUE(build_transaction_record(LogRecordType::BEGIN, 1, rec));
  lsn_t lsn = log_mgr.append_record(rec);
  ASSERT_NE(lsn, INVALID_LSN);

  // Before the timer fires, the record is in the buffer, not on disk.
  EXPECT_EQ(log_mgr.get_flushed_lsn(), WAL_SIGNATURE_SIZE);

  // Sleep just past the flush interval so the next append triggers a flush.
  std::this_thread::sleep_for(std::chrono::milliseconds(2));

  // Append another record — the timer should trigger a flush of the first one.
  LogRecord rec2{};
  ASSERT_TRUE(build_transaction_record(LogRecordType::BEGIN, 2, rec2));
  log_mgr.append_record(rec2);

  // The first record's LSN should now be on disk.
  EXPECT_GT(log_mgr.get_flushed_lsn(), lsn);
}

TEST_F(LogManagerTest, FlushToLsnIsIdempotent) {
  const uint32_t FLUSH_INTERVAL = 5000;

  // long interval, no auto-flush
  LogManager log_mgr(test_prefix_, TEST_SEGMENT_SIZE, FLUSH_INTERVAL);

  LogRecord rec{};
  ASSERT_TRUE(build_transaction_record(LogRecordType::BEGIN, 1, rec));
  lsn_t lsn = log_mgr.append_record(rec);
  ASSERT_NE(lsn, INVALID_LSN);

  // Buffer not flushed yet (timer still running).
  EXPECT_EQ(log_mgr.get_flushed_lsn(), WAL_SIGNATURE_SIZE);

  // Append a second record so the flush target is past the initial flushed_lsn_.
  LogRecord rec2{};
  ASSERT_TRUE(build_transaction_record(LogRecordType::BEGIN, 2, rec2));
  lsn_t lsn2 = log_mgr.append_record(rec2);
  ASSERT_NE(lsn2, INVALID_LSN);

  // Explicit flush persists the entire buffer.
  EXPECT_TRUE(log_mgr.flush_to_lsn(lsn2));
  EXPECT_EQ(log_mgr.get_flushed_lsn(), log_mgr.get_next_lsn());

  lsn_t last_flushed_lsn = log_mgr.get_flushed_lsn();

  // Calling flush_to_lsn again with any already-flushed LSN should return true immediately.
  EXPECT_TRUE(log_mgr.flush_to_lsn(lsn2));
  EXPECT_TRUE(log_mgr.flush_to_lsn(lsn));
  EXPECT_TRUE(log_mgr.flush_to_lsn(WAL_SIGNATURE_SIZE));
  EXPECT_TRUE(log_mgr.flush_to_lsn(INVALID_LSN));
  EXPECT_EQ(log_mgr.get_flushed_lsn(), last_flushed_lsn);
}

TEST_F(LogManagerTest, MultipleRecordsBatchedIntoSingleFlush) {
  // Long flush interval so records accumulate in the buffer.
  LogManager log_mgr(test_prefix_, TEST_SEGMENT_SIZE, 5000);

  std::vector<lsn_t> lsns;
  for (uint64_t i = 1; i <= 10; ++i) {
    LogRecord rec{};
    ASSERT_TRUE(build_transaction_record(LogRecordType::BEGIN, i, rec));
    lsn_t lsn = log_mgr.append_record(rec);
    ASSERT_NE(lsn, INVALID_LSN);
    lsns.push_back(lsn);
  }

  // None flushed yet — all in buffer.
  EXPECT_EQ(log_mgr.get_flushed_lsn(), WAL_SIGNATURE_SIZE);

  // Single explicit flush should persist all 10 records.
  EXPECT_TRUE(log_mgr.flush_to_lsn(lsns.back()));
  EXPECT_EQ(log_mgr.get_flushed_lsn(), log_mgr.get_next_lsn());

  // All records readable from disk.
  for (size_t i = 0; i < lsns.size(); ++i) {
    auto rec = log_mgr.read_record_at(lsns[i]);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->header.tran_id, i + 1);
  }
}

TEST_F(LogManagerTest, ReadRecordsFromSkipsEarlierRecordsInSameSegment) {
  LogManager log_mgr(test_prefix_, 1024 * 1024);

  std::vector<lsn_t> lsns;
  for (uint64_t i = 1; i <= 5; ++i) {
    LogRecord rec{};
    ASSERT_TRUE(build_transaction_record(LogRecordType::BEGIN, i, rec));
    lsn_t lsn = log_mgr.append_record(rec);
    ASSERT_NE(lsn, INVALID_LSN);
    lsns.push_back(lsn);
  }
  log_mgr.flush_to_lsn(lsns.back());

  auto subset = log_mgr.read_records_from(lsns[2]);
  ASSERT_EQ(subset.size(), 3u);
  EXPECT_EQ(subset[0].header.tran_id, 3u);
  EXPECT_EQ(subset[1].header.tran_id, 4u);
  EXPECT_EQ(subset[2].header.tran_id, 5u);
}

TEST_F(LogManagerTest, ReadRecordsFromSkipsEarlierSegments) {
  LogManager log_mgr(test_prefix_, 200);

  std::vector<lsn_t> lsns;
  for (uint64_t i = 1; i <= 15; ++i) {
    LogRecord rec{};
    ASSERT_TRUE(build_transaction_record(LogRecordType::BEGIN, i, rec));
    lsn_t lsn = log_mgr.append_record(rec);
    ASSERT_NE(lsn, INVALID_LSN);
    lsns.push_back(lsn);
  }
  log_mgr.flush_to_lsn(lsns.back());

  EXPECT_GT(log_mgr.get_current_segment_id(), 0u);

  lsn_t target_lsn = lsns[8];
  auto subset = log_mgr.read_records_from(target_lsn);
  ASSERT_EQ(subset.size(), 7u);
  EXPECT_EQ(subset.front().header.lsn, target_lsn);
  EXPECT_EQ(subset.front().header.tran_id, 9u);
  EXPECT_EQ(subset.back().header.tran_id, 15u);
}

TEST_F(LogManagerTest, ListSegmentIdsReturnsEmptyForFreshDirectory) {
  TestableLogManager log_mgr(test_prefix_, TEST_SEGMENT_SIZE);

  auto segments = log_mgr.list_segment_ids();
  ASSERT_EQ(segments.size(), 1u);
  EXPECT_EQ(segments[0], 0u);
}

TEST_F(LogManagerTest, ListSegmentIdsSortsMultipleSegments) {
  write_valid_segment(5, 100);
  write_valid_segment(2, 100);
  write_valid_segment(8, 100);
  write_valid_segment(1, 100);

  TestableLogManager log_mgr(test_prefix_, TEST_SEGMENT_SIZE);

  auto segments = log_mgr.list_segment_ids();
  ASSERT_EQ(segments.size(), 4u);
  EXPECT_EQ(segments[0], 1u);
  EXPECT_EQ(segments[1], 2u);
  EXPECT_EQ(segments[2], 5u);
  EXPECT_EQ(segments[3], 8u);
}

TEST_F(LogManagerTest, ListSegmentIdsIgnoresInvalidFilenames) {
  write_valid_segment(0, 50);
  write_valid_segment(1, 50);
  write_valid_segment(3, 50);

  std::ofstream((test_dir_ / "letty.wal.abc").string()).put('x');
  std::ofstream((test_dir_ / "letty.wal.1.2.3").string()).put('y');
  std::ofstream((test_dir_ / "letty.wal.").string()).put('z');
  std::ofstream((test_dir_ / "other.wal.0").string()).put('w');

  TestableLogManager log_mgr(test_prefix_, TEST_SEGMENT_SIZE);

  auto segments = log_mgr.list_segment_ids();
  ASSERT_EQ(segments.size(), 3u);
  EXPECT_EQ(segments[0], 0u);
  EXPECT_EQ(segments[1], 1u);
  EXPECT_EQ(segments[2], 3u);
}

TEST_F(LogManagerTest, ListSegmentIdsHandlesGapsInSegmentIds) {
  write_valid_segment(10, 100);
  write_valid_segment(20, 100);
  write_valid_segment(30, 100);

  TestableLogManager log_mgr(test_prefix_, TEST_SEGMENT_SIZE);

  auto segments = log_mgr.list_segment_ids();
  ASSERT_EQ(segments.size(), 3u);
  EXPECT_EQ(segments[0], 10u);
  EXPECT_EQ(segments[1], 20u);
  EXPECT_EQ(segments[2], 30u);
}

TEST_F(LogManagerTest, FindLatestSegmentReturnsSegmentZeroOnFreshStart) {
  TestableLogManager log_mgr(test_prefix_, TEST_SEGMENT_SIZE);

  auto latest = log_mgr.find_latest_segment();
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->first, 0u);
  EXPECT_EQ(latest->second, WAL_SIGNATURE_SIZE);
}

TEST_F(LogManagerTest, FindLatestSegmentReturnsSingleSegment) {
  write_valid_segment(0, 256);

  TestableLogManager log_mgr(test_prefix_, TEST_SEGMENT_SIZE);

  auto latest = log_mgr.find_latest_segment();
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->first, 0u);
  EXPECT_EQ(latest->second, WAL_SIGNATURE_SIZE + 256);
}

TEST_F(LogManagerTest, FindLatestSegmentReturnsHighestId) {
  write_valid_segment(2, 100);
  write_valid_segment(0, 150);
  write_valid_segment(5, 200);
  write_valid_segment(3, 50);

  TestableLogManager log_mgr(test_prefix_, TEST_SEGMENT_SIZE);

  auto latest = log_mgr.find_latest_segment();
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->first, 5u);
  EXPECT_EQ(latest->second, WAL_SIGNATURE_SIZE + 200);
}

TEST_F(LogManagerTest, FindLatestSegmentWithGappedIds) {
  write_valid_segment(100, 256);
  write_valid_segment(50, 100);
  write_valid_segment(200, 512);

  TestableLogManager log_mgr(test_prefix_, TEST_SEGMENT_SIZE);

  auto latest = log_mgr.find_latest_segment();
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->first, 200u);
  EXPECT_EQ(latest->second, WAL_SIGNATURE_SIZE + 512);
}

}  // namespace letty
