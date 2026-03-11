#include <gtest/gtest.h>
#include "storage/disk_manager.h"
#include "storage/config.h"
#include "storage/error_codes.h"
#include <filesystem>
#include <fstream>
#include <cstring>
#include <memory>
using namespace letty;
class DiskManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_db_file_ = "test_db_" + std::to_string(test_counter_++) + ".db";
        // Clean up any existing test file
        std::filesystem::remove(test_db_file_);
    }

    void TearDown() override {
        // Clean up test file after each test
        std::filesystem::remove(test_db_file_);
    }

    std::string test_db_file_;
    static int test_counter_;
};

int DiskManagerTest::test_counter_ = 0;

// Test constructor creates new file when it doesn't exist
TEST_F(DiskManagerTest, ConstructorCreatesNewFile) {
    ASSERT_FALSE(std::filesystem::exists(test_db_file_));
    
    {
        DiskManager dm(test_db_file_);
        EXPECT_TRUE(std::filesystem::exists(test_db_file_));
    }
}

// Test constructor opens existing file
TEST_F(DiskManagerTest, ConstructorOpensExistingFile) {
    // Create file first
    {
        DiskManager dm(test_db_file_);
    }
    ASSERT_TRUE(std::filesystem::exists(test_db_file_));
    
    // Open existing file
    DiskManager dm(test_db_file_);
    EXPECT_TRUE(std::filesystem::exists(test_db_file_));
}

// Test basic write and read operations
TEST_F(DiskManagerTest, BasicWriteAndRead) {
    DiskManager dm(test_db_file_);
    
    // Prepare test data
    char write_data[PAGE_SIZE];
    char read_data[PAGE_SIZE];
    memset(write_data, 'A', PAGE_SIZE);
    memset(read_data, 0, PAGE_SIZE);
    
    // Write to page 0
    IOResult write_result = dm.write_page(0, write_data);
    EXPECT_EQ(write_result, IOResult::SUCCESS);
    
    // Read from page 0
    IOResult read_result = dm.read_page(0, read_data);
    EXPECT_EQ(read_result, IOResult::SUCCESS);
    
    // Verify data matches
    EXPECT_EQ(memcmp(write_data, read_data, PAGE_SIZE), 0);
}

// Test writing to multiple pages
TEST_F(DiskManagerTest, WriteMultiplePages) {
    DiskManager dm(test_db_file_);
    
    char page0_data[PAGE_SIZE];
    char page1_data[PAGE_SIZE];
    char page2_data[PAGE_SIZE];
    char read_buffer[PAGE_SIZE];
    
    // Fill pages with different patterns
    memset(page0_data, 'A', PAGE_SIZE);
    memset(page1_data, 'B', PAGE_SIZE);
    memset(page2_data, 'C', PAGE_SIZE);
    
    // Write pages
    EXPECT_EQ(dm.write_page(0, page0_data), IOResult::SUCCESS);
    EXPECT_EQ(dm.write_page(1, page1_data), IOResult::SUCCESS);
    EXPECT_EQ(dm.write_page(2, page2_data), IOResult::SUCCESS);
    
    // Read and verify each page
    EXPECT_EQ(dm.read_page(0, read_buffer), IOResult::SUCCESS);
    EXPECT_EQ(memcmp(page0_data, read_buffer, PAGE_SIZE), 0);
    
    EXPECT_EQ(dm.read_page(1, read_buffer), IOResult::SUCCESS);
    EXPECT_EQ(memcmp(page1_data, read_buffer, PAGE_SIZE), 0);
    
    EXPECT_EQ(dm.read_page(2, read_buffer), IOResult::SUCCESS);
    EXPECT_EQ(memcmp(page2_data, read_buffer, PAGE_SIZE), 0);
}

// Test writing to non-sequential pages
TEST_F(DiskManagerTest, WriteNonSequentialPages) {
    DiskManager dm(test_db_file_);
    
    char write_data[PAGE_SIZE];
    char read_data[PAGE_SIZE];
    memset(write_data, 'X', PAGE_SIZE);
    
    // Write to page 10 (skipping pages 0-9)
    EXPECT_EQ(dm.write_page(10, write_data), IOResult::SUCCESS);
    
    // Read back page 10
    EXPECT_EQ(dm.read_page(10, read_data), IOResult::SUCCESS);
    EXPECT_EQ(memcmp(write_data, read_data, PAGE_SIZE), 0);
}

