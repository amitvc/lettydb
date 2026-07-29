#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include <set>
#include "storage/extent_manager.h"
#include "storage/disk_manager.h"
#include "storage/page_utils.h"
#include "storage/storage_def.h"
#include "buffer/buffer_pool_manager.h"
#include "buffer/lru_replacer.h"

namespace letty {

class ExtentManagerTest : public ::testing::Test {
 protected:
  static constexpr size_t POOL_SIZE = 16;

  void SetUp() override {
	test_db_file = "test_extent_manager.db";
	if (std::filesystem::exists(test_db_file)) {
	  std::filesystem::remove(test_db_file);
	}
  }

  void TearDown() override {
	std::filesystem::remove(test_db_file);
  }

  /** Helper: creates the full DiskManager → BufferPoolManager → ExtentManager stack. */
  struct TestStack {
    std::unique_ptr<DiskManager> disk_manager;
    std::unique_ptr<BufferPoolManager> bpm;
    std::unique_ptr<ExtentManager> extent_manager;
  };

  TestStack CreateStack(size_t pool_size = POOL_SIZE) {
    TestStack s;
    s.disk_manager = std::make_unique<DiskManager>(test_db_file);
    s.bpm = std::make_unique<BufferPoolManager>(
        *s.disk_manager, pool_size, std::make_unique<LRUPageReplacer>());
    s.extent_manager = std::make_unique<ExtentManager>(*s.bpm);
    return s;
  }

