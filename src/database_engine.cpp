#include "database_engine.h"
#include "storage/disk_manager.h"
#include "buffer/buffer_pool_manager.h"
#include "buffer/lru_replacer.h"
#include "storage/log_manager.h"
#include "transaction/transaction_manager.h"
#include "storage/checkpoint_manager.h"
#include "storage/recovery_manager.h"
#include "storage/rollback_manager.h"
#include "storage/page_allocator.h"
#include "catalog/catalog_manager.h"
#include "storage/table_manager.h"
#include "sql/executor.h"
#include "monitoring/storage_inspector.h"

namespace letty {

DatabaseEngine::DatabaseEngine(const std::string& db_path) {
  disk_manager_ = std::make_unique<DiskManager>(db_path);
  buffer_pool_ = std::make_unique<BufferPoolManager>(
      *disk_manager_, DEFAULT_POOL_SIZE, std::make_unique<LRUPageReplacer>());
  log_manager_ = std::make_unique<LogManager>(db_path + ".wal");
  rollback_manager_ = std::make_unique<RollbackManager>(*log_manager_, *buffer_pool_, db_path);
  txn_manager_ = std::make_unique<TransactionManager>(*log_manager_, *rollback_manager_);
  checkpoint_manager_ = std::make_unique<CheckpointManager>(db_path, *buffer_pool_, *log_manager_, *txn_manager_);

  // Crash recovery runs unconditionally on startup, before any component reads
  // pages as logical data (notably catalog_manager_->init() below). Replays the
  // WAL through Analysis -> Redo -> Undo. On a fresh or cleanly-shut-down
  // database there is nothing to replay and this is a no-op.
  RecoveryManager recovery_manager(db_path, *log_manager_, *buffer_pool_, *rollback_manager_);
  recovery_manager.analyze();

  page_allocator_ = std::make_unique<PageAllocator>(*buffer_pool_, *log_manager_, *txn_manager_);
  catalog_manager_ = std::make_unique<CatalogManager>(*buffer_pool_, *page_allocator_, *txn_manager_);
  catalog_manager_->init();

  buffer_pool_->flush_all_pages();
  checkpoint_manager_->run_checkpoint();
  table_manager_ = std::make_unique<TableManager>(*buffer_pool_, *catalog_manager_, *log_manager_, *txn_manager_,* page_allocator_);
  executor_ = std::make_unique<Executor>(*catalog_manager_, *table_manager_, *txn_manager_, *checkpoint_manager_);
  inspector_ = std::make_unique<StorageInspector>(
      *buffer_pool_, *catalog_manager_);
}

DatabaseEngine::~DatabaseEngine() {
  if (checkpoint_manager_) {
    checkpoint_manager_->run_checkpoint();
  }
}

Executor& DatabaseEngine::get_executor() { return *executor_; }
StorageInspector& DatabaseEngine::get_inspector() { return *inspector_; }
CatalogManager& DatabaseEngine::get_catalog() { return *catalog_manager_; }
BufferPoolManager& DatabaseEngine::get_buffer_pool() { return *buffer_pool_; }
CheckpointManager& DatabaseEngine::get_checkpoint_manager() { return *checkpoint_manager_; }

}
