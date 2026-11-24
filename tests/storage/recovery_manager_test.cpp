#include "storage/recovery_manager.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <gtest/gtest.h>

#include "buffer/buffer_pool_manager.h"
#include "buffer/lru_replacer.h"
#include "catalog/catalog_manager.h"
#include "database_engine.h"
#include "sql/executor.h"
#include "sql/lexer.h"
#include "sql/parser.h"
#include "storage/checkpoint_manager.h"
#include "storage/disk_manager.h"
#include "storage/log_manager.h"
#include "storage/log_record.h"
#include "storage/page_allocator.h"
#include "storage/rollback_manager.h"
#include "storage/slotted_page.h"
#include "storage/storage_def.h"
#include "storage/page_utils.h"
#include "storage/table_manager.h"
#include "storage/tuple.h"
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
    SlottedPage sp = SlottedPage::init(reinterpret_cast<Byte*>(pg.get_body()));
    Byte initial[] = {'A', 'B', 'C'};
    ASSERT_EQ(sp.insert_tuple(initial, 3), 0u);
  }

  PagePatch patch{};
  patch.page_id = page_id;
  patch.offset = 0;
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

  ASSERT_NE(result.min_rec_lsn, INVALID_LSN) << "Analysis should find PAGE_UPDATE record";
  ASSERT_EQ(result.dirty_page_table.size(), 1u);
  ASSERT_NE(result.dirty_page_table.find(page_id), result.dirty_page_table.end());

  PageGuard recovered_guard = bpm_->fetch_page_guard(page_id);
  ASSERT_NE(recovered_guard.get_page(), nullptr);
  SlottedPage recovered_sp = SlottedPage::from_page(recovered_guard);
  uint32_t size = 0;
  const Byte* tuple_bytes = recovered_sp.get_tuple(0, &size);
  ASSERT_NE(tuple_bytes, nullptr);
  EXPECT_EQ(size, 3u);
  EXPECT_EQ(tuple_bytes[0], 0x58u);
  EXPECT_EQ(tuple_bytes[1], 0x59u);
  EXPECT_EQ(tuple_bytes[2], 0x5Au);
  EXPECT_EQ(recovered_guard.get_page()->get_page_lsn(), update_lsn);
}

TEST_F(RecoveryManagerTest, RedoSkipsAlreadyAppliedChanges) {
  uint64_t txn1 = txn_mgr_->begin();

  page_id_t page_id = 101;
  bpm_->new_page(page_id);

  {
    PageGuard pg = bpm_->fetch_page_guard(page_id);
    ASSERT_NE(pg.get_page(), nullptr);
    SlottedPage sp = SlottedPage::init(reinterpret_cast<Byte*>(pg.get_body()));
    Byte val[] = {0xFF};
    ASSERT_EQ(sp.insert_tuple(val, 1), 0u);
  }

  PagePatch patch{};
  patch.page_id = page_id;
  patch.offset = 0;
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

  ASSERT_NE(result.min_rec_lsn, INVALID_LSN) << "Analysis should find PAGE_UPDATE record";
  ASSERT_EQ(result.dirty_page_table.size(), 1u);
  ASSERT_NE(result.dirty_page_table.find(page_id), result.dirty_page_table.end());

  PageGuard page_guard = bpm_->fetch_page_guard(page_id);
  ASSERT_NE(page_guard.get_page(), nullptr);
  SlottedPage recovered_sp = SlottedPage::from_page(page_guard);
  uint32_t size = 0;
  const Byte* tuple_bytes = recovered_sp.get_tuple(0, &size);
  ASSERT_NE(tuple_bytes, nullptr);
  EXPECT_EQ(size, 1u);
  EXPECT_EQ(tuple_bytes[0], 0xFFu);
  EXPECT_EQ(page_guard.get_page()->get_page_lsn(), update_lsn);
}

TEST_F(RecoveryManagerTest, RedoAppliesCompensationPatch) {
  uint64_t txn1 = txn_mgr_->begin();

  page_id_t page_id = 102;
  bpm_->new_page(page_id);

  {
    PageGuard pg = bpm_->fetch_page_guard(page_id);
    ASSERT_NE(pg.get_page(), nullptr);
    SlottedPage::init(reinterpret_cast<Byte*>(pg.get_body()));
  }

  lsn_t update_lsn = append_page_update(txn1, page_id, 0);
  bpm_->update_dirtied_page_lsn(page_id, update_lsn);
  bpm_->unpin_page(page_id, false);

  CompensationPatch clr_patch{};
  clr_patch.page_id = page_id;
  clr_patch.offset = 0;
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

  ASSERT_NE(result.min_rec_lsn, INVALID_LSN) << "Analysis should find PAGE_UPDATE and COMPENSATION records";
  ASSERT_EQ(result.dirty_page_table.size(), 1u);
  ASSERT_NE(result.dirty_page_table.find(page_id), result.dirty_page_table.end());

  PageGuard page_guard = bpm_->fetch_page_guard(page_id);
  ASSERT_NE(page_guard.get_page(), nullptr);
  SlottedPage recovered_sp = SlottedPage::from_page(page_guard);
  uint32_t size = 0;
  const Byte* tuple_bytes = recovered_sp.get_tuple(0, &size);
  ASSERT_NE(tuple_bytes, nullptr);
  EXPECT_EQ(size, 1u);
  EXPECT_EQ(tuple_bytes[0], 0x99u);
  EXPECT_EQ(page_guard.get_page()->get_page_lsn(), clr_lsn);
}

