// DatabaseEngine - owns the full storage/catalog/executor stack.
//

#pragma once

#include <memory>
#include <string>

namespace letty {

// Forward declarations
class DiskManager;
class BufferPoolManager;
class ExtentManager;
class IamManager;
class CatalogManager;
class TableManager;
class Executor;
class StorageInspector;

/**
 * @class DatabaseEngine
 * @brief Owns all database components and manages their lifecycle.
 *
 * Constructs the full component chain in the correct dependency order:
 *   DiskManager -> BufferPoolManager -> ExtentManager -> IamManager
 *     -> CatalogManager -> TableManager -> Executor -> StorageInspector
 *
 * Higher-level consumers (CLI, tests) take a DatabaseEngine
 * and interact through the accessors below.
 */
class DatabaseEngine {
 public:
  explicit DatabaseEngine(const std::string& db_path);
  ~DatabaseEngine();

  // Non-copyable, non-movable
  DatabaseEngine(const DatabaseEngine&) = delete;
  DatabaseEngine& operator=(const DatabaseEngine&) = delete;

  Executor& get_executor();
  StorageInspector& get_inspector();
  CatalogManager& get_catalog();
  BufferPoolManager& get_buffer_pool();

 private:
  std::unique_ptr<DiskManager> disk_manager_;
  std::unique_ptr<BufferPoolManager> buffer_pool_;
  std::unique_ptr<ExtentManager> extent_manager_;
  std::unique_ptr<IamManager> iam_manager_;
  std::unique_ptr<CatalogManager> catalog_manager_;
  std::unique_ptr<TableManager> table_manager_;
  std::unique_ptr<Executor> executor_;
  std::unique_ptr<StorageInspector> inspector_;
};

} // namespace letty
