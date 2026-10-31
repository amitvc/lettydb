#include "transaction/transaction_manager.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <string>

#include "storage/log_manager.h"

namespace letty {

class TransactionManagerTest : public ::testing::Test {
 protected:
  static constexpr uint32_t TEST_SEGMENT_SIZE = 1024;

  void SetUp() override {
    test_dir_ = "test_txn_dir_" + std::to_string(test_counter_++);
    std::filesystem::remove_all(test_dir_);
    std::filesystem::create_directories(test_dir_);
    test_prefix_ = (test_dir_ / "txn.wal").string();
    log_mgr_ = std::make_unique<LogManager>(test_prefix_, TEST_SEGMENT_SIZE);
    txn_mgr_ = std::make_unique<TransactionManager>(*log_mgr_);
  }

  void TearDown() override {
    std::filesystem::remove_all(test_dir_);
  }

  std::string wal_path() const {
    return test_prefix_ + ".0";
  }

  std::filesystem::path test_dir_;
  std::string test_prefix_;
  std::unique_ptr<LogManager> log_mgr_;
  std::unique_ptr<TransactionManager> txn_mgr_;
  static int test_counter_;
};

int TransactionManagerTest::test_counter_ = 0;

TEST_F(TransactionManagerTest, BeginReturnsNonZeroTxnId) {
  uint64_t txn_id = txn_mgr_->begin();
  EXPECT_GT(txn_id, 0u);
  EXPECT_EQ(txn_mgr_->get_status(txn_id), TransactionStatus::Active);
}

TEST_F(TransactionManagerTest, BeginIncrementsTxnId) {
  uint64_t txn1 = txn_mgr_->begin();
  uint64_t txn2 = txn_mgr_->begin();
  uint64_t txn3 = txn_mgr_->begin();

  EXPECT_GT(txn1, 0u);
  EXPECT_GT(txn2, txn1);
  EXPECT_GT(txn3, txn2);
}

TEST_F(TransactionManagerTest, CommitMarksTransactionCommitted) {
  uint64_t txn_id = txn_mgr_->begin();
  EXPECT_TRUE(txn_mgr_->commit(txn_id));
  EXPECT_EQ(txn_mgr_->get_status(txn_id), TransactionStatus::Committed);
}

TEST_F(TransactionManagerTest, CommitFlushesWALToDisk) {
  uint64_t txn_id = txn_mgr_->begin();
  EXPECT_TRUE(txn_mgr_->commit(txn_id));

  // After commit, the flushed LSN should be at or past the next LSN.
  EXPECT_GE(txn_mgr_->get_flushed_lsn(), txn_mgr_->get_next_lsn());
}

TEST_F(TransactionManagerTest, CommitUnknownTransactionFails) {
  EXPECT_FALSE(txn_mgr_->commit(9999));
}

TEST_F(TransactionManagerTest, CommitNonActiveTransactionFails) {
  uint64_t txn_id = txn_mgr_->begin();
  EXPECT_TRUE(txn_mgr_->commit(txn_id));
  // Second commit should fail — already committed.
  EXPECT_FALSE(txn_mgr_->commit(txn_id));
}

TEST_F(TransactionManagerTest, AbortMarksTransactionAborted) {
  uint64_t txn_id = txn_mgr_->begin();
  EXPECT_TRUE(txn_mgr_->abort(txn_id));
  EXPECT_EQ(txn_mgr_->get_status(txn_id), TransactionStatus::Aborted);
}

TEST_F(TransactionManagerTest, AbortUnknownTransactionFails) {
  EXPECT_FALSE(txn_mgr_->abort(9999));
}

TEST_F(TransactionManagerTest, AbortNonActiveTransactionFails) {
  uint64_t txn_id = txn_mgr_->begin();
  EXPECT_TRUE(txn_mgr_->abort(txn_id));
  // Second abort should fail — already aborted.
  EXPECT_FALSE(txn_mgr_->abort(txn_id));
}

TEST_F(TransactionManagerTest, MultipleTransactionsIndependent) {
  uint64_t txn1 = txn_mgr_->begin();
  uint64_t txn2 = txn_mgr_->begin();

  EXPECT_TRUE(txn_mgr_->commit(txn1));
  EXPECT_EQ(txn_mgr_->get_status(txn1), TransactionStatus::Committed);
  EXPECT_EQ(txn_mgr_->get_status(txn2), TransactionStatus::Active);

  EXPECT_TRUE(txn_mgr_->abort(txn2));
  EXPECT_EQ(txn_mgr_->get_status(txn2), TransactionStatus::Aborted);
}

}  // namespace letty
