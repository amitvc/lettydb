#include <gtest/gtest.h>
#include "buffer/lru_replacer.h"

using namespace letty;

class LRUPageReplacerTest : public ::testing::Test {
 protected:
  LRUPageReplacer replacer;
};

TEST_F(LRUPageReplacerTest, EmptyReplacerReturnsNoVictim) {
  EXPECT_FALSE(replacer.identify_frame_to_evict().has_value());
  EXPECT_EQ(replacer.size(), 0);
}

TEST_F(LRUPageReplacerTest, UnpinThenVictimReturnsOldest) {
  replacer.mark_evictable(0);
  replacer.mark_evictable(1);
  replacer.mark_evictable(2);
  EXPECT_EQ(replacer.size(), 3);

  auto victim = replacer.identify_frame_to_evict();
  ASSERT_TRUE(victim.has_value());
  EXPECT_EQ(*victim, 0);  // oldest

  victim = replacer.identify_frame_to_evict();
  ASSERT_TRUE(victim.has_value());
  EXPECT_EQ(*victim, 1);

  victim = replacer.identify_frame_to_evict();
  ASSERT_TRUE(victim.has_value());
  EXPECT_EQ(*victim, 2);

  EXPECT_FALSE(replacer.identify_frame_to_evict().has_value());  // empty now
}

TEST_F(LRUPageReplacerTest, PinRemovesFromReplacer) {
  replacer.mark_evictable(0);
  replacer.mark_evictable(1);
  replacer.mark_evictable(2);

  // Pin frame 1 — it should be skipped during eviction
  replacer.mark_not_evictable(1);
  EXPECT_EQ(replacer.size(), 2);

  auto victim = replacer.identify_frame_to_evict();
  ASSERT_TRUE(victim.has_value());
  EXPECT_EQ(*victim, 0);

  victim = replacer.identify_frame_to_evict();
  ASSERT_TRUE(victim.has_value());
  EXPECT_EQ(*victim, 2);  // 1 was skipped

  EXPECT_FALSE(replacer.identify_frame_to_evict().has_value());
}

TEST_F(LRUPageReplacerTest, UnpinAfterPinMovesToBack) {
  replacer.mark_evictable(0);
  replacer.mark_evictable(1);
  replacer.mark_evictable(2);

  // Pin and re-unpin frame 0 — it moves to the back (most recent)
  replacer.mark_not_evictable(0);
  replacer.mark_evictable(0);

  auto victim = replacer.identify_frame_to_evict();
  ASSERT_TRUE(victim.has_value());
  EXPECT_EQ(*victim, 1);  // 1 is now the oldest

  victim = replacer.identify_frame_to_evict();
  ASSERT_TRUE(victim.has_value());
  EXPECT_EQ(*victim, 2);

  victim = replacer.identify_frame_to_evict();
  ASSERT_TRUE(victim.has_value());
  EXPECT_EQ(*victim, 0);  // 0 is now the newest
}

TEST_F(LRUPageReplacerTest, DuplicateUnpinIsIdempotent) {
  replacer.mark_evictable(5);
  replacer.mark_evictable(5);  // should not add a second entry
  replacer.mark_evictable(5);

  EXPECT_EQ(replacer.size(), 1);

  auto victim = replacer.identify_frame_to_evict();
  ASSERT_TRUE(victim.has_value());
  EXPECT_EQ(*victim, 5);
  EXPECT_FALSE(replacer.identify_frame_to_evict().has_value());  // only one entry existed
}

TEST_F(LRUPageReplacerTest, PinOnNonTrackedFrameIsNoOp) {
  replacer.mark_evictable(0);

  replacer.mark_not_evictable(99);  // not in replacer — should not crash or alter state
  EXPECT_EQ(replacer.size(), 1);

  auto victim = replacer.identify_frame_to_evict();
  ASSERT_TRUE(victim.has_value());
  EXPECT_EQ(*victim, 0);
}

TEST_F(LRUPageReplacerTest, LargeScaleFIFOOrder) {
  const int N = 1000;

  for (int i = 0; i < N; ++i) {
	replacer.mark_evictable(static_cast<frame_id_t>(i));
  }
  EXPECT_EQ(replacer.size(), N);

  // Victims should come out in FIFO order (0, 1, 2, ... N-1)
  for (int i = 0; i < N; ++i) {
    auto victim = replacer.identify_frame_to_evict();
    ASSERT_TRUE(victim.has_value());
    EXPECT_EQ(*victim, static_cast<frame_id_t>(i));
  }
  EXPECT_EQ(replacer.size(), 0);
}

TEST_F(LRUPageReplacerTest, InterleavedPinUnpin) {
  // Simulate a realistic access pattern:
  // unpin 0,1,2 → pin 1 → unpin 3 → pin 0 → unpin 1
  replacer.mark_evictable(0);
  replacer.mark_evictable(1);
  replacer.mark_evictable(2);
  replacer.mark_not_evictable(1);     // remove 1
  replacer.mark_evictable(3);   // add 3
  replacer.mark_not_evictable(0);     // remove 0
  replacer.mark_evictable(1);   // re-add 1 at back

  // Expected order: 2 (oldest), 3, 1 (newest)
  EXPECT_EQ(replacer.size(), 3);

  auto victim = replacer.identify_frame_to_evict();
  ASSERT_TRUE(victim.has_value());
  EXPECT_EQ(*victim, 2);

  victim = replacer.identify_frame_to_evict();
  ASSERT_TRUE(victim.has_value());
  EXPECT_EQ(*victim, 3);

  victim = replacer.identify_frame_to_evict();
  ASSERT_TRUE(victim.has_value());
  EXPECT_EQ(*victim, 1);
}
