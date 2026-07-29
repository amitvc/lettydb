// Test suite for list-based IAM functionality. IAM chains live in dedicated
// extents: the head is extent-aligned and the chain fills the extent's pages
// front to back before a new extent is allocated.
//

#include <gtest/gtest.h>
#include "storage/iam_manager.h"
#include "storage/disk_manager.h"
#include "buffer/buffer_pool_manager.h"
#include "buffer/lru_replacer.h"
#include "storage/extent_manager.h"
#include "common/db_exception.h"
#include "common/logger.h"
#include <filesystem>
#include <memory>
#include <chrono>
#include <vector>

namespace letty {

class IamManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a temporary database file for testing
        test_db_file = std::filesystem::temp_directory_path() / "iam_manager_test.db";
        
        // Initialize components
        disk_manager = std::make_unique<DiskManager>(test_db_file.string());
        bpm = std::make_unique<BufferPoolManager>(
            *disk_manager, 64, std::make_unique<LRUPageReplacer>());
        extent_manager = std::make_unique<ExtentManager>(*bpm);
        ASSERT_TRUE(bpm->flush_all_pages());  // Flush init pages for components still using DiskManager directly
        iam_manager = std::make_unique<IamManager>(*bpm, *extent_manager);

        LOG_STORAGE_INFO("Test setup complete for IAMManager tests");
    }
    
    void TearDown() override {
        // Cleanup (reverse order of construction)
        iam_manager.reset();
        extent_manager.reset();
        bpm.reset();
        disk_manager.reset();
        
        // Remove test file
        if (std::filesystem::exists(test_db_file)) {
            std::filesystem::remove(test_db_file);
        }
        
        LOG_STORAGE_INFO("Test teardown complete");
    }
    
    /**
     * Helper function to create a test table's IAM chain
     */
    page_id_t create_test_table_iam() {
        return iam_manager->create_iam_chain("test_table");
    }
    
    /**
     * Helper to count the number of pages in an IAM chain
     */
    size_t count_iam_chain_length(page_id_t head_page_id) {
        EXPECT_TRUE(bpm->flush_all_pages());  // Ensure BPM writes are visible on disk
        size_t count = 0;
        page_id_t current = head_page_id;

        while (current != INVALID_PAGE_ID) {
            count++;
            char buffer[PAGE_SIZE];
            if (disk_manager->read_page(current, buffer) != IOResult::SUCCESS) {
                break;
            }
            IAMPage page{};
            std::memcpy(&page, buffer, sizeof(IAMPage));
            current = page.next_page_id;
        }

        return count;
    }

    /**
     * Helper to count total extents tracked in an IAM chain
     */
    size_t count_extents_in_iam(page_id_t head_page_id) {
        EXPECT_TRUE(bpm->flush_all_pages());  // Ensure BPM writes are visible on disk
        size_t count = 0;
        page_id_t current = head_page_id;

        while (current != INVALID_PAGE_ID) {
            char buffer[PAGE_SIZE];
            if (disk_manager->read_page(current, buffer) != IOResult::SUCCESS) {
                break;
            }
            IAMPage page{};
            std::memcpy(&page, buffer, sizeof(IAMPage));
            count += page.extent_count;
            current = page.next_page_id;
        }

        return count;
    }
    
    std::filesystem::path test_db_file;
    std::unique_ptr<DiskManager> disk_manager;
    std::unique_ptr<BufferPoolManager> bpm;
    std::unique_ptr<ExtentManager> extent_manager;
    std::unique_ptr<IamManager> iam_manager;
};

/**
 * Test basic IAM chain creation
 */
