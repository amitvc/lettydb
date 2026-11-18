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
#include "storage/rollback_manager.h"
#include "storage/storage_def.h"
#include "storage/page_utils.h"
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
    rollback_mgr_ = std::make_unique<RollbackManager>(*log_mgr_, *bpm_, db_path_);
    txn_mgr_ = std::make_unique<TransactionManager>(*log_mgr_, *rollback_mgr_);
  }

  void TearDown() override {
    txn_mgr_.reset();
    rollback_mgr_.reset();
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
    if (txn_id != INVALID_TXN_ID) {
      record.header.prev_lsn = txn_mgr_->get_last_lsn(txn_id);
    }
    lsn_t assigned_lsn = log_mgr_->append_record(record);
    if (txn_id != INVALID_TXN_ID) {
      txn_mgr_->record_lsn(txn_id, assigned_lsn);
    }
    return assigned_lsn;
  }

  lsn_t append_gam_update(uint64_t txn_id, page_id_t gam_page_id, uint16_t bit_index,
                          LogRecordType type = LogRecordType::ALLOCATE_EXTENT) {
    GamPatch patch{};
    patch.gam_page_id = gam_page_id;
    patch.bit_index = bit_index;

    LogRecord record{};
    build_extent_record(type, txn_id, patch, record);
    if (txn_id != INVALID_TXN_ID) {
      record.header.prev_lsn = txn_mgr_->get_last_lsn(txn_id);
    }
    lsn_t assigned_lsn = log_mgr_->append_record(record);
    if (txn_id != INVALID_TXN_ID) {
      txn_mgr_->record_lsn(txn_id, assigned_lsn);
    }
    return assigned_lsn;
  }

  lsn_t append_iam_update(uint64_t txn_id, page_id_t iam_page_id, uint32_t extent_id,
                          LogRecordType type = LogRecordType::IAM_ADD_EXTENT) {
    IamPatch patch{};
    patch.iam_page_id = iam_page_id;
    patch.extent_id = extent_id;

    LogRecord record{};
    build_iam_extent_record(type, txn_id, patch, record);
    if (txn_id != INVALID_TXN_ID) {
      record.header.prev_lsn = txn_mgr_->get_last_lsn(txn_id);
    }
    lsn_t assigned_lsn = log_mgr_->append_record(record);
    if (txn_id != INVALID_TXN_ID) {
      txn_mgr_->record_lsn(txn_id, assigned_lsn);
    }
    return assigned_lsn;
  }

  bool gam_bit_set(page_id_t gam_page_id, uint16_t bit_index) {
    PageGuard guard = bpm_->fetch_page_guard(gam_page_id);
    GAMPage gam = load_page_layout<GAMPage>(guard.get_page());
    Bitmap bitmap(gam.bitmap, GAM_MAX_BITS);
    return bitmap.is_set(bit_index);
  }

  bool iam_has_extent(page_id_t iam_page_id, uint32_t extent_id) {
    PageGuard guard = bpm_->fetch_page_guard(iam_page_id);
    IAMPage iam = load_page_layout<IAMPage>(guard.get_page());
    return iam.contains_extent(extent_id);
  }

  bool wal_has_compensation(CompensationKind kind) {
    std::vector<LogRecord> records = log_mgr_->read_records();
    for (const auto& rec : records) {
      if (rec.header.type != LogRecordType::COMPENSATION) {
        continue;
      }
      CompensationPatch clr{};
      if (deserialize_compensation_patch(rec.payload, clr) && clr.kind == kind) {
        return true;
      }
    }
    return false;
  }

  bool wal_has_end(uint64_t tran_id) {
    std::vector<LogRecord> records = log_mgr_->read_records();
    for (const auto& rec : records) {
      if (rec.header.type == LogRecordType::END && rec.header.tran_id == tran_id) {
        return true;
      }
    }
    return false;
  }

  lsn_t append_compensation(uint64_t txn_id, page_id_t page_id, lsn_t undo_next_lsn) {
    CompensationPatch patch{};
    patch.page_id = page_id;
    patch.offset = 0;
    patch.undo_next_lsn = undo_next_lsn;
    patch.payload = {9, 9, 9};

    LogRecord record{};
    build_compensation_patch_record(txn_id, patch, record);
    if (txn_id != INVALID_TXN_ID) {
      record.header.prev_lsn = txn_mgr_->get_last_lsn(txn_id);
    }
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
  std::unique_ptr<RollbackManager> rollback_mgr_;
  static int test_counter_;
};