TEST_F(RecoveryManagerTest, RedoAppliesMultiplePatchesToSamePage) {
  uint64_t txn1 = txn_mgr_->begin();

  page_id_t page_id = 103;
  bpm_->new_page(page_id);

  {
    PageGuard pg = bpm_->fetch_page_guard(page_id);
    ASSERT_NE(pg.get_page(), nullptr);
    SlottedPage::init(reinterpret_cast<Byte*>(pg.get_body()));
  }

  PagePatch patch1{};
  patch1.page_id = page_id;
  patch1.offset = 0;
  patch1.before_payload = {};
  patch1.after_payload = {0xAA};

  LogRecord record1{};
  build_page_patch_record(txn1, patch1, record1);
  lsn_t lsn1 = log_mgr_->append_record(record1);
  txn_mgr_->record_lsn(txn1, lsn1);

  PagePatch patch2{};
  patch2.page_id = page_id;
  patch2.offset = 1;
  patch2.before_payload = {};
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

  ASSERT_NE(result.min_rec_lsn, INVALID_LSN) << "Analysis should find both PAGE_UPDATE records";
  ASSERT_EQ(result.dirty_page_table.size(), 1u);
  ASSERT_NE(result.dirty_page_table.find(page_id), result.dirty_page_table.end());

  PageGuard page_guard = bpm_->fetch_page_guard(page_id);
  ASSERT_NE(page_guard.get_page(), nullptr);
  SlottedPage recovered_sp = SlottedPage::from_page(page_guard);
  uint32_t size0 = 0;
  uint32_t size1 = 0;
  const Byte* tuple0 = recovered_sp.get_tuple(0, &size0);
  const Byte* tuple1 = recovered_sp.get_tuple(1, &size1);
  ASSERT_NE(tuple0, nullptr);
  ASSERT_NE(tuple1, nullptr);
  EXPECT_EQ(tuple0[0], 0xAAu);
  EXPECT_EQ(tuple1[0], 0xBBu);
  EXPECT_EQ(page_guard.get_page()->get_page_lsn(), lsn2);
}

TEST_F(RecoveryManagerTest, RedoMaintainsPageLsnOrdering) {
  uint64_t txn1 = txn_mgr_->begin();

  page_id_t page_id = 105;
  bpm_->new_page(page_id);

  {
    PageGuard pg = bpm_->fetch_page_guard(page_id);
    ASSERT_NE(pg.get_page(), nullptr);
    SlottedPage::init(reinterpret_cast<Byte*>(pg.get_body()));
  }

  PagePatch patch1{};
  patch1.page_id = page_id;
  patch1.offset = 0;
  patch1.before_payload = {};
  patch1.after_payload = {0x11};

  LogRecord record1{};
  build_page_patch_record(txn1, patch1, record1);
  lsn_t lsn1 = log_mgr_->append_record(record1);

  PagePatch patch2{};
  patch2.page_id = page_id;
  patch2.offset = 1;
  patch2.before_payload = {};
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

  ASSERT_NE(result.min_rec_lsn, INVALID_LSN) << "Analysis should find both PAGE_UPDATE records";
  ASSERT_EQ(result.dirty_page_table.size(), 1u);
  ASSERT_NE(result.dirty_page_table.find(page_id), result.dirty_page_table.end());

  PageGuard page_guard = bpm_->fetch_page_guard(page_id);
  ASSERT_NE(page_guard.get_page(), nullptr);
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
    SlottedPage sp1 = SlottedPage::init(reinterpret_cast<Byte*>(pg1.get_body()));
    SlottedPage sp2 = SlottedPage::init(reinterpret_cast<Byte*>(pg2.get_body()));
    Byte init1[] = {0xAA};
    Byte init2[] = {0xBB};
    ASSERT_EQ(sp1.insert_tuple(init1, 1), 0u);
    ASSERT_EQ(sp2.insert_tuple(init2, 1), 0u);
  }

  PagePatch patch1{};
  patch1.page_id = page_id1;
  patch1.offset = 0;
  patch1.before_payload = {0xAA};
  patch1.after_payload = {0xCC};

  LogRecord record1{};
  build_page_patch_record(txn1, patch1, record1);
  record1.header.prev_lsn = txn_mgr_->get_last_lsn(txn1);
  lsn_t lsn1 = log_mgr_->append_record(record1);
  txn_mgr_->record_lsn(txn1, lsn1);

  PagePatch patch2{};
  patch2.page_id = page_id2;
  patch2.offset = 0;
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

  ASSERT_NE(result.min_rec_lsn, INVALID_LSN) << "Analysis should find both PAGE_UPDATE records";
  ASSERT_EQ(result.dirty_page_table.size(), 2u) << "Both pages should be in DPT";

  EXPECT_EQ(result.active_transactions.size(), 1u);
  EXPECT_EQ(result.active_transactions.find(txn1), result.active_transactions.end()) << "txn1 should be committed";
  EXPECT_NE(result.active_transactions.find(txn2), result.active_transactions.end()) << "txn2 should be active";

  EXPECT_EQ(result.dirty_page_table[page_id1], lsn1);
  EXPECT_EQ(result.dirty_page_table[page_id2], lsn2);

  PageGuard pg1_recovered = bpm_->fetch_page_guard(page_id1);
  PageGuard pg2_recovered = bpm_->fetch_page_guard(page_id2);
  ASSERT_NE(pg1_recovered.get_page(), nullptr);
  ASSERT_NE(pg2_recovered.get_page(), nullptr);
  SlottedPage sp1_rec = SlottedPage::from_page(pg1_recovered);
  SlottedPage sp2_rec = SlottedPage::from_page(pg2_recovered);
  uint32_t sz1 = 0;
  uint32_t sz2 = 0;
  const Byte* t1 = sp1_rec.get_tuple(0, &sz1);
  const Byte* t2 = sp2_rec.get_tuple(0, &sz2);
  ASSERT_NE(t1, nullptr);
  ASSERT_NE(t2, nullptr);
  EXPECT_EQ(t1[0], 0xCCu);
  EXPECT_EQ(pg1_recovered.get_page()->get_page_lsn(), lsn1);
  EXPECT_EQ(t2[0], 0xBBu);
  EXPECT_GT(pg2_recovered.get_page()->get_page_lsn(), lsn2);
}

