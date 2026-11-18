#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "storage/config.h"
#include "storage/log_record.h"

namespace letty {

class BufferPoolManager;
class LogManager;
class Page;
class RollbackManager;

template <typename PatchType>
struct patch_traits;

template <>
struct patch_traits<PagePatch> {
  static page_id_t get_page_id(const PagePatch& p) { return p.page_id; }
};

template <>
struct patch_traits<GamPatch> {
  static page_id_t get_page_id(const GamPatch& p) { return p.gam_page_id; }
};

template <>
struct patch_traits<IamPatch> {
  static page_id_t get_page_id(const IamPatch& p) { return p.iam_page_id; }
};

template <>
struct patch_traits<CompensationPatch> {
  static page_id_t get_page_id(const CompensationPatch& p) { return p.page_id; }
};

/**
 * @struct AnalysisResult
 * @brief An in-memory structure that is populated by the RecoveryManager as a result of Analysis phase of database
 * recovery.
 */
struct AnalysisResult {
  std::unordered_map<txn_id, lsn_t> active_transactions;
  std::unordered_map<page_id_t, lsn_t> dirty_page_table;
  lsn_t min_rec_lsn = INVALID_LSN;
  lsn_t scan_start_lsn = INVALID_LSN;
  lsn_t last_log_lsn = INVALID_LSN;
};

/**
 * @class RecoveryManager
 * @brief Responsible for database recovery using ARIES protocol.
 * The recovery begins with Analysis phase followed by REDO and UNDO.
 */
class RecoveryManager {
 public:
  /**
   * @brief Constructs RecoveryManager with database path for checkpoint resolution.
   * @param db_path Database file path (marker file is located at <db_path>.checkpoint).
   * @param log_manager Reference to LogManager for reading WAL records.
   * @param buffer_pool Reference to BufferPoolManager.
   * @param rollback_manager Reference to RollbackManager; the Undo pass is delegated to it.
   */
  RecoveryManager(std::string db_path,
                  LogManager& log_manager,
                  BufferPoolManager& buffer_pool,
                  RollbackManager& rollback_manager);

  /**
   * @brief Executes the ARIES Analysis pass.
   *
   * Locates the last checkpoint via the checkpoint marker (or begins from WAL start),
   * seeds ATT and DPT from the END_CHECKPOINT record, scans forward to EOF,
   * tracks loser transactions, and computes min_rec_lsn for the Redo pass.
   *
   * @return AnalysisResult containing reconstructed ATT, DPT, and min_rec_lsn.
   */
  AnalysisResult analyze();

  /**
   * @brief Executes the ARIES Analysis pass starting from an explicit checkpoint LSN.
   * @param checkpoint_lsn LSN of BEGIN_CHECKPOINT, or INVALID_LSN to scan from WAL start.
   * @return AnalysisResult containing reconstructed ATT, DPT, and min_rec_lsn.
   */
  AnalysisResult analyze_from(lsn_t checkpoint_lsn);

 private:
  std::string db_path_;
  LogManager& log_manager_;
  BufferPoolManager& bpm_;
  RollbackManager& rollback_manager_;

  lsn_t read_checkpoint_marker() const;

  void apply_checkpoint_snapshot(const LogRecord& record,
                                 std::unordered_map<txn_id, lsn_t>& active_transaction_table,
                                 std::unordered_map<page_id_t, lsn_t>& dirty_page_table);

  void process_log_record(const LogRecord& record,
                          std::unordered_map<txn_id, lsn_t>& active_transaction_table,
                          std::unordered_map<page_id_t, lsn_t>& dirty_page_table);

  template <typename PatchType, typename Deserializer>
  void process_update(const LogRecord& record,
                      std::unordered_map<txn_id, lsn_t>& active_transaction_table,
                      std::unordered_map<page_id_t, lsn_t>& dirty_page_table,
                      Deserializer deserializer);

  page_id_t extract_page_id(const LogRecord& record) const;
  bool redo_record(const LogRecord& record, Page* page) const;
  int redo(const AnalysisResult& analysis);
};

}
