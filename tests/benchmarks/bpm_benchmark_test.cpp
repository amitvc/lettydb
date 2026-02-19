//
// BPM Benchmark Harness
// Measures the effectiveness of the Buffer Pool Manager by running
// the same workload at different pool sizes and comparing I/O stats.
//

#include <gtest/gtest.h>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "sql/executor.h"
#include "sql/parser.h"
#include "sql/lexer.h"
#include "storage/disk_manager.h"
#include "buffer/buffer_pool_manager.h"
#include "buffer/lru_replacer.h"
#include "storage/extent_manager.h"
#include "storage/iam_manager.h"
#include "catalog/catalog_manager.h"
#include "storage/table_manager.h"

namespace letty {

// ─── Result row for the comparison table ───────────────────────────

struct BenchmarkResult {
  size_t pool_size;
  uint64_t disk_reads;
  uint64_t disk_writes;
  uint64_t cache_hits;
  uint64_t cache_misses;
  uint64_t evictions;
  double hit_ratio;
  double elapsed_ms;
};

// ─── Harness ───────────────────────────────────────────────────────

class BPMBenchmark : public ::testing::Test {
 protected:
  static constexpr int NUM_INSERTS = 100000;

  static void PrintResultsTable(const std::vector<BenchmarkResult>& results) {
    std::cout << "\n";
    std::cout << "╔══════════╦═════════╦══════════╦══════════╦══════════╦═══════════╦═══════════╦═══════════╗\n";
    std::cout << "║ Pool     ║ Disk    ║ Disk     ║ Cache    ║ Cache    ║           ║ Hit       ║ Time      ║\n";
    std::cout << "║ Size     ║ Reads   ║ Writes   ║ Hits     ║ Misses   ║ Evictions ║ Ratio     ║ (ms)      ║\n";
    std::cout << "╠══════════╬═════════╬══════════╬══════════╬══════════╬═══════════╬═══════════╬═══════════╣\n";

    for (const auto& r : results) {
      std::cout << " ║ " << std::setw(8) << std::left << r.pool_size
                << " ║ " << std::setw(7) << r.disk_reads
                << " ║ " << std::setw(8) << r.disk_writes
                << " ║ " << std::setw(8) << r.cache_hits
                << " ║ " << std::setw(8) << r.cache_misses
                << " ║ " << std::setw(9) << r.evictions
                << " ║ " << std::setw(8) << std::fixed << std::setprecision(1) << r.hit_ratio << "%"
                << " ║ " << std::setw(9) << std::fixed << std::setprecision(1) << r.elapsed_ms
                << " ║\n";
    }

    std::cout << "╚══════════╩═════════╩══════════╩══════════╩══════════╩═══════════╩═══════════╩═══════════╝\n";
    std::cout << "\n";
  }

  static BenchmarkResult RunWorkload(size_t pool_size, int num_inserts,
                                     const std::string& db_file) {
    // Clean slate
    std::remove(db_file.c_str());

    // Build the full stack
    auto disk_manager = std::make_unique<DiskManager>(db_file);
    auto bpm = std::make_unique<BufferPoolManager>(
        *disk_manager, pool_size, std::make_unique<LRUPageReplacer>());
    auto extent_manager = std::make_unique<ExtentManager>(*bpm);
    bpm->flush_all_pages();
    auto iam_manager = std::make_unique<IamManager>(*bpm, *extent_manager);
    auto catalog_manager = std::make_unique<CatalogManager>(*bpm, *iam_manager);
    catalog_manager->init();
    auto table_manager = std::make_unique<TableManager>(*bpm, *iam_manager, *catalog_manager);
    auto executor = std::make_unique<Executor>(*catalog_manager, *table_manager);

    // Helper to execute SQL
    auto execute_sql = [&](const std::string& sql) {
      Lexer lexer(sql);
      auto tokens = lexer.tokenize();
      Parser parser(std::move(tokens));
      auto ast = parser.parse();
      return executor->execute(ast.get());
    };

    // Create table
    execute_sql("CREATE TABLE emp (id INT, name VARCHAR(50), age INT)");

    // Reset stats after setup so we only measure the workload
    disk_manager->reset_io_stats();
    bpm->reset_cache_stats();

    // ── Timed workload ──
    auto start = std::chrono::high_resolution_clock::now();

    // Inserts
    for (int i = 1; i <= num_inserts; ++i) {
      std::string sql = "INSERT INTO emp VALUES (" +
                         std::to_string(i) + ", 'user_" +
                         std::to_string(i) + "', " +
                         std::to_string(20 + (i % 60)) + ")";
      auto result = execute_sql(sql);
      EXPECT_TRUE(result.success) << "Insert failed at row " << i
                                  << ": " << result.error_message;
    }

    // Full table scan
    auto select_result = execute_sql("SELECT * FROM emp");
    EXPECT_TRUE(select_result.success) << select_result.error_message;
    EXPECT_EQ(select_result.rows.size(), num_inserts);

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();

    // Collect stats
    auto io = disk_manager->get_io_stats();
    auto cache = bpm->get_cache_stats();

    // Tear down (BPM destructor flushes)
    executor.reset();
    table_manager.reset();
    catalog_manager.reset();
    iam_manager.reset();
    extent_manager.reset();
    bpm.reset();
    disk_manager.reset();
    std::remove(db_file.c_str());

    return {pool_size, io.reads, io.writes,
            cache.hits, cache.misses, cache.evictions,
            cache.hit_ratio(), elapsed_ms};
  }
};

// ─── The main benchmark test ──────────────────────────────────────

TEST_F(BPMBenchmark, PoolSizeComparison) {
  const std::vector<size_t> pool_sizes = {4, 16, 64, 256};
  std::vector<BenchmarkResult> results;

  std::cout << "\n=== BPM Benchmark: " << NUM_INSERTS
            << " INSERT + SELECT * ===\n";

  for (size_t pool_size : pool_sizes) {
    std::cout << "Running with pool_size=" << pool_size << "..." << std::flush;
    auto result = RunWorkload(pool_size, NUM_INSERTS, "bench_bpm.db");
    results.push_back(result);
    std::cout << " done (" << std::fixed << std::setprecision(1)
              << result.elapsed_ms << " ms)\n";
  }

  PrintResultsTable(results);

  // Verify that larger pools reduce disk reads
  if (results.size() >= 2) {
    EXPECT_LE(results.back().disk_reads, results.front().disk_reads)
        << "Larger pool should have fewer or equal disk reads";
  }

  // Verify that larger pools have better hit ratios
  if (results.size() >= 2) {
    EXPECT_GE(results.back().hit_ratio, results.front().hit_ratio)
        << "Larger pool should have better or equal hit ratio";
  }
}

}  // namespace letty