TEST_F(RecoveryManagerTest, RollbackTransactionUndoesUpdateAndWritesEnd) {
  page_id_t page_id = 120;
  bpm_->new_page(page_id);

  uint64_t txn1 = txn_mgr_->begin();

  {
    PageGuard pg = bpm_->fetch_page_guard(page_id);
    SlottedPage sp = SlottedPage::init(reinterpret_cast<Byte*>(pg.get_body()));
    Byte updated_tuple[] = {0xEE, 0xEF};
    ASSERT_EQ(sp.insert_tuple(updated_tuple, 2), 0u);
    pg.mark_dirty();
  }

  PagePatch patch{};
  patch.page_id = page_id;
  patch.offset = 0;
  patch.before_payload = {0xAA, 0xBB};
  patch.after_payload = {0xEE, 0xEF};

  LogRecord record{};
  build_page_patch_record(txn1, patch, record);
  record.header.prev_lsn = txn_mgr_->get_last_lsn(txn1);
  lsn_t update_lsn = log_mgr_->append_record(record);
  txn_mgr_->record_lsn(txn1, update_lsn);

  rollback_mgr_->rollback_transaction(txn1, update_lsn);

  {
    PageGuard pg = bpm_->fetch_page_guard(page_id);
    SlottedPage sp = SlottedPage::from_page(pg);
    uint32_t size = 0;
    const Byte* tuple_bytes = sp.get_tuple(0, &size);
    ASSERT_NE(tuple_bytes, nullptr);
    EXPECT_EQ(size, 2u);
    EXPECT_EQ(tuple_bytes[0], 0xAAu);
    EXPECT_EQ(tuple_bytes[1], 0xBBu);
  }
  EXPECT_TRUE(wal_has_compensation(CompensationKind::BytePatch));
  EXPECT_TRUE(wal_has_end(txn1));
}

TEST_F(RecoveryManagerTest, SlottedPageRedoAndUndoInsertRowPatches) {
  page_id_t page_id = 130;
  bpm_->new_page(page_id);
  {
    PageGuard pg = bpm_->fetch_page_guard(page_id);
    SlottedPage::init(reinterpret_cast<Byte*>(pg.get_body()));
    pg.mark_dirty();
  }
  bpm_->unpin_page(page_id, false);

  uint64_t committed_txn = txn_mgr_->begin();
  PagePatch insert_slot0{};
  insert_slot0.page_id = page_id;
  insert_slot0.offset = 0;
  insert_slot0.before_payload = {};
  insert_slot0.after_payload = {'A', 'l', 'i', 'c', 'e'};

  LogRecord rec0{};
  build_page_patch_record(committed_txn, insert_slot0, rec0);
  rec0.header.prev_lsn = txn_mgr_->get_last_lsn(committed_txn);
  lsn_t lsn0 = log_mgr_->append_record(rec0);
  txn_mgr_->record_lsn(committed_txn, lsn0);
  bpm_->update_dirtied_page_lsn(page_id, lsn0);
  ASSERT_TRUE(txn_mgr_->commit(committed_txn));

  uint64_t loser_txn = txn_mgr_->begin();
  PagePatch insert_slot1{};
  insert_slot1.page_id = page_id;
  insert_slot1.offset = 1;
  insert_slot1.before_payload = {};
  insert_slot1.after_payload = {'B', 'o', 'b'};

  LogRecord rec1{};
  build_page_patch_record(loser_txn, insert_slot1, rec1);
  rec1.header.prev_lsn = txn_mgr_->get_last_lsn(loser_txn);
  lsn_t lsn1 = log_mgr_->append_record(rec1);
  txn_mgr_->record_lsn(loser_txn, lsn1);
  ASSERT_TRUE(log_mgr_->flush_to_lsn(lsn1));

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
  recovery.analyze();

  PageGuard pg = bpm_->fetch_page_guard(page_id);
  SlottedPage sp = SlottedPage::from_page(pg);
  uint32_t sz0 = 0;
  const Byte* t0 = sp.get_tuple(0, &sz0);
  ASSERT_NE(t0, nullptr);
  EXPECT_EQ(sz0, 5u);
  EXPECT_EQ(std::memcmp(t0, "Alice", 5), 0);

  uint32_t sz1 = 0;
  EXPECT_EQ(sp.get_tuple(1, &sz1), nullptr);
  EXPECT_TRUE(wal_has_end(loser_txn));
}

TEST_F(RecoveryManagerTest, SlottedPageRedoAndUndoDeleteRowPatches) {
  page_id_t page_id = 131;
  bpm_->new_page(page_id);

  {
    PageGuard pg = bpm_->fetch_page_guard(page_id);
    SlottedPage sp = SlottedPage::init(reinterpret_cast<Byte*>(pg.get_body()));
    Byte row0[] = {'R', 'o', 'w', '0'};
    Byte row1[] = {'R', 'o', 'w', '1'};
    ASSERT_EQ(sp.insert_tuple(row0, 4), 0u);
    ASSERT_EQ(sp.insert_tuple(row1, 4), 1u);
    pg.mark_dirty();
  }
  bpm_->unpin_page(page_id, false);

  uint64_t committed_delete_txn = txn_mgr_->begin();
  PagePatch del_slot0{};
  del_slot0.page_id = page_id;
  del_slot0.offset = 0;
  del_slot0.before_payload = {'R', 'o', 'w', '0'};
  del_slot0.after_payload = {};

  LogRecord del_rec0{};
  build_page_patch_record(committed_delete_txn, del_slot0, del_rec0);
  del_rec0.header.prev_lsn = txn_mgr_->get_last_lsn(committed_delete_txn);
  lsn_t del_lsn0 = log_mgr_->append_record(del_rec0);
  txn_mgr_->record_lsn(committed_delete_txn, del_lsn0);
  bpm_->update_dirtied_page_lsn(page_id, del_lsn0);
  ASSERT_TRUE(txn_mgr_->commit(committed_delete_txn));

  uint64_t loser_delete_txn = txn_mgr_->begin();
  PagePatch del_slot1{};
  del_slot1.page_id = page_id;
  del_slot1.offset = 1;
  del_slot1.before_payload = {'R', 'o', 'w', '1'};
  del_slot1.after_payload = {};

  LogRecord del_rec1{};
  build_page_patch_record(loser_delete_txn, del_slot1, del_rec1);
  del_rec1.header.prev_lsn = txn_mgr_->get_last_lsn(loser_delete_txn);
  lsn_t del_lsn1 = log_mgr_->append_record(del_rec1);
  txn_mgr_->record_lsn(loser_delete_txn, del_lsn1);
  ASSERT_TRUE(log_mgr_->flush_to_lsn(del_lsn1));

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
  recovery.analyze();

  PageGuard pg = bpm_->fetch_page_guard(page_id);
  SlottedPage sp = SlottedPage::from_page(pg);
  uint32_t sz0 = 0;
  EXPECT_EQ(sp.get_tuple(0, &sz0), nullptr);

  uint32_t sz1 = 0;
  const Byte* t1 = sp.get_tuple(1, &sz1);
  ASSERT_NE(t1, nullptr);
  EXPECT_EQ(sz1, 4u);
  EXPECT_EQ(std::memcmp(t1, "Row1", 4), 0);
  EXPECT_TRUE(wal_has_end(loser_delete_txn));
}