int RecoveryManagerTest::test_counter_ = 0;

TEST_F(RecoveryManagerTest, EmptyLogReturnsEmptyAnalysis) {
  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
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

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
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

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
  AnalysisResult result = recovery.analyze();

  EXPECT_TRUE(result.active_transactions.empty());
  EXPECT_EQ(result.dirty_page_table.size(), 1u);
  EXPECT_EQ(result.dirty_page_table[10], p1_lsn);
  EXPECT_EQ(result.min_rec_lsn, p1_lsn);
}

TEST_F(RecoveryManagerTest, AnalysisWithCheckpointSeedsFromEndCheckpoint) {
  uint64_t txn1 = txn_mgr_->begin();
  bpm_->update_dirtied_page_lsn(10, 50);

  CheckpointManager checkpoint_mgr(db_path_, *bpm_, *log_mgr_, *txn_mgr_, 60000, 1024 * 1024);
  ASSERT_TRUE(checkpoint_mgr.run_checkpoint());
  lsn_t marker_lsn = checkpoint_mgr.read_checkpoint_marker();
  ASSERT_NE(marker_lsn, INVALID_LSN);

  uint64_t txn2 = txn_mgr_->begin();
  lsn_t p2_lsn = append_page_update(txn2, 20, 0);

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
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
  bpm_->update_dirtied_page_lsn(10, 50);

  CheckpointManager checkpoint_mgr(db_path_, *bpm_, *log_mgr_, *txn_mgr_, 60000, 1024 * 1024);
  ASSERT_TRUE(checkpoint_mgr.run_checkpoint());

  ASSERT_TRUE(txn_mgr_->commit(txn1));

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
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

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
  AnalysisResult result = recovery.analyze();

  EXPECT_EQ(result.dirty_page_table[5], first_lsn);
}

TEST_F(RecoveryManagerTest, AnalysisHandlesGamAndIamUpdates) {
  uint64_t txn1 = txn_mgr_->begin();
  lsn_t gam_lsn = append_gam_update(txn1, 0, 1);
  lsn_t iam_lsn = append_iam_update(txn1, 1, 100);

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
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

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
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

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
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

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
  AnalysisResult result = recovery.analyze_from(target_start_lsn);

  EXPECT_EQ(result.scan_start_lsn, target_start_lsn);
  EXPECT_EQ(result.active_transactions.size(), 1u);
  EXPECT_EQ(result.active_transactions[txn2], update_lsn);
  EXPECT_EQ(result.dirty_page_table.size(), 1u);
  EXPECT_EQ(result.dirty_page_table[16], update_lsn);
}

