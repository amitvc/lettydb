#include "storage/checkpoint_manager.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>
#include <gtest/gtest.h>

#include "buffer/buffer_pool_manager.h"
#include "buffer/lru_replacer.h"
#include "storage/disk_manager.h"
#include "storage/log_manager.h"
#include "storage/log_record.h"
#include "transaction/transaction_manager.h"

namespace letty {

class CheckpointManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = "test_checkpoint_dir_" + std::to_string(test_counter_++);
    std::filesystem::remove_all(test_dir_);
    std::filesystem::create_directories(test_dir_);
    db_path_ = (std::filesystem::path(test_dir_) / "test.db").string();
    wal_prefix_ = (std::filesystem::path(test_dir_) / "test.wal").string();

    disk_mgr_ = std::make_unique<DiskManager>(db_path_);
    bpm_ = std::make_unique<BufferPoolManager>(*disk_mgr_, 10, std::make_unique<LRUPageReplacer>());
    log_mgr_ = std::make_unique<LogManager>(wal_prefix_, 1024 * 1024);
    txn_mgr_ = std::make_unique<TransactionManager>(*log_mgr_);
  }

  void TearDown() override {
    txn_mgr_.reset();
    bpm_.reset();
    log_mgr_.reset();
    disk_mgr_.reset();
    std::filesystem::remove_all(test_dir_);
  }

  std::string test_dir_;
  std::string db_path_;
  std::string wal_prefix_;
  std::unique_ptr<DiskManager> disk_mgr_;
  std::unique_ptr<BufferPoolManager> bpm_;
  std::unique_ptr<LogManager> log_mgr_;
  std::unique_ptr<TransactionManager> txn_mgr_;
  static int test_counter_;
};

int CheckpointManagerTest::test_counter_ = 0;

TEST_F(CheckpointManagerTest, CheckpointMarkerSerializationRoundTrip) {
  CheckpointMarker original{};
  std::memcpy(original.signature, CHECKPOINT_MARKER_SIGNATURE, sizeof(original.signature));
  original.last_checkpoint_lsn = 5000;

  ByteBuffer buffer;
  ASSERT_TRUE(serialize_checkpoint_marker(original, buffer));
  EXPECT_EQ(buffer.size(), sizeof(CheckpointMarker));

  CheckpointMarker decoded{};
  ASSERT_TRUE(deserialize_checkpoint_marker(buffer, decoded));

  EXPECT_EQ(std::memcmp(decoded.signature, CHECKPOINT_MARKER_SIGNATURE, sizeof(decoded.signature)), 0);
  EXPECT_EQ(decoded.last_checkpoint_lsn, 5000);
}

TEST_F(CheckpointManagerTest, CheckpointMarkerRejectsInvalidSignature) {
  CheckpointMarker marker{};
  std::memcpy(marker.signature, "INVALID!!", sizeof(marker.signature));
  marker.last_checkpoint_lsn = 5000;

  ByteBuffer buffer;
  ASSERT_TRUE(serialize_checkpoint_marker(marker, buffer));

  CheckpointMarker decoded{};
  EXPECT_FALSE(deserialize_checkpoint_marker(buffer, decoded));
}

TEST_F(CheckpointManagerTest, CheckpointMarkerRejectsCorruptChecksum) {
  CheckpointMarker original{};
  std::memcpy(original.signature, CHECKPOINT_MARKER_SIGNATURE, sizeof(original.signature));
  original.last_checkpoint_lsn = 5000;

  ByteBuffer buffer;
  ASSERT_TRUE(serialize_checkpoint_marker(original, buffer));

  buffer[buffer.size() - 1] ^= 0xFF;

  CheckpointMarker decoded{};
  EXPECT_FALSE(deserialize_checkpoint_marker(buffer, decoded));
}