TEST_F(RecoveryManagerTest, RedoMaterializesPageNeverFlushedToDisk) {
  page_id_t unmaterialized_page_id = 200;
  ASSERT_EQ(disk_mgr_->get_file_size_in_pages(), 0);

  uint64_t txn1 = txn_mgr_->begin();
  PagePatch patch{};
  patch.page_id = unmaterialized_page_id;
  patch.offset = 0;
  patch.before_payload = {};
  patch.after_payload = {'N', 'e', 'w', 'P', 'a', 'g', 'e'};

  LogRecord rec{};
  build_page_patch_record(txn1, patch, rec);
  rec.header.prev_lsn = txn_mgr_->get_last_lsn(txn1);
  lsn_t update_lsn = log_mgr_->append_record(rec);
  txn_mgr_->record_lsn(txn1, update_lsn);
  ASSERT_TRUE(txn_mgr_->commit(txn1));

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
  recovery.analyze();

  PageGuard pg = bpm_->fetch_page_guard(unmaterialized_page_id);
  ASSERT_NE(pg.get_page(), nullptr);
  SlottedPage sp = SlottedPage::from_page(pg);
  uint32_t sz = 0;
  const Byte* tuple_bytes = sp.get_tuple(0, &sz);
  ASSERT_NE(tuple_bytes, nullptr);
  EXPECT_EQ(sz, 7u);
  EXPECT_EQ(std::memcmp(tuple_bytes, "NewPage", 7), 0);
  EXPECT_EQ(pg.get_page()->get_page_lsn(), update_lsn);
}

// Simulates a recovery that crashed mid-rollback: the loser already has a CLR
// compensating its second update (undo_next_lsn pointing back past it) but no
// END. Recovery must resume — skip the already-compensated update via
// undo_next_lsn, undo the remaining one, and finish with END.
TEST_F(RecoveryManagerTest, RecoveryResumesInterruptedRollbackSkippingCompensatedUpdate) {
  page_id_t page_id = 140;
  bpm_->new_page(page_id);

  uint64_t loser = txn_mgr_->begin();

  PagePatch upd0{};
  upd0.page_id = page_id;
  upd0.offset = 0;
  upd0.before_payload = {};
  upd0.after_payload = {'A', 'A'};
  LogRecord rec0{};
  build_page_patch_record(loser, upd0, rec0);
  rec0.header.prev_lsn = txn_mgr_->get_last_lsn(loser);
  lsn_t lsn0 = log_mgr_->append_record(rec0);
  txn_mgr_->record_lsn(loser, lsn0);

  PagePatch upd1{};
  upd1.page_id = page_id;
  upd1.offset = 1;
  upd1.before_payload = {};
  upd1.after_payload = {'B', 'B'};
  LogRecord rec1{};
  build_page_patch_record(loser, upd1, rec1);
  rec1.header.prev_lsn = txn_mgr_->get_last_lsn(loser);
  lsn_t lsn1 = log_mgr_->append_record(rec1);
  txn_mgr_->record_lsn(loser, lsn1);

  // CLR from the prior (crashed) recovery: compensated slot 1, resumes at slot 0.
  CompensationPatch clr{};
  clr.page_id = page_id;
  clr.kind = CompensationKind::BytePatch;
  clr.offset = 1;
  clr.payload = {};
  clr.undo_next_lsn = lsn0;
  LogRecord clr_rec{};
  build_compensation_patch_record(loser, clr, clr_rec);
  clr_rec.header.prev_lsn = txn_mgr_->get_last_lsn(loser);
  lsn_t clr_lsn = log_mgr_->append_record(clr_rec);
  txn_mgr_->record_lsn(loser, clr_lsn);

  // Page reflects both inserts with slot 1 already compensated, stamped at the CLR LSN.
  {
    PageGuard pg = bpm_->fetch_page_guard(page_id);
    SlottedPage sp = SlottedPage::init(reinterpret_cast<Byte*>(pg.get_body()));
    Byte a[] = {'A', 'A'};
    Byte b[] = {'B', 'B'};
    sp.insert_tuple(a, 2);
    sp.insert_tuple(b, 2);
    sp.delete_tuple(1);
    pg.get_page()->set_page_lsn(clr_lsn);
    pg.mark_dirty();
  }
  ASSERT_TRUE(log_mgr_->flush_to_lsn(clr_lsn));

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
  recovery.analyze();

  PageGuard pg = bpm_->fetch_page_guard(page_id);
  SlottedPage sp = SlottedPage::from_page(pg);
  uint32_t sz = 0;
  EXPECT_EQ(sp.get_tuple(0, &sz), nullptr) << "slot 0 should be undone on resume";
  EXPECT_EQ(sp.get_tuple(1, &sz), nullptr) << "slot 1 stays compensated";
  EXPECT_TRUE(wal_has_end(loser)) << "rollback completed with END";

  // One new CLR (for slot 0) on top of the pre-existing one — slot 1 was skipped
  // via undo_next_lsn, not compensated a second time.
  int clr_count = 0;
  for (const auto& r : log_mgr_->read_records()) {
    if (r.header.type == LogRecordType::COMPENSATION) {
      clr_count++;
    }
  }
  EXPECT_EQ(clr_count, 2);
}

class FaultInjectingDiskManager : public IDiskManager {
 public:
  explicit FaultInjectingDiskManager(const std::string& db_path)
      : inner_(db_path) {}

  IOResult write_page(page_id_t page_id, const char* data) override {
    if (power_lost_) {
      ++dropped_writes_;
      return IOResult::SUCCESS;
    }
    if (write_limit_ >= 0) {
      if (write_limit_ == 0) {
        power_lost_ = true;
        ++dropped_writes_;
        return IOResult::WRITE_ERROR;
      }
      --write_limit_;
    }
    return inner_.write_page(page_id, data);
  }

