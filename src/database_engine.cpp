#include "database_engine.h"
#include "storage/disk_manager.h"
#include "buffer/buffer_pool_manager.h"
#include "buffer/lru_replacer.h"
#include "storage/extent_manager.h"
#include "storage/iam_manager.h"
#include "catalog/catalog_manager.h"
#include "storage/table_manager.h"
#include "sql/executor.h"
#include "monitoring/storage_inspector.h"

namespace letty {

DatabaseEngine::DatabaseEngine(const std::string& db_path) {
  disk_manager_ = std::make_unique<DiskManager>(db_path);
  buffer_pool_ = std::make_unique<BufferPoolManager>(
      *disk_manager_, DEFAULT_POOL_SIZE, std::make_unique<LRUPageReplacer>());
  extent_manager_ = std::make_unique<ExtentManager>(*buffer_pool_);
  iam_manager_ = std::make_unique<IamManager>(*buffer_pool_, *extent_manager_);
  catalog_manager_ = std::make_unique<CatalogManager>(*buffer_pool_, *iam_manager_);
  catalog_manager_->init();
  table_manager_ = std::make_unique<TableManager>(*buffer_pool_, *iam_manager_, *catalog_manager_);
  executor_ = std::make_unique<Executor>(*catalog_manager_, *table_manager_);
  inspector_ = std::make_unique<StorageInspector>(
      *buffer_pool_, *extent_manager_, *iam_manager_, *catalog_manager_);
}

DatabaseEngine::~DatabaseEngine() = default;

Executor& DatabaseEngine::get_executor() { return *executor_; }
StorageInspector& DatabaseEngine::get_inspector() { return *inspector_; }
CatalogManager& DatabaseEngine::get_catalog() { return *catalog_manager_; }
BufferPoolManager& DatabaseEngine::get_buffer_pool() { return *buffer_pool_; }

} // namespace letty