// Test overwriting existing page
TEST_F(DiskManagerTest, OverwritePage) {
    DiskManager dm(test_db_file_);
    
    char first_data[PAGE_SIZE];
    char second_data[PAGE_SIZE];
    char read_data[PAGE_SIZE];
    
    memset(first_data, 'F', PAGE_SIZE);
    memset(second_data, 'S', PAGE_SIZE);
    
    // Write first data
    EXPECT_EQ(dm.write_page(0, first_data), IOResult::SUCCESS);
  	EXPECT_EQ(dm.read_page(0, read_data), IOResult::SUCCESS);
    EXPECT_EQ(memcmp(first_data, read_data, PAGE_SIZE), 0);


  // Overwrite with second data
    EXPECT_EQ(dm.write_page(0, second_data), IOResult::SUCCESS);
    
    // Read and verify second data
    EXPECT_EQ(dm.read_page(0, read_data), IOResult::SUCCESS);
    EXPECT_EQ(memcmp(second_data, read_data, PAGE_SIZE), 0);
    EXPECT_NE(memcmp(first_data, read_data, PAGE_SIZE), 0);
}

// Test persistence across DiskManager instances
TEST_F(DiskManagerTest, DataPersistence) {
    char write_data[PAGE_SIZE];
    char read_data[PAGE_SIZE];
    memset(write_data, 'P', PAGE_SIZE);
    
    // Write data with first instance
    {
        DiskManager dm1(test_db_file_);
        EXPECT_EQ(dm1.write_page(5, write_data), IOResult::SUCCESS);
    }
    
    // Read data with second instance
    {
        DiskManager dm2(test_db_file_);
        EXPECT_EQ(dm2.read_page(5, read_data), IOResult::SUCCESS);
        EXPECT_EQ(memcmp(write_data, read_data, PAGE_SIZE), 0);
    }
}

// Test reading from unwritten page (should work but data is undefined)
TEST_F(DiskManagerTest, ReadUnwrittenPage) {
    DiskManager dm(test_db_file_);
    char read_data[PAGE_SIZE];
    
    // Try to read from page 100 without writing to it first
    // This should succeed but data content is undefined
    IOResult result = dm.read_page(100, read_data);
    // Note: This might return SUCCESS or SEEK_ERROR depending on file size
    EXPECT_TRUE(result == IOResult::READ_ERROR);
}

// Test with different page IDs
TEST_F(DiskManagerTest, DifferentPageIds) {
    DiskManager dm(test_db_file_);
    
    const std::vector<page_id_t> page_ids = {0, 1, 5, 10, 100, 999};
    
    for (page_id_t page_id : page_ids) {
        char write_data[PAGE_SIZE];
        char read_data[PAGE_SIZE];
        
        // Fill with pattern based on page_id
        memset(write_data, static_cast<char>('A' + (page_id % 26)), PAGE_SIZE);
        
        EXPECT_EQ(dm.write_page(page_id, write_data), IOResult::SUCCESS);
        EXPECT_EQ(dm.read_page(page_id, read_data), IOResult::SUCCESS);
        EXPECT_EQ(memcmp(write_data, read_data, PAGE_SIZE), 0);
    }
}

// Test with binary data (not just text patterns)
TEST_F(DiskManagerTest, BinaryData) {
    DiskManager dm(test_db_file_);
    
    char write_data[PAGE_SIZE];
    char read_data[PAGE_SIZE];
    
    // Fill with binary pattern including null bytes
    for (int i = 0; i < PAGE_SIZE; ++i) {
        write_data[i] = static_cast<char>(i % 256);
    }
    
    EXPECT_EQ(dm.write_page(0, write_data), IOResult::SUCCESS);
    EXPECT_EQ(dm.read_page(0, read_data), IOResult::SUCCESS);
    EXPECT_EQ(memcmp(write_data, read_data, PAGE_SIZE), 0);
}

// Test file operations are properly flushed
TEST_F(DiskManagerTest, DataFlushing) {
    char write_data[PAGE_SIZE];
    memset(write_data, 'F', PAGE_SIZE);
    
    {
        DiskManager dm(test_db_file_);
        EXPECT_EQ(dm.write_page(0, write_data), IOResult::SUCCESS);

    }
    
    // File should contain the data even after DiskManager destruction
    std::ifstream file(test_db_file_, std::ios::binary);
    ASSERT_TRUE(file.is_open());
    
    char file_data[PAGE_SIZE];
    file.read(file_data, PAGE_SIZE);
    EXPECT_EQ(file.gcount(), PAGE_SIZE);
    EXPECT_EQ(memcmp(write_data, file_data, PAGE_SIZE), 0);
}