  IOResult read_page(page_id_t page_id, char* buffer) override {
    return inner_.read_page(page_id, buffer);
  }

  page_id_t get_file_size_in_pages() override {
    return inner_.get_file_size_in_pages();
  }

  IOStats get_io_stats() const override {
    return inner_.get_io_stats();
  }

  void reset_io_stats() override {
    inner_.reset_io_stats();
  }

  bool sync() override {
    if (power_lost_) {
      return true;
    }
    return inner_.sync();
  }

  void simulate_power_loss() {
    power_lost_ = true;
  }

  void restore_power() {
    power_lost_ = false;
    write_limit_ = -1;
  }

  void fail_after_n_writes(int n) {
    write_limit_ = n;
  }

  uint64_t get_dropped_writes() const {
    return dropped_writes_;
  }

 private:
  DiskManager inner_;
  bool power_lost_ = false;
  int write_limit_ = -1;
  uint64_t dropped_writes_ = 0;
};

class FaultInjectionRecoveryE2ETest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = "test_e2e_recovery_dir_" + std::to_string(e2e_counter_++);
    std::filesystem::remove_all(test_dir_);
    std::filesystem::create_directories(test_dir_);
    db_path_ = (std::filesystem::path(test_dir_) / "e2e_recovery.db").string();
    wal_prefix_ = db_path_ + ".wal";

    fault_disk_ = std::make_unique<FaultInjectingDiskManager>(db_path_);
    start_full_stack();
    create_users_table();
  }

  void TearDown() override {
    if (fault_disk_) {
      fault_disk_->restore_power();
    }
    shutdown_stack();
    fault_disk_.reset();
    std::filesystem::remove_all(test_dir_);
  }

  Schema users_schema() const {
    std::vector<Column> cols;
    cols.emplace_back("id", DataType::INTEGER, 4, 0);
    cols.emplace_back("name", DataType::VARCHAR, 32, 4);
    return Schema(cols);
  }

  void start_full_stack() {
    bpm_ = std::make_unique<BufferPoolManager>(*fault_disk_, pool_size_, std::make_unique<LRUPageReplacer>());
    log_mgr_ = std::make_unique<LogManager>(wal_prefix_, wal_segment_size_, 60000);
    rollback_mgr_ = std::make_unique<RollbackManager>(*log_mgr_, *bpm_, db_path_);
    txn_mgr_ = std::make_unique<TransactionManager>(*log_mgr_, *rollback_mgr_);
    page_allocator_ = std::make_unique<PageAllocator>(*bpm_, *log_mgr_, *txn_mgr_);
    catalog_mgr_ = std::make_unique<CatalogManager>(*bpm_, *page_allocator_, *txn_mgr_);
    catalog_mgr_->init();
    table_mgr_ = std::make_unique<TableManager>(*bpm_, *catalog_mgr_, *log_mgr_, *txn_mgr_, *page_allocator_);
    executor_ = std::make_unique<Executor>(*catalog_mgr_, *table_mgr_, *txn_mgr_);
  }

  void shutdown_stack() {
    executor_.reset();
    table_mgr_.reset();
    catalog_mgr_.reset();
    page_allocator_.reset();
    txn_mgr_.reset();
    rollback_mgr_.reset();
    bpm_.reset();
    log_mgr_.reset();
  }

  void reconfigure_and_restart(size_t pool_size, uint32_t wal_segment_size) {
    ASSERT_TRUE(bpm_->checkpoint());
    ASSERT_TRUE(log_mgr_->flush_to_lsn(log_mgr_->get_next_lsn()));
    shutdown_stack();
    pool_size_ = pool_size;
    wal_segment_size_ = wal_segment_size;
    start_full_stack();
  }

  ExecutionResult execute_sql(const std::string& sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    Parser parser(std::move(tokens));
    auto ast = parser.parse();
    return executor_->execute(ast.get());
  }

  std::string build_orders_batch_insert(int start_id, int count) const {
    std::ostringstream sql;
    sql << "INSERT INTO orders VALUES ";
    for (int i = 0; i < count; ++i) {
      int id = start_id + i;
      if (i > 0) {
        sql << ", ";
      }
      double amount = id * 1.25 + 0.5;
      sql << "(" << id << ", 'customer_" << id << "', "
          << std::fixed << std::setprecision(2) << amount << ", "
          << (id % 2 == 0 ? "TRUE" : "FALSE") << ")";
    }
    return sql.str();
  }

  void create_users_table() {
    ASSERT_TRUE(catalog_mgr_->create_table("users", users_schema()));
    ASSERT_TRUE(bpm_->checkpoint());
    ASSERT_TRUE(log_mgr_->flush_to_lsn(log_mgr_->get_next_lsn()));
  }

  void simulate_crash_and_recover() {
    lsn_t durable_wal_lsn = log_mgr_->get_flushed_lsn();
    uint32_t active_seg = log_mgr_->get_current_segment_id();
    std::string active_wal_file = wal_prefix_ + "." + std::to_string(active_seg);

    fault_disk_->simulate_power_loss();
    shutdown_stack();

    if (std::filesystem::exists(active_wal_file) && durable_wal_lsn > 0) {
      size_t durable_offset = static_cast<size_t>(durable_wal_lsn % wal_segment_size_);
      std::filesystem::resize_file(active_wal_file, durable_offset);
    }

    fault_disk_->restore_power();

    bpm_ = std::make_unique<BufferPoolManager>(*fault_disk_, pool_size_, std::make_unique<LRUPageReplacer>());
    log_mgr_ = std::make_unique<LogManager>(wal_prefix_, wal_segment_size_, 60000);
    rollback_mgr_ = std::make_unique<RollbackManager>(*log_mgr_, *bpm_, db_path_);
    txn_mgr_ = std::make_unique<TransactionManager>(*log_mgr_, *rollback_mgr_);

    RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
    recovery.analyze();

    page_allocator_ = std::make_unique<PageAllocator>(*bpm_, *log_mgr_, *txn_mgr_);
    catalog_mgr_ = std::make_unique<CatalogManager>(*bpm_, *page_allocator_, *txn_mgr_);
    catalog_mgr_->init();
    table_mgr_ = std::make_unique<TableManager>(*bpm_, *catalog_mgr_, *log_mgr_, *txn_mgr_, *page_allocator_);
    executor_ = std::make_unique<Executor>(*catalog_mgr_, *table_mgr_, *txn_mgr_);
  }

  std::pair<page_id_t, uint16_t> find_row_location(int32_t target_id) {
    Schema schema = users_schema();
    TableScanner scanner = table_mgr_->scan_table("users");
    while (scanner.next()) {
      Tuple tup = Tuple::deserialize(schema, scanner.get_tuple_data(), scanner.get_tuple_size());
      if (std::get<int32_t>(tup.get_value(0)) == target_id) {
        return {scanner.current_page_id(), scanner.current_slot_id()};
      }
    }
    return {INVALID_PAGE_ID, 0};
  }

  std::vector<std::pair<int32_t, std::string>> scan_users() {
    std::vector<std::pair<int32_t, std::string>> rows;
    Schema schema = users_schema();
    TableScanner scanner = table_mgr_->scan_table("users");
    while (scanner.next()) {
      Tuple tup = Tuple::deserialize(schema, scanner.get_tuple_data(), scanner.get_tuple_size());
      rows.emplace_back(std::get<int32_t>(tup.get_value(0)),
                        std::get<std::string>(tup.get_value(1)));
    }
    std::sort(rows.begin(), rows.end());
    return rows;
  }

  std::string test_dir_;
  std::string db_path_;
  std::string wal_prefix_;
  size_t pool_size_ = 32;
  uint32_t wal_segment_size_ = 1024 * 1024;
  std::unique_ptr<FaultInjectingDiskManager> fault_disk_;
  std::unique_ptr<BufferPoolManager> bpm_;
  std::unique_ptr<LogManager> log_mgr_;
  std::unique_ptr<RollbackManager> rollback_mgr_;
  std::unique_ptr<TransactionManager> txn_mgr_;
  std::unique_ptr<PageAllocator> page_allocator_;
  std::unique_ptr<CatalogManager> catalog_mgr_;
  std::unique_ptr<TableManager> table_mgr_;
  std::unique_ptr<Executor> executor_;
  static int e2e_counter_;
};