TEST_F(RecoveryManagerTest, RedoAppliesPagePatchToDirtiedPage) {
  uint64_t txn1 = txn_mgr_->begin();

  page_id_t page_id = 100;
  bpm_->new_page(page_id);

  {
    PageGuard pg = bpm_->fetch_page_guard(page_id);
    ASSERT_NE(pg.get_page(), nullptr);
    char* data = pg.get_page()->get_data();
    std::memset(data, 0, PAGE_SIZE);
    data[100] = 'A';
    data[101] = 'B';
    data[102] = 'C';
  }

  PagePatch patch{};
  patch.page_id = page_id;
  patch.offset = 100;
  patch.before_payload = {0x41, 0x42, 0x43};
  patch.after_payload = {0x58, 0x59, 0x5A};

  LogRecord record{};
  build_page_patch_record(txn1, patch, record);
  lsn_t update_lsn = log_mgr_->append_record(record);
  ASSERT_NE(update_lsn, INVALID_LSN);

  txn_mgr_->record_lsn(txn1, update_lsn);
  bpm_->update_dirtied_page_lsn(page_id, update_lsn);
  bpm_->unpin_page(page_id, false);

  ASSERT_TRUE(txn_mgr_->commit(txn1));

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
  AnalysisResult result = recovery.analyze();

  // Verify analysis found the record
  ASSERT_NE(result.min_rec_lsn, INVALID_LSN) << "Analysis should find PAGE_UPDATE record";
  ASSERT_EQ(result.dirty_page_table.size(), 1u);
  ASSERT_NE(result.dirty_page_table.find(page_id), result.dirty_page_table.end());

  PageGuard recovered_guard = bpm_->fetch_page_guard(page_id);
  ASSERT_NE(recovered_guard.get_page(), nullptr);
  // Verify patch was actually applied
  EXPECT_EQ((unsigned char)recovered_guard.get_page()->get_data()[100], 0x58u);
  EXPECT_EQ((unsigned char)recovered_guard.get_page()->get_data()[101], 0x59u);
  EXPECT_EQ((unsigned char)recovered_guard.get_page()->get_data()[102], 0x5Au);
  EXPECT_EQ(recovered_guard.get_page()->get_page_lsn(), update_lsn);
}

TEST_F(RecoveryManagerTest, RedoSkipsAlreadyAppliedChanges) {
  uint64_t txn1 = txn_mgr_->begin();

  page_id_t page_id = 101;
  bpm_->new_page(page_id);

  {
    PageGuard pg = bpm_->fetch_page_guard(page_id);
    ASSERT_NE(pg.get_page(), nullptr);
    char* data = pg.get_page()->get_data();
    std::memset(data, 0, PAGE_SIZE);
    data[50] = 0xFF;
  }

  PagePatch patch{};
  patch.page_id = page_id;
  patch.offset = 50;
  patch.before_payload = {0x00};
  patch.after_payload = {0xFF};

  LogRecord record{};
  build_page_patch_record(txn1, patch, record);
  lsn_t update_lsn = log_mgr_->append_record(record);

  txn_mgr_->record_lsn(txn1, update_lsn);
  bpm_->update_dirtied_page_lsn(page_id, update_lsn);
  bpm_->unpin_page(page_id, false);

  ASSERT_TRUE(txn_mgr_->commit(txn1));

  {
    PageGuard pg = bpm_->fetch_page_guard(page_id);
    ASSERT_NE(pg.get_page(), nullptr);
    pg.get_page()->set_page_lsn(update_lsn);
  }

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
  AnalysisResult result = recovery.analyze();

  // Verify analysis found the record
  ASSERT_NE(result.min_rec_lsn, INVALID_LSN) << "Analysis should find PAGE_UPDATE record";
  ASSERT_EQ(result.dirty_page_table.size(), 1u);
  ASSERT_NE(result.dirty_page_table.find(page_id), result.dirty_page_table.end());

  PageGuard page_guard = bpm_->fetch_page_guard(page_id);
  ASSERT_NE(page_guard.get_page(), nullptr);
  // Verify page was not modified (already had correct page_lsn)
  EXPECT_EQ((unsigned char)page_guard.get_page()->get_data()[50], 0xFFu);
  EXPECT_EQ(page_guard.get_page()->get_page_lsn(), update_lsn);
}