TEST_F(CheckpointManagerTest, RunCheckpointCreatesMarkerAndWALRecords) {
  uint64_t txn1 = txn_mgr_->begin();
  uint64_t txn2 = txn_mgr_->begin();
  txn_mgr_->record_lsn(txn1, 150);

  bpm_->track_dirtied_page(10, 100);
  bpm_->track_dirtied_page(20, 200);

  CheckpointManager checkpoint_mgr(db_path_, *bpm_, *log_mgr_, *txn_mgr_, 60000, 1024 * 1024);
  ASSERT_TRUE(checkpoint_mgr.run_checkpoint());

  lsn_t marker_lsn = checkpoint_mgr.read_checkpoint_marker();
  EXPECT_NE(marker_lsn, INVALID_LSN);
  EXPECT_TRUE(std::filesystem::exists(db_path_ + ".checkpoint"));

  auto records = log_mgr_->read_records();
  ASSERT_GE(records.size(), 2u);

  std::optional<LogRecord> begin_record;
  std::optional<LogRecord> end_record;
  for (const auto& rec : records) {
    if (rec.header.lsn == marker_lsn) {
      begin_record = rec;
    } else if (begin_record.has_value() && !end_record.has_value()) {
      end_record = rec;
    }
  }

  ASSERT_TRUE(begin_record.has_value());
  EXPECT_EQ(begin_record->header.type, LogRecordType::BEGIN_CHECKPOINT);

  ASSERT_TRUE(end_record.has_value());
  EXPECT_EQ(end_record->header.type, LogRecordType::END_CHECKPOINT);

  CheckpointPatch patch{};
  ASSERT_TRUE(deserialize_checkpoint_patch(end_record->payload, patch));

  EXPECT_EQ(patch.dirty_pages.size(), 2u);
  EXPECT_EQ(patch.active_transactions.size(), 2u);

  std::unordered_map<page_id_t, lsn_t> dpt_map(patch.dirty_pages.begin(), patch.dirty_pages.end());
  EXPECT_EQ(dpt_map[10], 100);
  EXPECT_EQ(dpt_map[20], 200);

  std::unordered_map<uint64_t, lsn_t> att_map(patch.active_transactions.begin(), patch.active_transactions.end());
  EXPECT_EQ(att_map[txn1], 150);
  EXPECT_EQ(att_map[txn2], txn_mgr_->get_last_lsn(txn2));

  EXPECT_GE(log_mgr_->get_flushed_lsn(), end_record->header.lsn);
}

TEST_F(CheckpointManagerTest, ShouldCheckpointTimeTrigger) {
  CheckpointManager checkpoint_mgr(db_path_, *bpm_, *log_mgr_, *txn_mgr_, 50, 1024 * 1024);
  EXPECT_FALSE(checkpoint_mgr.should_checkpoint());

  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  EXPECT_TRUE(checkpoint_mgr.should_checkpoint());

  ASSERT_TRUE(checkpoint_mgr.run_checkpoint());
  EXPECT_FALSE(checkpoint_mgr.should_checkpoint());
}

TEST_F(CheckpointManagerTest, ShouldCheckpointWalSizeTrigger) {
  CheckpointManager checkpoint_mgr(db_path_, *bpm_, *log_mgr_, *txn_mgr_, 60000, 100);
  EXPECT_FALSE(checkpoint_mgr.should_checkpoint());

  for (uint64_t i = 1; i <= 5; ++i) {
    LogRecord rec{};
    ASSERT_TRUE(build_transaction_record(LogRecordType::BEGIN, i, rec));
    ASSERT_NE(log_mgr_->append_record(rec), INVALID_LSN);
  }

  EXPECT_TRUE(checkpoint_mgr.should_checkpoint());
  ASSERT_TRUE(checkpoint_mgr.run_checkpoint());
  EXPECT_FALSE(checkpoint_mgr.should_checkpoint());
}

TEST_F(CheckpointManagerTest, ReadCheckpointMarkerReturnsInvalidWhenNoFile) {
  CheckpointManager checkpoint_mgr(db_path_, *bpm_, *log_mgr_, *txn_mgr_);
  EXPECT_EQ(checkpoint_mgr.read_checkpoint_marker(), INVALID_LSN);
}

TEST_F(CheckpointManagerTest, ReadCheckpointMarkerRejectsCorruptFile) {
  CheckpointManager checkpoint_mgr(db_path_, *bpm_, *log_mgr_, *txn_mgr_);
  ASSERT_TRUE(checkpoint_mgr.run_checkpoint());
  EXPECT_NE(checkpoint_mgr.read_checkpoint_marker(), INVALID_LSN);

  std::string marker_path = db_path_ + ".checkpoint";
  std::fstream file(marker_path, std::ios::in | std::ios::out | std::ios::binary);
  ASSERT_TRUE(file.is_open());
  char corrupted_byte = 0;
  file.seekg(10);
  file.read(&corrupted_byte, 1);
  corrupted_byte ^= 0xFF;
  file.seekp(10);
  file.write(&corrupted_byte, 1);
  file.close();

  EXPECT_EQ(checkpoint_mgr.read_checkpoint_marker(), INVALID_LSN);
}

TEST_F(CheckpointManagerTest, ReadCheckpointMarkerRejectsTruncatedFile) {
  CheckpointManager checkpoint_mgr(db_path_, *bpm_, *log_mgr_, *txn_mgr_);
  std::string marker_path = db_path_ + ".checkpoint";

  std::ofstream file(marker_path, std::ios::trunc | std::ios::binary);
  ASSERT_TRUE(file.is_open());
  file.write("SHORT", 5);
  file.close();

  EXPECT_EQ(checkpoint_mgr.read_checkpoint_marker(), INVALID_LSN);
}

