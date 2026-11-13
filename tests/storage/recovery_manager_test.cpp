#include "storage/recovery_manager.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>
#include <gtest/gtest.h>

#include "buffer/buffer_pool_manager.h"
#include "buffer/lru_replacer.h"
#include "storage/checkpoint_manager.h"
#include "storage/disk_manager.h"
#include "storage/log_manager.h"
#include "storage/log_record.h"
#include "transaction/transaction_manager.h"

namespace letty {

class RecoveryManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = "test_recovery_dir_" + std::to_string(test_counter_++);
    std::filesystem::remove_all(test_dir_);
    std::filesystem::create_directories(test_dir_);
    db_path_ = (std::filesystem::path(test_dir_) / "recovery.db").string();
    wal_prefix_ = (std::filesystem::path(test_dir_) / "recovery.wal").string();

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

  lsn_t append_page_update(uint64_t txn_id, page_id_t page_id, uint16_t offset) {
    PagePatch patch{};
    patch.page_id = page_id;
    patch.offset = offset;
    patch.before_payload = {1, 2, 3};
    patch.after_payload = {4, 5, 6};

    LogRecord record{};
    build_page_patch_record(txn_id, patch, record);
    lsn_t assigned_lsn = log_mgr_->append_record(record);
    if (txn_id != INVALID_TXN_ID) {
      txn_mgr_->record_lsn(txn_id, assigned_lsn);
    }
    return assigned_lsn;
  }

  lsn_t append_gam_update(uint64_t txn_id, page_id_t gam_page_id, uint16_t bit_index) {
    GamPatch patch{};
    patch.gam_page_id = gam_page_id;
    patch.bit_index = bit_index;

    LogRecord record{};
    build_extent_record(LogRecordType::ALLOCATE_EXTENT, txn_id, patch, record);
    lsn_t assigned_lsn = log_mgr_->append_record(record);
    if (txn_id != INVALID_TXN_ID) {
      txn_mgr_->record_lsn(txn_id, assigned_lsn);
    }
    return assigned_lsn;
  }

  lsn_t append_iam_update(uint64_t txn_id, page_id_t iam_page_id, uint32_t extent_id) {
    IamPatch patch{};
    patch.iam_page_id = iam_page_id;
    patch.extent_id = extent_id;

    LogRecord record{};
    build_iam_extent_record(LogRecordType::IAM_ADD_EXTENT, txn_id, patch, record);
    lsn_t assigned_lsn = log_mgr_->append_record(record);
    if (txn_id != INVALID_TXN_ID) {
      txn_mgr_->record_lsn(txn_id, assigned_lsn);
    }
    return assigned_lsn;
  }

  lsn_t append_compensation(uint64_t txn_id, page_id_t page_id, lsn_t undo_next_lsn) {
    CompensationPatch patch{};
    patch.page_id = page_id;
    patch.offset = 0;
    patch.undo_next_lsn = undo_next_lsn;
    patch.payload = {9, 9, 9};

    LogRecord record{};
    build_compensation_patch_record(txn_id, patch, record);
    lsn_t assigned_lsn = log_mgr_->append_record(record);
    if (txn_id != INVALID_TXN_ID) {
      txn_mgr_->record_lsn(txn_id, assigned_lsn);
    }
    return assigned_lsn;
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

int RecoveryManagerTest::test_counter_ = 0;

TEST_F(RecoveryManagerTest, EmptyLogReturnsEmptyAnalysis) {
  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_);
  AnalysisResult result = recovery.analyze();

  EXPECT_TRUE(result.active_transactions.empty());
  EXPECT_TRUE(result.dirty_page_table.empty());
  EXPECT_EQ(result.min_rec_lsn, INVALID_LSN);
  EXPECT_EQ(result.last_log_lsn, INVALID_LSN);
}

TEST_F(RecoveryManagerTest, AnalysisWithoutCheckpointTracksActiveAndCommitted) {
  uint64_t txn1 = txn_mgr_->begin();
  lsn_t p1_lsn = append_page_update(txn1, 5, 0);

  uint64_t txn2 = txn_mgr_->begin();
  lsn_t p2_lsn = append_page_update(txn2, 8, 0);

  ASSERT_TRUE(txn_mgr_->commit(txn1));

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_);
  AnalysisResult result = recovery.analyze();

  EXPECT_EQ(result.active_transactions.size(), 1u);
  EXPECT_EQ(result.active_transactions.find(txn1), result.active_transactions.end());
  EXPECT_EQ(result.active_transactions[txn2], p2_lsn);

  EXPECT_EQ(result.dirty_page_table.size(), 2u);
  EXPECT_EQ(result.dirty_page_table[5], p1_lsn);
  EXPECT_EQ(result.dirty_page_table[8], p2_lsn);

  EXPECT_EQ(result.min_rec_lsn, p1_lsn);
  EXPECT_GT(result.last_log_lsn, p2_lsn);
}

TEST_F(RecoveryManagerTest, AnalysisWithoutCheckpointTracksAborted) {
  uint64_t txn1 = txn_mgr_->begin();
  lsn_t p1_lsn = append_page_update(txn1, 10, 0);
  ASSERT_TRUE(txn_mgr_->abort(txn1));

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_);
  AnalysisResult result = recovery.analyze();

  EXPECT_TRUE(result.active_transactions.empty());
  EXPECT_EQ(result.dirty_page_table.size(), 1u);
  EXPECT_EQ(result.dirty_page_table[10], p1_lsn);
  EXPECT_EQ(result.min_rec_lsn, p1_lsn);
}