TEST_F(RecoveryManagerTest, RedoAppliesCompensationPatch) {
  uint64_t txn1 = txn_mgr_->begin();

  page_id_t page_id = 102;
  bpm_->new_page(page_id);

  {
    PageGuard pg = bpm_->fetch_page_guard(page_id);
    ASSERT_NE(pg.get_page(), nullptr);
    char* data = pg.get_page()->get_data();
    std::memset(data, 0, PAGE_SIZE);
    data[80] = 0x11;
  }

  lsn_t update_lsn = append_page_update(txn1, page_id, 80);
  bpm_->update_dirtied_page_lsn(page_id, update_lsn);
  bpm_->unpin_page(page_id, false);

  CompensationPatch clr_patch{};
  clr_patch.page_id = page_id;
  clr_patch.offset = 80;
  clr_patch.undo_next_lsn = update_lsn;
  clr_patch.payload = {0x99};

  LogRecord clr_record{};
  build_compensation_patch_record(txn1, clr_patch, clr_record);
  clr_record.header.prev_lsn = txn_mgr_->get_last_lsn(txn1);
  lsn_t clr_lsn = log_mgr_->append_record(clr_record);
  txn_mgr_->record_lsn(txn1, clr_lsn);

  ASSERT_TRUE(txn_mgr_->commit(txn1));

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
  AnalysisResult result = recovery.analyze();

  // Verify analysis found both records
  ASSERT_NE(result.min_rec_lsn, INVALID_LSN) << "Analysis should find PAGE_UPDATE and COMPENSATION records";
  ASSERT_EQ(result.dirty_page_table.size(), 1u);
  ASSERT_NE(result.dirty_page_table.find(page_id), result.dirty_page_table.end());

  PageGuard page_guard = bpm_->fetch_page_guard(page_id);
  ASSERT_NE(page_guard.get_page(), nullptr);
  // Verify CLR patch was applied
  EXPECT_EQ((unsigned char)page_guard.get_page()->get_data()[80], 0x99u);
  EXPECT_EQ(page_guard.get_page()->get_page_lsn(), clr_lsn);
}

TEST_F(RecoveryManagerTest, RedoAppliesMultiplePatchesToSamePage) {
  uint64_t txn1 = txn_mgr_->begin();

  page_id_t page_id = 103;
  bpm_->new_page(page_id);

  {
    PageGuard pg = bpm_->fetch_page_guard(page_id);
    ASSERT_NE(pg.get_page(), nullptr);
    std::memset(pg.get_page()->get_data(), 0, PAGE_SIZE);
  }

  PagePatch patch1{};
  patch1.page_id = page_id;
  patch1.offset = 10;
  patch1.before_payload = {0x00};
  patch1.after_payload = {0xAA};

  LogRecord record1{};
  build_page_patch_record(txn1, patch1, record1);
  lsn_t lsn1 = log_mgr_->append_record(record1);
  txn_mgr_->record_lsn(txn1, lsn1);

  PagePatch patch2{};
  patch2.page_id = page_id;
  patch2.offset = 20;
  patch2.before_payload = {0x00};
  patch2.after_payload = {0xBB};

  LogRecord record2{};
  build_page_patch_record(txn1, patch2, record2);
  lsn_t lsn2 = log_mgr_->append_record(record2);
  txn_mgr_->record_lsn(txn1, lsn2);

  bpm_->update_dirtied_page_lsn(page_id, lsn1);
  bpm_->unpin_page(page_id, false);

  ASSERT_TRUE(txn_mgr_->commit(txn1));

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
  AnalysisResult result = recovery.analyze();

  // Verify analysis found both records
  ASSERT_NE(result.min_rec_lsn, INVALID_LSN) << "Analysis should find both PAGE_UPDATE records";
  ASSERT_EQ(result.dirty_page_table.size(), 1u);
  ASSERT_NE(result.dirty_page_table.find(page_id), result.dirty_page_table.end());

  PageGuard page_guard = bpm_->fetch_page_guard(page_id);
  ASSERT_NE(page_guard.get_page(), nullptr);
  // Verify both patches were applied in order
  EXPECT_EQ((unsigned char)page_guard.get_page()->get_data()[10], 0xAAu);
  EXPECT_EQ((unsigned char)page_guard.get_page()->get_data()[20], 0xBBu);
  EXPECT_EQ(page_guard.get_page()->get_page_lsn(), lsn2);
}