TEST_F(CheckpointManagerTest, SequentialCheckpointsAdvanceMarker) {
  CheckpointManager checkpoint_mgr(db_path_, *bpm_, *log_mgr_, *txn_mgr_, 60000, 1024 * 1024);

  ASSERT_TRUE(checkpoint_mgr.run_checkpoint());
  lsn_t first_checkpoint_lsn = checkpoint_mgr.read_checkpoint_marker();
  EXPECT_NE(first_checkpoint_lsn, INVALID_LSN);

  txn_mgr_->begin();

  ASSERT_TRUE(checkpoint_mgr.run_checkpoint());
  lsn_t second_checkpoint_lsn = checkpoint_mgr.read_checkpoint_marker();
  EXPECT_NE(second_checkpoint_lsn, INVALID_LSN);

  EXPECT_GT(second_checkpoint_lsn, first_checkpoint_lsn);
}

TEST_F(CheckpointManagerTest, ManualAnalysisReconstruction) {
  uint64_t txn1 = txn_mgr_->begin();
  bpm_->track_dirtied_page(10, 100);

  CheckpointManager checkpoint_mgr(db_path_, *bpm_, *log_mgr_, *txn_mgr_, 60000, 1024 * 1024);
  ASSERT_TRUE(checkpoint_mgr.run_checkpoint());
  lsn_t marker_lsn = checkpoint_mgr.read_checkpoint_marker();
  ASSERT_NE(marker_lsn, INVALID_LSN);

  uint64_t txn2 = txn_mgr_->begin();
  bpm_->track_dirtied_page(20, 200);
  ASSERT_TRUE(txn_mgr_->commit(txn1));

  auto records = log_mgr_->read_records();
  std::unordered_map<uint64_t, lsn_t> reconstructed_att;
  std::unordered_map<page_id_t, lsn_t> reconstructed_dpt;

  bool found_end_checkpoint = false;
  for (const auto& rec : records) {
    if (rec.header.lsn < marker_lsn) {
      continue;
    }

    if (rec.header.type == LogRecordType::END_CHECKPOINT) {
      CheckpointPatch patch{};
      ASSERT_TRUE(deserialize_checkpoint_patch(rec.payload, patch));
      for (const auto& [page_id, rec_lsn] : patch.dirty_pages) {
        reconstructed_dpt[page_id] = rec_lsn;
      }
      for (const auto& [active_txn_id, last_lsn] : patch.active_transactions) {
        reconstructed_att[active_txn_id] = last_lsn;
      }
      found_end_checkpoint = true;
      continue;
    }

    if (!found_end_checkpoint) {
      continue;
    }

    if (rec.header.type == LogRecordType::BEGIN) {
      reconstructed_att[rec.header.tran_id] = rec.header.lsn;
    } else if (rec.header.type == LogRecordType::COMMIT || rec.header.type == LogRecordType::ABORT) {
      reconstructed_att.erase(rec.header.tran_id);
    }
  }

  ASSERT_TRUE(found_end_checkpoint);
  EXPECT_EQ(reconstructed_att.find(txn1), reconstructed_att.end());
  EXPECT_NE(reconstructed_att.find(txn2), reconstructed_att.end());
  EXPECT_EQ(reconstructed_dpt.count(10), 1u);
}

TEST_F(CheckpointManagerTest, CheckpointWithNoDirtiedPages) {
  CheckpointManager checkpoint_mgr(db_path_, *bpm_, *log_mgr_, *txn_mgr_, 60000, 1024 * 1024);
  ASSERT_TRUE(checkpoint_mgr.run_checkpoint());

  lsn_t marker_lsn = checkpoint_mgr.read_checkpoint_marker();
  EXPECT_NE(marker_lsn, INVALID_LSN);

  auto records = log_mgr_->read_records();
  std::optional<LogRecord> end_record;
  for (const auto& rec : records) {
    if (rec.header.type == LogRecordType::END_CHECKPOINT) {
      end_record = rec;
      break;
    }
  }

  ASSERT_TRUE(end_record.has_value());
  CheckpointPatch patch{};
  ASSERT_TRUE(deserialize_checkpoint_patch(end_record->payload, patch));
  EXPECT_EQ(patch.dirty_pages.size(), 0u);
}

TEST_F(CheckpointManagerTest, CheckpointWithNoActiveTransactions) {
  uint64_t txn1 = txn_mgr_->begin();
  bpm_->track_dirtied_page(10, 100);
  ASSERT_TRUE(txn_mgr_->commit(txn1));

  CheckpointManager checkpoint_mgr(db_path_, *bpm_, *log_mgr_, *txn_mgr_, 60000, 1024 * 1024);
  ASSERT_TRUE(checkpoint_mgr.run_checkpoint());

  lsn_t marker_lsn = checkpoint_mgr.read_checkpoint_marker();
  EXPECT_NE(marker_lsn, INVALID_LSN);

  auto records = log_mgr_->read_records();
  std::optional<LogRecord> end_record;
  for (const auto& rec : records) {
    if (rec.header.type == LogRecordType::END_CHECKPOINT) {
      end_record = rec;
      break;
    }
  }

  ASSERT_TRUE(end_record.has_value());
  CheckpointPatch patch{};
  ASSERT_TRUE(deserialize_checkpoint_patch(end_record->payload, patch));
  EXPECT_EQ(patch.active_transactions.size(), 0u);
}

}  // namespace letty


