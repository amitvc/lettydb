#include "storage/checkpoint_manager.h"
#include "storage/log_record.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <thread>
#include <chrono>
#include <memory>

using namespace letty;

class MockBufferPoolManager {
 public:
  std::unordered_map<page_id_t, lsn_t> get_dirty_page_table() const {
    return {{10, 100}, {20, 200}};
  }
};

class MockLogManager {
 public:
  lsn_t get_next_lsn() const { return next_lsn_; }
  void set_next_lsn(lsn_t lsn) { next_lsn_ = lsn; }

 private:
  lsn_t next_lsn_ = 1000;
};

class MockTransactionManager {
 public:
  std::unordered_map<uint64_t, lsn_t> get_active_transaction_table() const {
    return {{1, 150}, {2, 175}};
  }
};

class CheckpointManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = "test_checkpoint_dir_" + std::to_string(test_counter_++);
    std::filesystem::remove_all(test_dir_);
    std::filesystem::create_directories(test_dir_);
    db_path_ = (std::filesystem::path(test_dir_) / "test.db").string();
  }

  void TearDown() override {
    std::filesystem::remove_all(test_dir_);
  }

  std::string test_dir_;
  std::string db_path_;
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

TEST_F(CheckpointManagerTest, CheckpointMarkerRejactsInvalidSignature) {
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

