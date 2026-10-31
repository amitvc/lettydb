#include "storage/log_manager.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

#include "common/db_exception.h"
#include "storage/config.h"

namespace letty {

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

}  // namespace letty