TEST_F(RecoveryManagerTest, RedoMaintainsPageLsnOrdering) {
  uint64_t txn1 = txn_mgr_->begin();

  page_id_t page_id = 105;
  bpm_->new_page(page_id);

  {
    PageGuard pg = bpm_->fetch_page_guard(page_id);
    ASSERT_NE(pg.get_page(), nullptr);
    std::memset(pg.get_page()->get_data(), 0, PAGE_SIZE);
  }

  PagePatch patch1{};
  patch1.page_id = page_id;
  patch1.offset = 0;
  patch1.before_payload = {0x00};
  patch1.after_payload = {0x11};

  LogRecord record1{};
  build_page_patch_record(txn1, patch1, record1);
  lsn_t lsn1 = log_mgr_->append_record(record1);

  PagePatch patch2{};
  patch2.page_id = page_id;
  patch2.offset = 10;
  patch2.before_payload = {0x00};
  patch2.after_payload = {0x22};

  LogRecord record2{};
  build_page_patch_record(txn1, patch2, record2);
  lsn_t lsn2 = log_mgr_->append_record(record2);

  bpm_->update_dirtied_page_lsn(page_id, lsn1);
  bpm_->unpin_page(page_id, false);

  txn_mgr_->record_lsn(txn1, lsn2);
  ASSERT_TRUE(txn_mgr_->commit(txn1));

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
  AnalysisResult result = recovery.analyze();

  // Verify analysis found both records
  ASSERT_NE(result.min_rec_lsn, INVALID_LSN) << "Analysis should find both PAGE_UPDATE records";
  ASSERT_EQ(result.dirty_page_table.size(), 1u);
  ASSERT_NE(result.dirty_page_table.find(page_id), result.dirty_page_table.end());

  PageGuard page_guard = bpm_->fetch_page_guard(page_id);
  ASSERT_NE(page_guard.get_page(), nullptr);
  // Verify page_lsn was set to the latest record LSN
  EXPECT_EQ(page_guard.get_page()->get_page_lsn(), lsn2);
  EXPECT_LT(lsn1, lsn2);
}

TEST_F(RecoveryManagerTest, RedoFullRecoveryIntegration) {
  uint64_t txn1 = txn_mgr_->begin();
  uint64_t txn2 = txn_mgr_->begin();

  page_id_t page_id1 = 106;
  page_id_t page_id2 = 107;
  bpm_->new_page(page_id1);
  bpm_->new_page(page_id2);

  {
    PageGuard pg1 = bpm_->fetch_page_guard(page_id1);
    PageGuard pg2 = bpm_->fetch_page_guard(page_id2);
    ASSERT_NE(pg1.get_page(), nullptr);
    ASSERT_NE(pg2.get_page(), nullptr);
    std::memset(pg1.get_page()->get_data(), 0, PAGE_SIZE);
    std::memset(pg2.get_page()->get_data(), 0, PAGE_SIZE);
    pg1.get_page()->get_data()[50] = 0xAA;
    pg2.get_page()->get_data()[50] = 0xBB;
  }

  PagePatch patch1{};
  patch1.page_id = page_id1;
  patch1.offset = 50;
  patch1.before_payload = {0xAA};
  patch1.after_payload = {0xCC};

  LogRecord record1{};
  build_page_patch_record(txn1, patch1, record1);
  record1.header.prev_lsn = txn_mgr_->get_last_lsn(txn1);
  lsn_t lsn1 = log_mgr_->append_record(record1);
  txn_mgr_->record_lsn(txn1, lsn1);

  PagePatch patch2{};
  patch2.page_id = page_id2;
  patch2.offset = 50;
  patch2.before_payload = {0xBB};
  patch2.after_payload = {0xDD};

  LogRecord record2{};
  build_page_patch_record(txn2, patch2, record2);
  record2.header.prev_lsn = txn_mgr_->get_last_lsn(txn2);
  lsn_t lsn2 = log_mgr_->append_record(record2);
  txn_mgr_->record_lsn(txn2, lsn2);

  ASSERT_TRUE(txn_mgr_->commit(txn1));

  bpm_->update_dirtied_page_lsn(page_id1, lsn1);
  bpm_->unpin_page(page_id1, false);
  bpm_->update_dirtied_page_lsn(page_id2, lsn2);
  bpm_->unpin_page(page_id2, false);

  ASSERT_TRUE(log_mgr_->flush_to_lsn(lsn2));

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
  AnalysisResult result = recovery.analyze();

  // Verify analysis found all records
  ASSERT_NE(result.min_rec_lsn, INVALID_LSN) << "Analysis should find both PAGE_UPDATE records";
  ASSERT_EQ(result.dirty_page_table.size(), 2u) << "Both pages should be in DPT";

  // Verify ATT correctly reflects transaction state
  EXPECT_EQ(result.active_transactions.size(), 1u);
  EXPECT_EQ(result.active_transactions.find(txn1), result.active_transactions.end()) << "txn1 should be committed";
  EXPECT_NE(result.active_transactions.find(txn2), result.active_transactions.end()) << "txn2 should be active";

  EXPECT_EQ(result.dirty_page_table[page_id1], lsn1);
  EXPECT_EQ(result.dirty_page_table[page_id2], lsn2);

  PageGuard pg1_recovered = bpm_->fetch_page_guard(page_id1);
  PageGuard pg2_recovered = bpm_->fetch_page_guard(page_id2);
  ASSERT_NE(pg1_recovered.get_page(), nullptr);
  ASSERT_NE(pg2_recovered.get_page(), nullptr);
  // txn1 committed: redo applies its patch and undo leaves it alone.
  EXPECT_EQ((unsigned char)pg1_recovered.get_page()->get_data()[50], 0xCCu);
  EXPECT_EQ(pg1_recovered.get_page()->get_page_lsn(), lsn1);
  // txn2 is still active (a loser): redo applies its patch, then undo rolls it
  // back to the before_payload via a CLR, so page_lsn advances past lsn2.
  EXPECT_EQ((unsigned char)pg2_recovered.get_page()->get_data()[50], 0xBBu);
  EXPECT_GT(pg2_recovered.get_page()->get_page_lsn(), lsn2);
}

