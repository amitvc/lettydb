#include "storage/storage_def.h"

#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#include "storage/config.h"

namespace letty {
class BitmapTest : public ::testing::Test {
 protected:
  void SetUp() override {
	std::memset(bitmap_data, 0, sizeof(bitmap_data));
  }

  char bitmap_data[64] = {0}; // 512 bits
};

TEST_F(BitmapTest, InitialBitsAreNotSet) {
  Bitmap bitmap(bitmap_data, 512);

  for (uint32_t i = 0; i < 512; ++i) {
	EXPECT_FALSE(bitmap.is_set(i)) << "Bit " << i << " should not be set initially";
  }
}

TEST_F(BitmapTest, SetAndCheckSingleBit) {
  Bitmap bitmap(bitmap_data, 512);

  bitmap.set(42);
  EXPECT_TRUE(bitmap.is_set(42));

  // Verify other bits are still unset
  EXPECT_FALSE(bitmap.is_set(41));
  EXPECT_FALSE(bitmap.is_set(43));
}

TEST_F(BitmapTest, SetMultipleBits) {
  Bitmap bitmap(bitmap_data, 512);

  std::vector<uint32_t> bits_to_set = {0, 1, 7, 8, 15, 16, 31, 32, 63, 64, 127, 255, 511};

  for (uint32_t bit : bits_to_set) {
	bitmap.set(bit);
  }

  for (uint32_t bit : bits_to_set) {
	EXPECT_TRUE(bitmap.is_set(bit)) << "Bit " << bit << " should be set";
  }
}

TEST_F(BitmapTest, BoundaryConditions) {
  Bitmap bitmap(bitmap_data, 512);

  // Test first bit
  bitmap.set(0);
  EXPECT_TRUE(bitmap.is_set(0));

  // Test last valid bit
  bitmap.set(511);
  EXPECT_TRUE(bitmap.is_set(511));

  // Test out of bounds access (should be safe)
  bitmap.set(512);
  EXPECT_FALSE(bitmap.is_set(512));

  bitmap.set(1000);
  EXPECT_FALSE(bitmap.is_set(1000));
}

TEST_F(BitmapTest, ByteBoundaryTests) {
  Bitmap bitmap(bitmap_data, 512);

  // Test bits across byte boundaries
  for (uint32_t byte = 0; byte < 8; ++byte) {
	uint32_t bit_start = byte * 8;

	// Set all bits in this byte
	for (uint32_t bit = 0; bit < 8; ++bit) {
	  bitmap.set(bit_start + bit);
	}

	// Verify all bits in this byte are set
	for (uint32_t bit = 0; bit < 8; ++bit) {
	  EXPECT_TRUE(bitmap.is_set(bit_start + bit))
				<< "Bit " << (bit_start + bit) << " in byte " << byte << " should be set";
	}
  }
}

TEST_F(BitmapTest, ClearAllBits) {
  Bitmap bitmap(bitmap_data, 512);

  // Set every 10th bit
  for (uint32_t i = 0; i < 512; i += 10) {
	bitmap.set(i);
  }

  // Verify they are set
  for (uint32_t i = 0; i < 512; i += 10) {
	EXPECT_TRUE(bitmap.is_set(i));
  }

  // Clear them all
  for (uint32_t i = 0; i < 512; i += 10) {
	bitmap.clear(i);
  }

  // Verify they are cleared
  for (uint32_t i = 0; i < 512; i += 10) {
	EXPECT_FALSE(bitmap.is_set(i));
  }
}
}


