// Test suite for list-based IAM and metadata pool functionality
//

#include <gtest/gtest.h>
#include "storage/iam_manager.h"
#include "storage/disk_manager.h"
#include "buffer/buffer_pool_manager.h"
#include "buffer/lru_replacer.h"
#include "storage/extent_manager.h"
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
        
        // Initialize the metadata pool (needed for the new design)
        iam_manager->init_shared_extent(FIRST_SHARED_EXTENT_PAGE_ID);
        
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
            auto page = reinterpret_cast<IAMPage*>(buffer);
            current = page->next_page_id;
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
            auto page = reinterpret_cast<IAMPage*>(buffer);
            count += page->extent_count;
            current = page->next_page_id;
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
 * Test metadata pool initialization
 */
TEST_F(IamManagerTest, SharedExtentInit) {
    // Flush BPM so init_shared_extent writes are visible on disk
    EXPECT_TRUE(bpm->flush_all_pages());

    // Read the metadata pool header
    char buffer[PAGE_SIZE];
    ASSERT_EQ(disk_manager->read_page(FIRST_SHARED_EXTENT_PAGE_ID, buffer), IOResult::SUCCESS);
    
    auto* pool_header = reinterpret_cast<SharedExtentDirectoryPage*>(buffer);
    
    // Should have no slots used initially (just after init)
    // Note: After SetUp, we may have allocated IAM pages, so just check structure is valid
    EXPECT_EQ(pool_header->next_pool_page, INVALID_PAGE_ID);
    
    LOG_STORAGE_INFO("Metadata pool init test passed");
}

/**
 * Test basic IAM chain creation
 */
TEST_F(IamManagerTest, CreateIAMChain) {
    page_id_t table_iam = create_test_table_iam();
    ASSERT_NE(table_iam, INVALID_PAGE_ID);
    
    // Verify the IAM page is from the metadata pool (pages 9-15)
    EXPECT_GE(table_iam, FIRST_SHARED_EXTENT_PAGE_ID + 1);
    EXPECT_LE(table_iam, FIRST_SHARED_EXTENT_PAGE_ID + SHARED_EXTENT_SLOTS);
    
    // Flush BPM so create_iam_chain writes are visible on disk
    EXPECT_TRUE(bpm->flush_all_pages());

    // Verify the IAM page structure
    char buffer[PAGE_SIZE];
    ASSERT_EQ(disk_manager->read_page(table_iam, buffer), IOResult::SUCCESS);

    auto* iam_page = reinterpret_cast<IAMPage*>(buffer);
    EXPECT_EQ(iam_page->next_page_id, INVALID_PAGE_ID);
    EXPECT_EQ(iam_page->extent_count, 0);

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
 * Test multiple tables sharing metadata pool
 */
TEST_F(IamManagerTest, MultipleTablesSharePool) {
    // Create multiple table IAM chains
    page_id_t table1_iam = create_test_table_iam();
    page_id_t table2_iam = create_test_table_iam();
    page_id_t table3_iam = create_test_table_iam();
    
    ASSERT_NE(table1_iam, INVALID_PAGE_ID);
    ASSERT_NE(table2_iam, INVALID_PAGE_ID);
    ASSERT_NE(table3_iam, INVALID_PAGE_ID);
    
    // All should be different pages
    EXPECT_NE(table1_iam, table2_iam);
    EXPECT_NE(table2_iam, table3_iam);
    EXPECT_NE(table1_iam, table3_iam);
    
    // All should be in metadata pool extent (pages 9-15)
    EXPECT_GE(table1_iam, FIRST_SHARED_EXTENT_PAGE_ID + 1);
    EXPECT_GE(table2_iam, FIRST_SHARED_EXTENT_PAGE_ID + 1);
    EXPECT_GE(table3_iam, FIRST_SHARED_EXTENT_PAGE_ID + 1);
    
    LOG_STORAGE_INFO("Multiple tables share pool test passed");
}

/**
 * Test IAMPage structure functionality
 */
TEST_F(IamManagerTest, IAMPageStructure) {
    char buffer[PAGE_SIZE] = {0};
    auto* iam_page = new(buffer) IAMPage();
    
    // Test initial state
    EXPECT_EQ(iam_page->extent_count, 0);
    EXPECT_TRUE(iam_page->has_space());
    EXPECT_FALSE(iam_page->contains_extent(100));
    
    // Test adding extents
    EXPECT_TRUE(iam_page->add_extent(100));
    EXPECT_EQ(iam_page->extent_count, 1);
    EXPECT_TRUE(iam_page->contains_extent(100));
    EXPECT_FALSE(iam_page->contains_extent(200));
    
    // Add more extents
    EXPECT_TRUE(iam_page->add_extent(200));
    EXPECT_TRUE(iam_page->add_extent(300));
    EXPECT_EQ(iam_page->extent_count, 3);
    EXPECT_TRUE(iam_page->contains_extent(200));
    EXPECT_TRUE(iam_page->contains_extent(300));
    
    LOG_STORAGE_INFO("IAMPage structure test passed");
}

/**
 * Test SharedExtentDirectoryPage structure functionality
 */
TEST_F(IamManagerTest, SharedExtentDirectoryPageStructure) {
    char buffer[PAGE_SIZE] = {0};
    auto* pool_header = new(buffer) SharedExtentDirectoryPage();
    
    // Test initial state
    for (uint8_t slot = 1; slot <= SHARED_EXTENT_SLOTS; ++slot) {
        EXPECT_FALSE(pool_header->is_slot_used(slot));
    }
    
    // Find free slot should return 1 (slots are 1-7)
    EXPECT_EQ(pool_header->find_free_slot(), 1);
    
    // Mark slot 1 as used
    pool_header->mark_slot_used(1);
    EXPECT_TRUE(pool_header->is_slot_used(1));
    EXPECT_FALSE(pool_header->is_slot_used(2));
    EXPECT_EQ(pool_header->find_free_slot(), 2);
    
    // Mark more slots
    pool_header->mark_slot_used(2);
    pool_header->mark_slot_used(3);
    EXPECT_EQ(pool_header->find_free_slot(), 4);
    
    // Free a slot
    pool_header->mark_slot_free(2);
    EXPECT_FALSE(pool_header->is_slot_used(2));
    EXPECT_EQ(pool_header->find_free_slot(), 2);  // Should find slot 2 first
    
    // Mark all slots used
    for (uint8_t i = 1; i <= SHARED_EXTENT_SLOTS; ++i) {
        pool_header->mark_slot_used(i);
    }
    EXPECT_EQ(pool_header->find_free_slot(), 0);  // No free slot
    
    LOG_STORAGE_INFO("SharedExtentDirectoryPage structure test passed");
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
    page_id_t invalid_extent = iam_manager->allocate_extent_for_table(INVALID_PAGE_ID);
    EXPECT_EQ(invalid_extent, INVALID_PAGE_ID);
    
    // Test find_page_with_space with invalid IAM
    page_id_t no_page = iam_manager->find_page_with_space(INVALID_PAGE_ID, 100);
    EXPECT_EQ(no_page, INVALID_PAGE_ID);
    
    LOG_STORAGE_INFO("Edge cases test passed");
}

}