TEST_F(IamManagerTest, CreateIAMChain) {
    page_id_t table_iam = create_test_table_iam();
    ASSERT_NE(table_iam, INVALID_PAGE_ID);

    // The IAM head is the first page of a dedicated extent
    EXPECT_EQ(table_iam % EXTENT_SIZE, 0);

    // Flush BPM so create_iam_chain writes are visible on disk
    EXPECT_TRUE(bpm->flush_all_pages());

    // Verify the IAM page structure
    char buffer[PAGE_SIZE];
    ASSERT_EQ(disk_manager->read_page(table_iam, buffer), IOResult::SUCCESS);

    IAMPage iam_page{};
    std::memcpy(&iam_page, buffer, sizeof(IAMPage));
    EXPECT_EQ(iam_page.next_page_id, INVALID_PAGE_ID);
    EXPECT_EQ(iam_page.extent_count, 0);

    LOG_STORAGE_INFO("Create IAM chain test passed, IAM page: {}", table_iam);
}

/**
 * Test IAM Page allocation - should create only necessary pages
 */
TEST_F(IamManagerTest, BasicIAMPageAllocation) {
    page_id_t table_iam = create_test_table_iam();
    ASSERT_NE(table_iam, INVALID_PAGE_ID);
    
    // Initially should have 1 IAM page with 0 extents
    EXPECT_EQ(count_iam_chain_length(table_iam), 1);
    EXPECT_EQ(count_extents_in_iam(table_iam), 0);
    
    // Allocate extent
    page_id_t extent1 = iam_manager->allocate_extent_for_table(table_iam);
    ASSERT_NE(extent1, INVALID_PAGE_ID);
    
    // Should still have only 1 IAM page, but now with 1 extent
    EXPECT_EQ(count_iam_chain_length(table_iam), 1);
    EXPECT_EQ(count_extents_in_iam(table_iam), 1);
    
    LOG_STORAGE_INFO("Basic IAM page allocation test passed");
}

/**
 * Test multiple extent allocations in same IAM page
 */
TEST_F(IamManagerTest, MultipleExtentAllocations) {
    page_id_t table_iam = create_test_table_iam();
    ASSERT_NE(table_iam, INVALID_PAGE_ID);
    
    const int NUM_EXTENTS = 10;
    std::vector<page_id_t> extents;
    
    for (int i = 0; i < NUM_EXTENTS; ++i) {
        page_id_t extent = iam_manager->allocate_extent_for_table(table_iam);
        ASSERT_NE(extent, INVALID_PAGE_ID);
        extents.push_back(extent);
    }
    
    // All extents should fit in 1 IAM page (capacity is 1022)
    EXPECT_EQ(count_iam_chain_length(table_iam), 1);
    EXPECT_EQ(count_extents_in_iam(table_iam), NUM_EXTENTS);
    
    // Verify all extents are different
    for (size_t i = 0; i < extents.size(); ++i) {
        for (size_t j = i + 1; j < extents.size(); ++j) {
            EXPECT_NE(extents[i], extents[j]);
        }
    }
    
    LOG_STORAGE_INFO("Multiple extent allocations test passed");
}

/**
 * Test that each table's IAM chain gets its own dedicated extent
 */
TEST_F(IamManagerTest, MultipleTablesGetDedicatedExtents) {
    // Create multiple table IAM chains
    page_id_t table1_iam = create_test_table_iam();
    page_id_t table2_iam = create_test_table_iam();
    page_id_t table3_iam = create_test_table_iam();

    ASSERT_NE(table1_iam, INVALID_PAGE_ID);
    ASSERT_NE(table2_iam, INVALID_PAGE_ID);
    ASSERT_NE(table3_iam, INVALID_PAGE_ID);

    // Each head is extent-aligned
    EXPECT_EQ(table1_iam % EXTENT_SIZE, 0);
    EXPECT_EQ(table2_iam % EXTENT_SIZE, 0);
    EXPECT_EQ(table3_iam % EXTENT_SIZE, 0);

    // Each table's IAM lives in a different extent
    EXPECT_NE(extent_id_from_page(table1_iam), extent_id_from_page(table2_iam));
    EXPECT_NE(extent_id_from_page(table2_iam), extent_id_from_page(table3_iam));
    EXPECT_NE(extent_id_from_page(table1_iam), extent_id_from_page(table3_iam));

    LOG_STORAGE_INFO("Multiple tables get dedicated extents test passed");
}

/**
 * Test IAMPage structure functionality
 */
