// Simple integration test to verify list-based IAM functionality
//

#include <gtest/gtest.h>
#include "storage/iam_manager.h"
#include "storage/disk_manager.h"
#include "buffer/buffer_pool_manager.h"
#include "buffer/lru_replacer.h"
#include "storage/extent_manager.h"
#include "common/logger.h"
#include <filesystem>

namespace letty {

/**
 * Simple integration test for list-based IAM functionality
 */
TEST(IamIntegrationTest, ListBasedIamBasicFunctionality) {
    // Setup temporary database
    auto test_db_file = std::filesystem::temp_directory_path() / "iam_integration_test.db";
    
    try {
        // Initialize components
        DiskManager disk_manager(test_db_file.string());
        BufferPoolManager bpm(disk_manager, 16, std::make_unique<LRUPageReplacer>());
        ExtentManager extent_manager(bpm);
        ASSERT_TRUE(bpm.flush_all_pages());  // Flush init pages for components still using DiskManager directly
        IamManager iam_manager(bpm, extent_manager);

        // Create a table's IAM chain
        page_id_t table_iam = iam_manager.create_iam_chain("test_table");
        ASSERT_NE(table_iam, INVALID_PAGE_ID);
        
        // Allocate some extents
        page_id_t extent1 = iam_manager.allocate_extent_for_table(table_iam);
        EXPECT_NE(extent1, INVALID_PAGE_ID);
        
        page_id_t extent2 = iam_manager.allocate_extent_for_table(table_iam);
        EXPECT_NE(extent2, INVALID_PAGE_ID);
        
        // Both should be valid and different
        EXPECT_NE(extent1, extent2);
        
        LOG_STORAGE_INFO("Basic list-based IAM integration test passed");
        
    } catch (...) {
        if (std::filesystem::exists(test_db_file)) {
            std::filesystem::remove(test_db_file);
        }
        throw;
    }
    
    if (std::filesystem::exists(test_db_file)) {
        std::filesystem::remove(test_db_file);
    }
}

/**
 * Test IAMPage structure basic functionality
 */
TEST(IamIntegrationTest, IAMPageBasics) {
    // Test IAMPage structure
    IAMPage page;
    
    // Test initial state
    EXPECT_EQ(page.extent_count, 0);
    EXPECT_TRUE(page.has_space());
    
    // Add some extents
    EXPECT_TRUE(page.add_extent(100));
    EXPECT_TRUE(page.add_extent(200));
    EXPECT_TRUE(page.add_extent(300));
    EXPECT_EQ(page.extent_count, 3);
    
    // Test contains
    EXPECT_TRUE(page.contains_extent(100));
    EXPECT_TRUE(page.contains_extent(200));
    EXPECT_TRUE(page.contains_extent(300));
    EXPECT_FALSE(page.contains_extent(400));
    
    LOG_STORAGE_INFO("IAMPage basics test passed");
}

/**
 * Test that each table's IAM chain gets its own dedicated extent
 */
TEST(IamIntegrationTest, MultipleTablesGetDedicatedExtents) {
    auto test_db_file = std::filesystem::temp_directory_path() / "multi_table_test.db";

    try {
        DiskManager disk_manager(test_db_file.string());
        BufferPoolManager bpm(disk_manager, 16, std::make_unique<LRUPageReplacer>());
        ExtentManager extent_manager(bpm);
        ASSERT_TRUE(bpm.flush_all_pages());  // Flush init pages for components still using DiskManager directly
        IamManager iam_manager(bpm, extent_manager);

        // Create multiple tables
        page_id_t table1_iam = iam_manager.create_iam_chain("test_table");
        page_id_t table2_iam = iam_manager.create_iam_chain("test_table");
        page_id_t table3_iam = iam_manager.create_iam_chain("test_table");

        EXPECT_NE(table1_iam, INVALID_PAGE_ID);
        EXPECT_NE(table2_iam, INVALID_PAGE_ID);
        EXPECT_NE(table3_iam, INVALID_PAGE_ID);

        // Each head is the first page of its own extent
        EXPECT_EQ(table1_iam % EXTENT_SIZE, 0);
        EXPECT_EQ(table2_iam % EXTENT_SIZE, 0);
        EXPECT_EQ(table3_iam % EXTENT_SIZE, 0);

        EXPECT_NE(extent_id_from_page(table1_iam), extent_id_from_page(table2_iam));
        EXPECT_NE(extent_id_from_page(table2_iam), extent_id_from_page(table3_iam));
        EXPECT_NE(extent_id_from_page(table1_iam), extent_id_from_page(table3_iam));

        LOG_STORAGE_INFO("Multiple tables get dedicated extents test passed");

    } catch (...) {
        if (std::filesystem::exists(test_db_file)) {
            std::filesystem::remove(test_db_file);
        }
        throw;
    }

    if (std::filesystem::exists(test_db_file)) {
        std::filesystem::remove(test_db_file);
    }
}

} // namespace letty