TEST_F(RecoveryManagerTest, UndoAllocateExtentClearsGamBit) {
  page_id_t gam_page = 8;
  uint16_t bit = 5;
  bpm_->new_page(gam_page);

  uint64_t txn1 = txn_mgr_->begin();
  lsn_t alloc_lsn = append_gam_update(txn1, gam_page, bit, LogRecordType::ALLOCATE_EXTENT);

  // Put the page in its post-allocate state (bit set, page_lsn at the record) so
  // redo treats it as already applied and undo is the sole actor.
  {
    PageGuard guard = bpm_->fetch_page_guard(gam_page);
    GAMPage gam = load_page_layout<GAMPage>(guard.get_page());
    Bitmap bitmap(gam.bitmap, GAM_MAX_BITS);
    bitmap.set(bit);
    gam.page_lsn = alloc_lsn;
    store_page_layout(guard.get_page(), gam);
    guard.mark_dirty();
  }
  bpm_->update_dirtied_page_lsn(gam_page, alloc_lsn);
  ASSERT_TRUE(log_mgr_->flush_to_lsn(alloc_lsn));

  ASSERT_TRUE(gam_bit_set(gam_page, bit));

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
  recovery.analyze();

  EXPECT_FALSE(gam_bit_set(gam_page, bit));
  EXPECT_TRUE(wal_has_compensation(CompensationKind::GamClearBit));
}

TEST_F(RecoveryManagerTest, UndoDeallocateExtentResetsGamBit) {
  page_id_t gam_page = 8;
  uint16_t bit = 5;
  bpm_->new_page(gam_page);

  uint64_t txn1 = txn_mgr_->begin();
  lsn_t dealloc_lsn = append_gam_update(txn1, gam_page, bit, LogRecordType::DEALLOCATE_EXTENT);

  // Post-deallocate state: bit cleared, page_lsn at the record.
  {
    PageGuard guard = bpm_->fetch_page_guard(gam_page);
    GAMPage gam = load_page_layout<GAMPage>(guard.get_page());
    Bitmap bitmap(gam.bitmap, GAM_MAX_BITS);
    bitmap.clear(bit);
    gam.page_lsn = dealloc_lsn;
    store_page_layout(guard.get_page(), gam);
    guard.mark_dirty();
  }
  bpm_->update_dirtied_page_lsn(gam_page, dealloc_lsn);
  ASSERT_TRUE(log_mgr_->flush_to_lsn(dealloc_lsn));

  ASSERT_FALSE(gam_bit_set(gam_page, bit));

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
  recovery.analyze();

  EXPECT_TRUE(gam_bit_set(gam_page, bit));
  EXPECT_TRUE(wal_has_compensation(CompensationKind::GamSetBit));
}