TEST_F(IamManagerTest, IAMPageStructure) {
    IAMPage iam_page = make_iam_page();
    
    // Test initial state
    EXPECT_EQ(iam_page.extent_count, 0);
    EXPECT_TRUE(iam_page.has_space());
    EXPECT_FALSE(iam_page.contains_extent(100));
    
    // Test adding extents
    EXPECT_TRUE(iam_page.add_extent(100));
    EXPECT_EQ(iam_page.extent_count, 1);
    EXPECT_TRUE(iam_page.contains_extent(100));
    EXPECT_FALSE(iam_page.contains_extent(200));
    
    // Add more extents
    EXPECT_TRUE(iam_page.add_extent(200));
    EXPECT_TRUE(iam_page.add_extent(300));
    EXPECT_EQ(iam_page.extent_count, 3);
    EXPECT_TRUE(iam_page.contains_extent(200));
    EXPECT_TRUE(iam_page.contains_extent(300));
    
    LOG_STORAGE_INFO("IAMPage structure test passed");
}

/**
 * Test that a full IAM page grows the chain into the next page of the
 * same extent rather than allocating a new extent.
 */
TEST_F(IamManagerTest, ChainGrowthStaysInIamExtent) {
    page_id_t table_iam = create_test_table_iam();
    ASSERT_NE(table_iam, INVALID_PAGE_ID);

    // Fill the head IAM page (IAM_MAX_EXTENTS entries) plus one to force growth
    for (size_t i = 0; i < IAM_MAX_EXTENTS + 1; ++i) {
        page_id_t extent = iam_manager->allocate_extent_for_table(table_iam);
        ASSERT_NE(extent, INVALID_PAGE_ID);
    }

    EXPECT_EQ(count_iam_chain_length(table_iam), 2);
    EXPECT_EQ(count_extents_in_iam(table_iam), IAM_MAX_EXTENTS + 1);

    // The second IAM page is the next page of the head's extent
    EXPECT_TRUE(bpm->flush_all_pages());
    char buffer[PAGE_SIZE];
    ASSERT_EQ(disk_manager->read_page(table_iam, buffer), IOResult::SUCCESS);
    IAMPage head{};
    std::memcpy(&head, buffer, sizeof(IAMPage));
    EXPECT_EQ(head.next_page_id, table_iam + 1);

    LOG_STORAGE_INFO("Chain growth stays in IAM extent test passed");
}

/**
 * Performance test - verify allocations are efficient
 */
TEST_F(IamManagerTest, PerformanceVerification) {
    page_id_t table_iam = create_test_table_iam();
    ASSERT_NE(table_iam, INVALID_PAGE_ID);
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    std::vector<page_id_t> allocated_extents;
    const int NUM_ALLOCATIONS = 50;
    
    for (int i = 0; i < NUM_ALLOCATIONS; ++i) {
        page_id_t extent = iam_manager->allocate_extent_for_table(table_iam);
        if (extent != INVALID_PAGE_ID) {
            allocated_extents.push_back(extent);
        }
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    // Verify we allocated all extents
    EXPECT_EQ(allocated_extents.size(), NUM_ALLOCATIONS);
    
    // Verify chain length is minimal (all fit in one page)
    size_t final_chain_length = count_iam_chain_length(table_iam);
    EXPECT_EQ(final_chain_length, 1);
    
    LOG_STORAGE_INFO("Performance test: {} allocations in {}ms, final chain length: {}", 
                    allocated_extents.size(), duration.count(), final_chain_length);
}

/**
 * Test edge cases and error conditions
 */
TEST_F(IamManagerTest, EdgeCases) {
    // Test with invalid IAM head
    EXPECT_THROW({
        iam_manager->allocate_extent_for_table(INVALID_PAGE_ID);
    }, DbException);
    
    // Test find_page_with_space with invalid IAM
    page_id_t no_page = iam_manager->find_page_with_space(INVALID_PAGE_ID, 100);
    EXPECT_EQ(no_page, INVALID_PAGE_ID);
    
    LOG_STORAGE_INFO("Edge cases test passed");
}

}