int FaultInjectionRecoveryE2ETest::e2e_counter_ = 0;

TEST_F(FaultInjectionRecoveryE2ETest, CommittedInsertAndDeleteCrashBeforeFlushRecoversViaScan) {
  uint64_t txn1 = txn_mgr_->begin();
  ASSERT_TRUE(table_mgr_->insert_row("users", Tuple({int32_t(1), std::string("Alice")}), txn1).has_value());
  ASSERT_TRUE(table_mgr_->insert_row("users", Tuple({int32_t(2), std::string("Bob")}), txn1).has_value());
  ASSERT_TRUE(table_mgr_->insert_row("users", Tuple({int32_t(3), std::string("Charlie")}), txn1).has_value());
  ASSERT_TRUE(txn_mgr_->commit(txn1));

  uint64_t txn2 = txn_mgr_->begin();
  auto bob_loc = find_row_location(2);
  ASSERT_NE(bob_loc.first, INVALID_PAGE_ID);
  ASSERT_TRUE(table_mgr_->delete_row("users", bob_loc.first, bob_loc.second, txn2));
  ASSERT_TRUE(table_mgr_->insert_row("users", Tuple({int32_t(4), std::string("Diana")}), txn2).has_value());
  ASSERT_TRUE(txn_mgr_->commit(txn2));

  simulate_crash_and_recover();
  EXPECT_GT(fault_disk_->get_dropped_writes(), 0u);

  auto rows = scan_users();
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0], std::make_pair(int32_t(1), std::string("Alice")));
  EXPECT_EQ(rows[1], std::make_pair(int32_t(3), std::string("Charlie")));
  EXPECT_EQ(rows[2], std::make_pair(int32_t(4), std::string("Diana")));
}

TEST_F(FaultInjectionRecoveryE2ETest, UncommittedInsertAndDeleteFlushedToDiskCrashRollsBackViaScan) {
  uint64_t txn1 = txn_mgr_->begin();
  ASSERT_TRUE(table_mgr_->insert_row("users", Tuple({int32_t(10), std::string("Committed1")}), txn1).has_value());
  ASSERT_TRUE(table_mgr_->insert_row("users", Tuple({int32_t(20), std::string("Committed2")}), txn1).has_value());
  ASSERT_TRUE(txn_mgr_->commit(txn1));
  ASSERT_TRUE(bpm_->checkpoint());

  uint64_t loser_txn = txn_mgr_->begin();
  auto loc10 = find_row_location(10);
  ASSERT_NE(loc10.first, INVALID_PAGE_ID);
  ASSERT_TRUE(table_mgr_->delete_row("users", loc10.first, loc10.second, loser_txn));
  ASSERT_TRUE(table_mgr_->insert_row("users", Tuple({int32_t(99), std::string("UncommittedGhost")}), loser_txn).has_value());

  ASSERT_TRUE(log_mgr_->flush_to_lsn(log_mgr_->get_next_lsn()));
  ASSERT_TRUE(bpm_->flush_all_pages());

  simulate_crash_and_recover();

  auto rows = scan_users();
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0], std::make_pair(int32_t(10), std::string("Committed1")));
  EXPECT_EQ(rows[1], std::make_pair(int32_t(20), std::string("Committed2")));
}