TEST_F(RecoveryManagerTest, AnalysisWithCheckpointSeedsFromEndCheckpoint) {
  uint64_t txn1 = txn_mgr_->begin();
  bpm_->track_dirtied_page(10, 50);

  CheckpointManager checkpoint_mgr(db_path_, *bpm_, *log_mgr_, *txn_mgr_, 60000, 1024 * 1024);
  ASSERT_TRUE(checkpoint_mgr.run_checkpoint());
  lsn_t marker_lsn = checkpoint_mgr.read_checkpoint_marker();
  ASSERT_NE(marker_lsn, INVALID_LSN);

  uint64_t txn2 = txn_mgr_->begin();
  lsn_t p2_lsn = append_page_update(txn2, 20, 0);

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_);
  AnalysisResult result = recovery.analyze();

  EXPECT_EQ(result.scan_start_lsn, marker_lsn);
  EXPECT_EQ(result.active_transactions.size(), 2u);
  EXPECT_EQ(result.active_transactions[txn1], txn_mgr_->get_last_lsn(txn1));
  EXPECT_EQ(result.active_transactions[txn2], p2_lsn);

  EXPECT_EQ(result.dirty_page_table.size(), 2u);
  EXPECT_EQ(result.dirty_page_table[10], 50);
  EXPECT_EQ(result.dirty_page_table[20], p2_lsn);
  EXPECT_EQ(result.min_rec_lsn, 50);
}

TEST_F(RecoveryManagerTest, AnalysisWithCheckpointHandlesCommittedLoser) {
  uint64_t txn1 = txn_mgr_->begin();
  bpm_->track_dirtied_page(10, 50);

  CheckpointManager checkpoint_mgr(db_path_, *bpm_, *log_mgr_, *txn_mgr_, 60000, 1024 * 1024);
  ASSERT_TRUE(checkpoint_mgr.run_checkpoint());

  ASSERT_TRUE(txn_mgr_->commit(txn1));

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_);
  AnalysisResult result = recovery.analyze();

  EXPECT_TRUE(result.active_transactions.empty());
  EXPECT_EQ(result.dirty_page_table.size(), 1u);
  EXPECT_EQ(result.dirty_page_table[10], 50);
}

TEST_F(RecoveryManagerTest, AnalysisPreservesEarliestRecLsnOnMultipleUpdatesToSamePage) {
  uint64_t txn1 = txn_mgr_->begin();
  lsn_t first_lsn = append_page_update(txn1, 5, 0);

  uint64_t txn2 = txn_mgr_->begin();
  append_page_update(txn2, 5, 10);
  append_page_update(txn2, 5, 20);

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_);
  AnalysisResult result = recovery.analyze();

  EXPECT_EQ(result.dirty_page_table[5], first_lsn);
}

TEST_F(RecoveryManagerTest, AnalysisHandlesGamAndIamUpdates) {
  uint64_t txn1 = txn_mgr_->begin();
  lsn_t gam_lsn = append_gam_update(txn1, 0, 1);
  lsn_t iam_lsn = append_iam_update(txn1, 1, 100);

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_);
  AnalysisResult result = recovery.analyze();

  EXPECT_EQ(result.active_transactions[txn1], iam_lsn);
  EXPECT_EQ(result.dirty_page_table[0], gam_lsn);
  EXPECT_EQ(result.dirty_page_table[1], iam_lsn);
  EXPECT_EQ(result.min_rec_lsn, gam_lsn);
}

TEST_F(RecoveryManagerTest, AnalysisHandlesCompensationLogRecord) {
  uint64_t txn1 = txn_mgr_->begin();
  lsn_t update_lsn = append_page_update(txn1, 7, 0);
  lsn_t clr_lsn = append_compensation(txn1, 7, update_lsn);

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_);
  AnalysisResult result = recovery.analyze();

  EXPECT_EQ(result.active_transactions[txn1], clr_lsn);
  EXPECT_EQ(result.dirty_page_table[7], update_lsn);
}

TEST_F(RecoveryManagerTest, AnalysisWithCorruptCheckpointFallsBackToStart) {
  uint64_t txn1 = txn_mgr_->begin();
  lsn_t update_lsn = append_page_update(txn1, 12, 0);

  CheckpointManager checkpoint_mgr(db_path_, *bpm_, *log_mgr_, *txn_mgr_, 60000, 1024 * 1024);
  ASSERT_TRUE(checkpoint_mgr.run_checkpoint());

  std::string marker_path = db_path_ + ".checkpoint";
  std::ofstream marker_file(marker_path, std::ios::trunc | std::ios::binary);
  marker_file.write("CORRUPT", 7);
  marker_file.close();

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_);
  AnalysisResult result = recovery.analyze();

  EXPECT_EQ(result.scan_start_lsn, WAL_SIGNATURE_SIZE);
  EXPECT_EQ(result.dirty_page_table[12], update_lsn);
  EXPECT_EQ(result.active_transactions[txn1], update_lsn);
}

TEST_F(RecoveryManagerTest, AnalysisExplicitFromLsn) {
  uint64_t txn1 = txn_mgr_->begin();
  append_page_update(txn1, 15, 0);
  ASSERT_TRUE(txn_mgr_->commit(txn1));

  lsn_t target_start_lsn = log_mgr_->get_next_lsn();
  uint64_t txn2 = txn_mgr_->begin();
  lsn_t update_lsn = append_page_update(txn2, 16, 0);

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_);
  AnalysisResult result = recovery.analyze_from(target_start_lsn);

  EXPECT_EQ(result.scan_start_lsn, target_start_lsn);
  EXPECT_EQ(result.active_transactions.size(), 1u);
  EXPECT_EQ(result.active_transactions[txn2], update_lsn);
  EXPECT_EQ(result.dirty_page_table.size(), 1u);
  EXPECT_EQ(result.dirty_page_table[16], update_lsn);
}

}  // namespace letty