TEST_F(RecoveryManagerTest, UndoIamAddExtentRemovesEntry) {
  page_id_t iam_page = 16;
  uint32_t extent = 42;
  bpm_->new_page(iam_page);

  uint64_t txn1 = txn_mgr_->begin();
  lsn_t add_lsn = append_iam_update(txn1, iam_page, extent, LogRecordType::IAM_ADD_EXTENT);

  // Post-add state: extent present, page_lsn at the record.
  {
    PageGuard guard = bpm_->fetch_page_guard(iam_page);
    IAMPage iam = load_page_layout<IAMPage>(guard.get_page());
    iam.add_extent(extent);
    iam.page_lsn = add_lsn;
    store_page_layout(guard.get_page(), iam);
    guard.mark_dirty();
  }
  bpm_->update_dirtied_page_lsn(iam_page, add_lsn);
  ASSERT_TRUE(log_mgr_->flush_to_lsn(add_lsn));

  ASSERT_TRUE(iam_has_extent(iam_page, extent));

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
  recovery.analyze();

  EXPECT_FALSE(iam_has_extent(iam_page, extent));
  EXPECT_TRUE(wal_has_compensation(CompensationKind::IamRemoveExtent));
}

TEST_F(RecoveryManagerTest, UndoIamRemoveExtentReAddsEntry) {
  page_id_t iam_page = 16;
  uint32_t extent = 42;
  bpm_->new_page(iam_page);

  uint64_t txn1 = txn_mgr_->begin();
  lsn_t remove_lsn = append_iam_update(txn1, iam_page, extent, LogRecordType::IAM_REMOVE_EXTENT);

  // Post-remove state: extent absent (fresh page), page_lsn at the record.
  {
    PageGuard guard = bpm_->fetch_page_guard(iam_page);
    IAMPage iam = load_page_layout<IAMPage>(guard.get_page());
    iam.page_lsn = remove_lsn;
    store_page_layout(guard.get_page(), iam);
    guard.mark_dirty();
  }
  bpm_->update_dirtied_page_lsn(iam_page, remove_lsn);
  ASSERT_TRUE(log_mgr_->flush_to_lsn(remove_lsn));

  ASSERT_FALSE(iam_has_extent(iam_page, extent));

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
  recovery.analyze();

  EXPECT_TRUE(iam_has_extent(iam_page, extent));
  EXPECT_TRUE(wal_has_compensation(CompensationKind::IamAddExtent));
}

TEST_F(RecoveryManagerTest, RollbackTransactionUndoesUpdateAndWritesEnd) {
  page_id_t page_id = 120;
  bpm_->new_page(page_id);

  uint64_t txn1 = txn_mgr_->begin();

  // Page currently holds the post-update value (0xEE at offset 30).
  {
    PageGuard pg = bpm_->fetch_page_guard(page_id);
    std::memset(pg.get_page()->get_data(), 0, PAGE_SIZE);
    pg.get_page()->get_data()[30] = 0xEE;
    pg.mark_dirty();
  }

  PagePatch patch{};
  patch.page_id = page_id;
  patch.offset = 30;
  patch.before_payload = {0x00};
  patch.after_payload = {0xEE};

  LogRecord record{};
  build_page_patch_record(txn1, patch, record);
  record.header.prev_lsn = txn_mgr_->get_last_lsn(txn1);
  lsn_t update_lsn = log_mgr_->append_record(record);
  txn_mgr_->record_lsn(txn1, update_lsn);

  rollback_mgr_->rollback_transaction(txn1, update_lsn);

  // Update reversed to the before-image; CLR and END logged.
  {
    PageGuard pg = bpm_->fetch_page_guard(page_id);
    EXPECT_EQ((unsigned char)pg.get_page()->get_data()[30], 0x00u);
  }
  EXPECT_TRUE(wal_has_compensation(CompensationKind::BytePatch));
  EXPECT_TRUE(wal_has_end(txn1));
}

}