TEST_F(FaultInjectionRecoveryE2ETest, InterleavedWinnerAndLoserOnSamePageRecoversExactRows) {
  uint64_t winner_txn = txn_mgr_->begin();
  uint64_t loser_txn = txn_mgr_->begin();

  ASSERT_TRUE(table_mgr_->insert_row("users", Tuple({int32_t(1), std::string("Winner1")}), winner_txn).has_value());
  ASSERT_TRUE(table_mgr_->insert_row("users", Tuple({int32_t(2), std::string("Loser1")}), loser_txn).has_value());
  ASSERT_TRUE(table_mgr_->insert_row("users", Tuple({int32_t(3), std::string("Winner2")}), winner_txn).has_value());
  ASSERT_TRUE(table_mgr_->insert_row("users", Tuple({int32_t(4), std::string("Loser2")}), loser_txn).has_value());

  ASSERT_TRUE(txn_mgr_->commit(winner_txn));
  ASSERT_TRUE(log_mgr_->flush_to_lsn(log_mgr_->get_next_lsn()));

  simulate_crash_and_recover();

  auto rows = scan_users();
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0], std::make_pair(int32_t(1), std::string("Winner1")));
  EXPECT_EQ(rows[1], std::make_pair(int32_t(3), std::string("Winner2")));

  uint64_t post_crash_txn = txn_mgr_->begin();
  ASSERT_TRUE(table_mgr_->insert_row("users", Tuple({int32_t(5), std::string("PostCrash")}), post_crash_txn).has_value());
  ASSERT_TRUE(txn_mgr_->commit(post_crash_txn));

  auto rows_after = scan_users();
  ASSERT_EQ(rows_after.size(), 3u);
  EXPECT_EQ(rows_after[2], std::make_pair(int32_t(5), std::string("PostCrash")));
}

TEST_F(FaultInjectionRecoveryE2ETest, TornWalTailAndDiskWriteFaultRecoversPriorCommittedRows) {
  uint64_t txn1 = txn_mgr_->begin();
  ASSERT_TRUE(table_mgr_->insert_row("users", Tuple({int32_t(1), std::string("DurableAlice")}), txn1).has_value());
  ASSERT_TRUE(table_mgr_->insert_row("users", Tuple({int32_t(2), std::string("DurableBob")}), txn1).has_value());
  ASSERT_TRUE(txn_mgr_->commit(txn1));

  fault_disk_->fail_after_n_writes(1);

  uint64_t torn_txn = txn_mgr_->begin();
  ASSERT_TRUE(table_mgr_->insert_row("users", Tuple({int32_t(3), std::string("TornCharlie")}), torn_txn).has_value());
  ASSERT_TRUE(log_mgr_->flush_to_lsn(log_mgr_->get_next_lsn()));

  std::string wal_file = wal_prefix_ + ".0";
  size_t full_wal_size = static_cast<size_t>(std::filesystem::file_size(wal_file));
  ASSERT_GT(full_wal_size, 16u);

  fault_disk_->simulate_power_loss();
  shutdown_stack();

  std::filesystem::resize_file(wal_file, full_wal_size - 6);

  fault_disk_->restore_power();
  bpm_ = std::make_unique<BufferPoolManager>(*fault_disk_, 32, std::make_unique<LRUPageReplacer>());
  log_mgr_ = std::make_unique<LogManager>(wal_prefix_, 1024 * 1024, 60000);
  rollback_mgr_ = std::make_unique<RollbackManager>(*log_mgr_, *bpm_, db_path_);
  txn_mgr_ = std::make_unique<TransactionManager>(*log_mgr_, *rollback_mgr_);

  RecoveryManager recovery(db_path_, *log_mgr_, *bpm_, *rollback_mgr_);
  recovery.analyze();

  page_allocator_ = std::make_unique<PageAllocator>(*bpm_, *log_mgr_, *txn_mgr_);
  catalog_mgr_ = std::make_unique<CatalogManager>(*bpm_, *page_allocator_, *txn_mgr_);
  catalog_mgr_->init();
  table_mgr_ = std::make_unique<TableManager>(*bpm_, *catalog_mgr_, *log_mgr_, *txn_mgr_, *page_allocator_);

  auto rows = scan_users();
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0], std::make_pair(int32_t(1), std::string("DurableAlice")));
  EXPECT_EQ(rows[1], std::make_pair(int32_t(2), std::string("DurableBob")));
}

TEST_F(FaultInjectionRecoveryE2ETest, FuzzyCheckpointWithWinnerAndLoserRecoversViaScan) {
  uint64_t txn1 = txn_mgr_->begin();
  uint64_t txn2 = txn_mgr_->begin();

  ASSERT_TRUE(table_mgr_->insert_row("users", Tuple({int32_t(1), std::string("PreCkptWinner")}), txn1).has_value());
  ASSERT_TRUE(table_mgr_->insert_row("users", Tuple({int32_t(2), std::string("PreCkptLoser")}), txn2).has_value());

  CheckpointManager ckpt_mgr(db_path_, *bpm_, *log_mgr_, *txn_mgr_, 60000, 1024 * 1024);
  ASSERT_TRUE(ckpt_mgr.run_checkpoint());

  ASSERT_TRUE(txn_mgr_->commit(txn1));

  uint64_t txn3 = txn_mgr_->begin();
  ASSERT_TRUE(table_mgr_->insert_row("users", Tuple({int32_t(3), std::string("PostCkptWinner")}), txn3).has_value());
  ASSERT_TRUE(txn_mgr_->commit(txn3));

  ASSERT_TRUE(table_mgr_->insert_row("users", Tuple({int32_t(4), std::string("PostCkptLoser")}), txn2).has_value());
  ASSERT_TRUE(log_mgr_->flush_to_lsn(log_mgr_->get_next_lsn()));

  simulate_crash_and_recover();

  auto rows = scan_users();
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0], std::make_pair(int32_t(1), std::string("PreCkptWinner")));
  EXPECT_EQ(rows[1], std::make_pair(int32_t(3), std::string("PostCkptWinner")));
}

TEST_F(FaultInjectionRecoveryE2ETest, VoluntaryAbortRollsBackInsertsViaScan) {
  uint64_t committed = txn_mgr_->begin();
  ASSERT_TRUE(table_mgr_->insert_row("users", Tuple({int32_t(1), std::string("Keeper")}), committed).has_value());
  ASSERT_TRUE(txn_mgr_->commit(committed));

  uint64_t aborted = txn_mgr_->begin();
  ASSERT_TRUE(table_mgr_->insert_row("users", Tuple({int32_t(2), std::string("Ghost1")}), aborted).has_value());
  ASSERT_TRUE(table_mgr_->insert_row("users", Tuple({int32_t(3), std::string("Ghost2")}), aborted).has_value());
  ASSERT_TRUE(txn_mgr_->abort(aborted));

  auto rows = scan_users();
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0], std::make_pair(int32_t(1), std::string("Keeper")));
}