  std::string test_db_file;
};

// Test if the db initialization works correctly when we start with empty db file.
// This test ensures all essential pages (db header, GAM's and IAM's) are written correctly to the db file.
TEST_F(ExtentManagerTest, TestInitialization) {
  auto [disk_manager, bpm, extent_manager] = CreateStack();

  // Flush BPM so we can verify on-disk state
  EXPECT_TRUE(bpm->flush_all_pages());

  // Check if Header page is initialized correctly
  char header_buffer[PAGE_SIZE];
  disk_manager->read_page(HEADER_PAGE_ID, header_buffer);
  DatabaseHeader header{};
  std::memcpy(&header, header_buffer, sizeof(DatabaseHeader));
  EXPECT_EQ(std::string(header.signature), DB_SIGNATURE);
  // File size is 2 extents (extent 0: header, extent 1: GAM)
  EXPECT_EQ(disk_manager->get_file_size_in_pages(), 2 * EXTENT_SIZE);
  EXPECT_EQ(header.gam_page_id, FIRST_GAM_PAGE_ID);
  // sys_tables and sys_columns IAM pages are dynamically allocated (INVALID initially)
  EXPECT_EQ(header.sys_tables_iam_page, INVALID_PAGE_ID);
  EXPECT_EQ(header.sys_columns_iam_page, INVALID_PAGE_ID);

  // Check if GAM page is initialized
  char gam_buffer[PAGE_SIZE];
  disk_manager->read_page(FIRST_GAM_PAGE_ID, gam_buffer);
  GAMPage gam_page{};
  std::memcpy(&gam_page, gam_buffer, sizeof(GAMPage));

  // Verify extents 0 & 1 are allocated and extent 2 is free for tables
  Bitmap gam_bitmap(gam_page.bitmap, GAM_MAX_BITS);
  EXPECT_TRUE(gam_bitmap.is_set(0));   // Extent 0: Header
  EXPECT_TRUE(gam_bitmap.is_set(1));   // Extent 1: GAM page extent
  EXPECT_FALSE(gam_bitmap.is_set(2));  // Extent 2: free

}

// Verify ExtentManager allocates pages correctly.
TEST_F(ExtentManagerTest, TestAllocation) {
  auto [disk_manager, bpm, extent_manager] = CreateStack();

  // Allocate first available extent (extent 2, since 0=header, 1=GAM extent)
  page_id_t extent1_start = extent_manager->allocate_extent();
  EXPECT_EQ(extent1_start, 2 * EXTENT_SIZE);  // 16

  // Allocate second extent
  page_id_t extent2_start = extent_manager->allocate_extent();
  EXPECT_EQ(extent2_start, 3 * EXTENT_SIZE);  // 24

  // Flush BPM so we can verify GAM on disk
  EXPECT_TRUE(bpm->flush_all_pages());

  // Verify in GAM
  char gam_buffer[PAGE_SIZE];
  disk_manager->read_page(FIRST_GAM_PAGE_ID, gam_buffer);
  GAMPage gam_page{};
  std::memcpy(&gam_page, gam_buffer, sizeof(GAMPage));
  Bitmap gam_bitmap(gam_page.bitmap, GAM_MAX_BITS);
  EXPECT_TRUE(gam_bitmap.is_set(0));  // System
  EXPECT_TRUE(gam_bitmap.is_set(1));  // Gam pages extent
  EXPECT_TRUE(gam_bitmap.is_set(2));  // first user allocation
  EXPECT_TRUE(gam_bitmap.is_set(3));  // second user allocation

}

TEST_F(ExtentManagerTest, TestPersistence) {
  {
	auto [disk_manager, bpm, extent_manager] = CreateStack();

	// First allocation is extent 2 (page 16), since 0=system, 1=GAM extent
	page_id_t p1 = extent_manager->allocate_extent();
	EXPECT_EQ(p1, 16);
    // BPM destructor flushes all dirty pages when block ends
  }

  // Re-open
  {
	auto [disk_manager, bpm, extent_manager] = CreateStack();

	// Next allocation should be extent 3 (page 24)
	page_id_t p2 = extent_manager->allocate_extent();
	EXPECT_EQ(p2, 24);

	// Flush and verify on disk
	EXPECT_TRUE(bpm->flush_all_pages());

    // Check that extents 0, 1, 2, 3 are all allocated
    char gam_buffer[PAGE_SIZE];
    disk_manager->read_page(FIRST_GAM_PAGE_ID, gam_buffer);
    GAMPage gam_page{};
    std::memcpy(&gam_page, gam_buffer, sizeof(GAMPage));
    Bitmap gam_bitmap(gam_page.bitmap, GAM_MAX_BITS);
	EXPECT_TRUE(gam_bitmap.is_set(0));  // System
	EXPECT_TRUE(gam_bitmap.is_set(1));  // GAM pages extent
	EXPECT_TRUE(gam_bitmap.is_set(2));  // First user allocation
	EXPECT_TRUE(gam_bitmap.is_set(3));  // Second user allocation
  }
}

TEST_F(ExtentManagerTest, TestGAMPageExpansion) {
  auto [disk_manager, bpm, extent_manager] = CreateStack();

  // Fill the first GAM page (page 8) through BPM (it may be cached from init)
  Page* gam_page_obj = bpm->fetch_page(FIRST_GAM_PAGE_ID);
  ASSERT_NE(gam_page_obj, nullptr);
  GAMPage gam_page = load_page_layout<GAMPage>(gam_page_obj);
  std::memset(gam_page.bitmap, 0xFF, sizeof(gam_page.bitmap));
  store_page_layout(gam_page_obj, gam_page);
  bpm->unpin_page(FIRST_GAM_PAGE_ID, true);

  // Allocate extent. This should trigger creating a new GAM page.
  page_id_t new_extent_page_id = extent_manager->allocate_extent();

  // Flush so we can verify on disk
  EXPECT_TRUE(bpm->flush_all_pages());

  // File size should still be 2 extents (new GAM page fits inside GAM extent 1)
  EXPECT_EQ(disk_manager->get_file_size_in_pages(), 2 * EXTENT_SIZE);

  // The new GAM page should be at page 9 (next page in GAM extent after page 8)
  char gam_buffer[PAGE_SIZE];
  disk_manager->read_page(FIRST_GAM_PAGE_ID, gam_buffer);
  GAMPage first_gam{};
  std::memcpy(&first_gam, gam_buffer, sizeof(GAMPage));
  EXPECT_EQ(first_gam.next_page_id, 9); // Should point to page 9

  // Validate the new GAM page itself
  char new_gam_buffer[PAGE_SIZE];
  disk_manager->read_page(9, new_gam_buffer);
  GAMPage new_gam_page{};
  std::memcpy(&new_gam_page, new_gam_buffer, sizeof(GAMPage));

  // Validate allocation return value
  // The new extent should be at the start of the range covered by the second GAM page
  // Formula: GAM_MAX_BITS (extents per GAM) * EXTENT_SIZE (pages per extent)
  EXPECT_EQ(new_extent_page_id, GAM_MAX_BITS * EXTENT_SIZE);
}

TEST_F(ExtentManagerTest, TestDeallocation) {
  auto [disk_manager, bpm, extent_manager] = CreateStack();

  // Allocate two extents (extents 2 and 3, since 0=header, 1=GAM)
  page_id_t extent1_start = extent_manager->allocate_extent();  // 16
  page_id_t extent2_start = extent_manager->allocate_extent();  // 24
  EXPECT_EQ(extent1_start, 16);
  EXPECT_EQ(extent2_start, 24);

  // Deallocate the first one
  extent_manager->deallocate_extent(extent1_start);

  // Flush and verify in GAM
  EXPECT_TRUE(bpm->flush_all_pages());

  char gam_buffer[PAGE_SIZE];
  disk_manager->read_page(FIRST_GAM_PAGE_ID, gam_buffer);
  GAMPage gam_page{};
  std::memcpy(&gam_page, gam_buffer, sizeof(GAMPage));

  Bitmap gam_bitmap(gam_page.bitmap, GAM_MAX_BITS);

  EXPECT_FALSE(gam_bitmap.is_set(2)); // Extent 2 should be free
  EXPECT_TRUE(gam_bitmap.is_set(3));  // Extent 3 should still be allocated

  // Now, allocate again - should reuse the deallocated one
  page_id_t reused_extent_start = extent_manager->allocate_extent();
  EXPECT_EQ(reused_extent_start, extent1_start);
}

TEST_F(ExtentManagerTest, TestGAMPageExpansionWhenGAMExtentIsFull) {
  auto [disk_manager, bpm, extent_manager] = CreateStack();

  // We need to fill up the GAM extent (extent 1, pages 8-15) with GAM pages.
  // GAM extent layout:
  // 8:  GAM page 0 (already created during init)
  // 9-15: Available for additional GAM pages

  page_id_t gam_pages[] = {8, 9, 10, 11, 12, 13, 14, 15};
  int num_gam_pages = 8;

  // Fill GAM page 8 (already exists, cached from init) and set chain
  Page* gam1_obj = bpm->fetch_page(FIRST_GAM_PAGE_ID);
  ASSERT_NE(gam1_obj, nullptr);
  GAMPage gam1 = load_page_layout<GAMPage>(gam1_obj);
  gam1.next_page_id = 9;
  std::memset(gam1.bitmap, 0xFF, sizeof(gam1.bitmap));
  store_page_layout(gam1_obj, gam1);
  bpm->unpin_page(FIRST_GAM_PAGE_ID, true);

  // Create GAM pages 9-15 through BPM
  // Note: Page 15 already exists in BPM from initialize_new_db()
  // (GAM extent reservation page), so we fetch it instead of new_page.
  for (int i = 1; i < num_gam_pages; ++i) {
    page_id_t current = gam_pages[i];
    page_id_t next = (i == num_gam_pages - 1) ? INVALID_PAGE_ID : gam_pages[i + 1];

    Page* page_obj;
    if (current == 2 * EXTENT_SIZE - 1) {
      page_obj = bpm->fetch_page(current);  // Page 15 already in BPM
    } else {
      page_obj = bpm->new_page(current);
    }
    ASSERT_NE(page_obj, nullptr) << "Failed to create GAM page " << current;
    GAMPage page = make_gam_page();
    page.next_page_id = next;
    std::memset(page.bitmap, 0xFF, sizeof(page.bitmap));
    store_page_layout(page_obj, page);
    bpm->unpin_page(current, true);
  }

  // Verify file size before (2 extents: header + GAM)
  EXPECT_TRUE(bpm->flush_all_pages());
  EXPECT_EQ(disk_manager->get_file_size_in_pages(), 2 * EXTENT_SIZE);

  // Allocate!
  // Should traverse 8->9->10->11->12->13->14->15->Full -> Create new GAM at EOF.
  page_id_t new_page_id = extent_manager->allocate_extent();

  // Flush to verify on disk
  EXPECT_TRUE(bpm->flush_all_pages());

  // Verify file size after - should have grown to include the new GAM extent
  EXPECT_GE(disk_manager->get_file_size_in_pages(), 2 * EXTENT_SIZE + 1);

  // Verify new GAM at EOF (new extent since GAM extent is full)
  // The new GAM was placed at what was the end of file (page 16)
  page_id_t expected_new_gam_page = 2 * EXTENT_SIZE; // page 16
  char new_gam_buffer[PAGE_SIZE];
  IOResult res = disk_manager->read_page(expected_new_gam_page, new_gam_buffer);
  EXPECT_EQ(res, IOResult::SUCCESS);
  GAMPage new_gam{};
  std::memcpy(&new_gam, new_gam_buffer, sizeof(GAMPage));
  EXPECT_EQ(new_gam.next_page_id, INVALID_PAGE_ID);

  // Verify link from page 15 (last page in GAM extent) to new GAM
  char page15_buffer[PAGE_SIZE];
  disk_manager->read_page(15, page15_buffer);
  GAMPage page15{};
  std::memcpy(&page15, page15_buffer, sizeof(GAMPage));
  EXPECT_EQ(page15.next_page_id, expected_new_gam_page);

  // Return ID should be valid and large
  EXPECT_GT(new_page_id, 0);
}

TEST_F(ExtentManagerTest, TestCorruptDatabaseSignature) {
  // Create a file with valid database
  {
	auto [disk_manager, bpm, extent_manager] = CreateStack();
    // BPM destructor flushes all dirty pages
  }

  // Corrupt the signature
  {
	std::fstream file(test_db_file, std::ios::in | std::ios::out | std::ios::binary);
	file.seekp(0);
	file.write("INVALID ", 8); // Write invalid db header signature.
	file.close();
  }

  // Opening with bad signature should throw
  {
	auto disk_manager = std::make_unique<DiskManager>(test_db_file);
	auto bpm = std::make_unique<BufferPoolManager>(
	    *disk_manager, POOL_SIZE, std::make_unique<LRUPageReplacer>());
	EXPECT_THROW({
				   ExtentManager em(*bpm);
				 }, std::runtime_error);
  }
}

TEST_F(ExtentManagerTest, TestConcurrentAllocationDeallocation) {
  auto [disk_manager, bpm, extent_manager] = CreateStack();

  const int num_threads = 4;
  const int operations_per_thread = 10;
  std::vector<std::thread> threads;
  std::vector<std::vector<page_id_t>> allocated_extents(num_threads);

  // Launch allocation threads
  auto& em = extent_manager;
  for (int i = 0; i < num_threads; ++i) {
	threads.emplace_back([&em, &allocated_extents, i, operations_per_thread]() {
	  for (int j = 0; j < operations_per_thread; ++j) {
		page_id_t extent_start = em->allocate_extent();
		EXPECT_NE(extent_start, INVALID_PAGE_ID);
		allocated_extents[i].push_back(extent_start);
	  }
	});
  }

  // Wait for allocation threads to complete
  for (auto &thread : threads) {
	thread.join();
  }
  threads.clear();

  // Verify all allocated extents are unique
  std::set < page_id_t > all_extents;
  for (int i = 0; i < num_threads; ++i) {
	for (page_id_t extent : allocated_extents[i]) {
	  EXPECT_TRUE(all_extents.insert(extent).second) << "Duplicate extent allocated: " << extent;
	}
  }

  // Launch deallocation threads
  for (int i = 0; i < num_threads; ++i) {
	threads.emplace_back([&em, &allocated_extents, i]() {
	  for (page_id_t extent : allocated_extents[i]) {
		em->deallocate_extent(extent);
	  }
	});
  }

  // Wait for deallocation threads to complete
  for (auto &thread : threads) {
	thread.join();
  }

  // Verify all extents can be reallocated (showing they were properly deallocated)
  std::set < page_id_t > reallocated_extents;
  for (int i = 0; i < num_threads * operations_per_thread; ++i) {
	page_id_t extent_start = extent_manager->allocate_extent();
	EXPECT_NE(extent_start, INVALID_PAGE_ID);
	reallocated_extents.insert(extent_start);
  }

  // Should be able to reallocate the same number of extents
  EXPECT_EQ(reallocated_extents.size(), num_threads * operations_per_thread);
}

TEST_F(ExtentManagerTest, TestInvalidExtentDeallocation) {
  auto [disk_manager, bpm, extent_manager] = CreateStack();

  // Test deallocating invalid page IDs
  EXPECT_NO_THROW(extent_manager->deallocate_extent(INVALID_PAGE_ID));
  EXPECT_NO_THROW(extent_manager->deallocate_extent(999999)); // Very large invalid ID

  // Test deallocating non-extent-aligned page IDs
  EXPECT_NO_THROW(extent_manager->deallocate_extent(1)); // Not start of extent
  EXPECT_NO_THROW(extent_manager->deallocate_extent(9)); // Not start of extent
}

TEST_F(ExtentManagerTest, TestDoubleFreeExtent) {
  auto [disk_manager, bpm, extent_manager] = CreateStack();

  // Allocate an extent
  page_id_t extent_start = extent_manager->allocate_extent();
  EXPECT_NE(extent_start, INVALID_PAGE_ID);

  // Deallocate it once
  EXPECT_NO_THROW(extent_manager->deallocate_extent(extent_start));

  // Deallocate it again (double-free) - should not crash
  EXPECT_NO_THROW(extent_manager->deallocate_extent(extent_start));

  // Verify we can still allocate and it reuses the same extent
  page_id_t reused_extent = extent_manager->allocate_extent();
  EXPECT_EQ(reused_extent, extent_start);
}

TEST_F(ExtentManagerTest, TestDeallocateSystemExtent) {
  auto [disk_manager, bpm, extent_manager] = CreateStack();

  // First allocate a normal extent to ensure system is working
  page_id_t normal_extent = extent_manager->allocate_extent();
  EXPECT_EQ(normal_extent, 2 * EXTENT_SIZE); // Should be extent 2 (page 16)

  // Try to deallocate system extent (extent 0)
  // This should not crash but behavior might be undefined
  EXPECT_NO_THROW(extent_manager->deallocate_extent(0));
}

TEST_F(ExtentManagerTest, TestGAMCacheEfficiency) {
  auto [disk_manager, bpm, extent_manager] = CreateStack();

  // Allocate multiple extents to test cache behavior
  std::vector<page_id_t> allocated_extents;

  // First allocation should cache the GAM page
  page_id_t extent1 = extent_manager->allocate_extent();
  EXPECT_EQ(extent1, 2 * EXTENT_SIZE);  // First user extent is extent 2 (page 16)
  allocated_extents.push_back(extent1);

  // Subsequent allocations in the same GAM should hit cache
  for (int i = 0; i < 5; ++i) {
	page_id_t extent = extent_manager->allocate_extent();
	EXPECT_NE(extent, INVALID_PAGE_ID);
	allocated_extents.push_back(extent);
  }

  // Mix allocation and deallocation to test cache coherency
  extent_manager->deallocate_extent(allocated_extents[1]);

  // Allocate again - should reuse the deallocated extent (cache hit)
  page_id_t reused = extent_manager->allocate_extent();
  EXPECT_EQ(reused, allocated_extents[1]);

  // Deallocate an extent and then allocate from a different GAM
  // (when we force GAM expansion) to test cache miss scenarios
  extent_manager->deallocate_extent(allocated_extents[2]);
}

TEST_F(ExtentManagerTest, TestAllocationLimits) {
  auto [disk_manager, bpm, extent_manager] = CreateStack();

  // Allocate many extents to test system behavior under pressure
  std::vector<page_id_t> allocated_extents;

  // Allocate a reasonable number of extents (not enough to fill disk)
  // This tests that the system can handle many allocations
  for (int i = 0; i < 100; ++i) {
	page_id_t extent = extent_manager->allocate_extent();
	if (extent == INVALID_PAGE_ID) {
	  // If allocation fails, that's acceptable for high counts
	  break;
	}
	allocated_extents.push_back(extent);
  }

  // Should have allocated at least some extents
  EXPECT_GT(allocated_extents.size(), 0);

  // All allocated extents should be unique and properly aligned
  std::set < page_id_t > unique_extents(allocated_extents.begin(), allocated_extents.end());
  EXPECT_EQ(unique_extents.size(), allocated_extents.size());

  for (page_id_t extent : allocated_extents) {
	EXPECT_EQ(extent % EXTENT_SIZE, 0); // Should be extent-aligned
	EXPECT_GE(extent, 2 * EXTENT_SIZE); // Should not be a reserved extent (0=header, 1=GAM)
  }

  // Cleanup - deallocate all extents
  for (page_id_t extent : allocated_extents) {
	EXPECT_NO_THROW(extent_manager->deallocate_extent(extent));
  }
}

// For now we will keep this. Not sure if this belongs in unit test.
// I wanted to benchmark the allocation api for extent manager
TEST_F(ExtentManagerTest, TestConcurrentAllocationStress) {
  auto [disk_manager, bpm, extent_manager] = CreateStack();

  const int num_threads = 10;
  const int allocations_per_thread = 500; // Total 5000 allocations
  std::vector<std::thread> threads;
  std::vector<std::vector<page_id_t>> thread_results(num_threads);

  auto start = std::chrono::high_resolution_clock::now();

  // High contention: All threads trying to allocate simultaneously
  auto& em = extent_manager;
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back([&em, &thread_results, i, allocations_per_thread]() {
      for (int j = 0; j < allocations_per_thread; ++j) {
        page_id_t page_id = em->allocate_extent();
        if (page_id != INVALID_PAGE_ID) {
            thread_results[i].push_back(page_id);
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> diff = end - start;

  // Verification
  std::set<page_id_t> all_extents;
  size_t total_allocations = 0;
  for (const auto& results : thread_results) {
      total_allocations += results.size();
      for (page_id_t pid : results) {
          EXPECT_TRUE(all_extents.insert(pid).second) << "Duplicate Page ID found: " << pid;
          EXPECT_EQ(pid % EXTENT_SIZE, 0) << "Page ID not aligned: " << pid;
      }
  }

  double throughput = total_allocations / diff.count();
  std::cout << "==================================================" << std::endl;
  std::cout << "Concurrent Stress Test Results:" << std::endl;
  std::cout << "Threads: " << num_threads << ", Allocations/Thread: " << allocations_per_thread << std::endl;
  std::cout << "Total Allocations: " << total_allocations << " in " << diff.count() << " seconds." << std::endl;
  std::cout << "Throughput: " << throughput << " allocations/sec" << std::endl;
  std::cout << "==================================================" << std::endl;

  EXPECT_EQ(total_allocations, num_threads * allocations_per_thread);
}

TEST_F(ExtentManagerTest, ExtentManagerBenchmark) {
  auto [disk_manager, bpm, extent_manager] = CreateStack();

  const int num_allocations = 50000;
  std::vector<page_id_t> allocated;
  allocated.reserve(num_allocations);

  auto start = std::chrono::high_resolution_clock::now();

  for (int i = 0; i < num_allocations; ++i) {
      page_id_t pid = extent_manager->allocate_extent();
      if (pid == INVALID_PAGE_ID) break;
      allocated.push_back(pid);
  }

  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> diff = end - start;

  double throughput = allocated.size() / diff.count();

  std::cout << "==================================================" << std::endl;
  std::cout << "ExtentManager Benchmark Results:" << std::endl;
  std::cout << "Allocated " << allocated.size() << " extents in " << diff.count() << " seconds." << std::endl;
  std::cout << "Throughput: " << throughput << " allocations/sec" << std::endl;
  std::cout << "==================================================" << std::endl;

  EXPECT_GT(throughput, 1000.0) << "Throughput should be reasonably high";
}

// Note: The old mock-based tests (TestDiskReadFailureDuringAllocation,
// TestWriteFailureDuringGAMExpansion) were removed because ExtentManager no
// longer calls DiskManager directly. Disk I/O errors are now handled at the
// BufferPoolManager layer. To test ExtentManager's response to BPM failures
// (e.g., pool full), we would need a BufferPoolManager interface/mock.

}