TEST_F(FaultInjectionRecoveryE2ETest, SqlStress500RowsMultiSegmentSmallPoolCrashAndRecover) {
  reconfigure_and_restart(6, 4096);

  auto create_res = execute_sql(
      "CREATE TABLE orders (id INT, customer VARCHAR(32), amount FLOAT, active BOOL)");
  ASSERT_TRUE(create_res.success) << create_res.error_message;
  ASSERT_TRUE(bpm_->checkpoint());

  for (int batch = 0; batch < 10; ++batch) {
    int start_id = batch * 50 + 1;
    auto ins_res = execute_sql(build_orders_batch_insert(start_id, 50));
    ASSERT_TRUE(ins_res.success) << ins_res.error_message;
    EXPECT_EQ(ins_res.affected_rows, 50u);

    if (batch == 4) {
      CheckpointManager ckpt_mgr(db_path_, *bpm_, *log_mgr_, *txn_mgr_, 60000, 1024 * 1024);
      ASSERT_TRUE(ckpt_mgr.run_checkpoint());
    }
  }

  for (int id = 5; id <= 500; id += 5) {
    auto del_res = execute_sql("DELETE FROM orders WHERE id = " + std::to_string(id));
    ASSERT_TRUE(del_res.success) << del_res.error_message;
    EXPECT_EQ(del_res.affected_rows, 1u);
  }

  uint64_t loser_txn = txn_mgr_->begin();
  for (int ghost_id = 1000; ghost_id < 1060; ++ghost_id) {
    Tuple ghost_row({int32_t(ghost_id),
                     std::string("ghost_" + std::to_string(ghost_id)),
                     double(999.99),
                     bool(true)});
    ASSERT_TRUE(table_mgr_->insert_row("orders", ghost_row, loser_txn).has_value());
  }
  ASSERT_TRUE(log_mgr_->flush_to_lsn(log_mgr_->get_next_lsn()));
  EXPECT_GT(log_mgr_->get_current_segment_id(), 1u);
  EXPECT_GT(bpm_->get_cache_stats().dirty_evictions, 0u);

  simulate_crash_and_recover();

  auto select_res = execute_sql("SELECT id, customer, amount, active FROM orders");
  ASSERT_TRUE(select_res.success) << select_res.error_message;
  ASSERT_EQ(select_res.rows.size(), 400u);

  std::vector<int32_t> recovered_ids;
  recovered_ids.reserve(select_res.rows.size());
  for (const auto& tuple : select_res.rows) {
    int32_t id = std::get<int32_t>(tuple.get_value(0));
    std::string customer = std::get<std::string>(tuple.get_value(1));
    bool active = std::get<bool>(tuple.get_value(3));

    EXPECT_NE(id % 5, 0) << "Committed deleted row id=" << id << " reappeared";
    EXPECT_LT(id, 1000) << "Uncommitted loser row id=" << id << " was not rolled back";
    EXPECT_EQ(customer, "customer_" + std::to_string(id));
    EXPECT_EQ(active, (id % 2 == 0));
    recovered_ids.push_back(id);
  }

  std::sort(recovered_ids.begin(), recovered_ids.end());
  size_t expected_idx = 0;
  for (int id = 1; id <= 500; ++id) {
    if (id % 5 == 0) {
      continue;
    }
    ASSERT_LT(expected_idx, recovered_ids.size());
    EXPECT_EQ(recovered_ids[expected_idx], id);
    ++expected_idx;
  }
  EXPECT_EQ(expected_idx, 400u);

  auto post_ins = execute_sql("INSERT INTO orders VALUES (2001, 'post_crash_user', 42.50, TRUE)");
  ASSERT_TRUE(post_ins.success) << post_ins.error_message;
  auto post_sel = execute_sql("SELECT id FROM orders WHERE id = 2001");
  ASSERT_TRUE(post_sel.success) << post_sel.error_message;
  EXPECT_EQ(post_sel.rows.size(), 1u);
}

TEST_F(FaultInjectionRecoveryE2ETest, DatabaseEngineStartupRecoversAfterCrashAndCreatesCheckpointMarker) {
  auto create_res = execute_sql(
      "CREATE TABLE orders (id INT, customer VARCHAR(32), amount FLOAT, active BOOL)");
  ASSERT_TRUE(create_res.success) << create_res.error_message;

  for (int batch = 0; batch < 6; ++batch) {
    int start_id = batch * 50 + 1;
    auto ins_res = execute_sql(build_orders_batch_insert(start_id, 50));
    ASSERT_TRUE(ins_res.success) << ins_res.error_message;
    EXPECT_EQ(ins_res.affected_rows, 50u);
  }

  for (int id = 10; id <= 300; id += 10) {
    auto del_res = execute_sql("DELETE FROM orders WHERE id = " + std::to_string(id));
    ASSERT_TRUE(del_res.success) << del_res.error_message;
  }

  uint64_t loser_txn = txn_mgr_->begin();
  for (int ghost_id = 5000; ghost_id < 5040; ++ghost_id) {
    Tuple ghost_row({int32_t(ghost_id),
                     std::string("uncommitted_" + std::to_string(ghost_id)),
                     double(123.45),
                     bool(false)});
    ASSERT_TRUE(table_mgr_->insert_row("orders", ghost_row, loser_txn).has_value());
  }
  ASSERT_TRUE(log_mgr_->flush_to_lsn(log_mgr_->get_next_lsn()));

  fault_disk_->simulate_power_loss();
  shutdown_stack();
  fault_disk_.reset();

  DatabaseEngine engine(db_path_);
  EXPECT_TRUE(std::filesystem::exists(db_path_ + ".checkpoint"));

  Lexer lexer("SELECT id, customer FROM orders");
  Parser parser(lexer.tokenize());
  auto ast = parser.parse();
  ExecutionResult select_res = engine.get_executor().execute(ast.get());
  ASSERT_TRUE(select_res.success) << select_res.error_message;
  EXPECT_EQ(select_res.rows.size(), 270u);

  for (const auto& tuple : select_res.rows) {
    int32_t id = std::get<int32_t>(tuple.get_value(0));
    EXPECT_NE(id % 10, 0);
    EXPECT_LT(id, 5000);
  }
}

}
