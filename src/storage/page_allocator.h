//
// Created by Amit Chavan on 11/22/26.
//

#pragma once

#include <mutex>

#include "buffer/buffer_pool_manager.h"
#include "storage/log_manager.h"
#include "storage/storage_def.h"
#include "transaction/transaction_manager.h"

namespace letty {
  /**
   * @class PageAllocator
   * @brief Manages single-page allocation, deallocation, and doubly-linked table page chains.
   *
   * Reuses freed pages via an LIFO freelist rooted at Page 0
   * (DatabaseMetadata::free_page_list_head_page), extending the file when the
   * freelist is empty. Also maintains prev_page_id/next_page_id links for table
   * page chains. All Page 0 and PageHeader mutations are WAL-logged for ARIES
   * recovery and serialized under lock_.
   */
  class PageAllocator {

   public:
	explicit PageAllocator(BufferPoolManager &buffer_pool,
						   LogManager &log_manager,
						   TransactionManager &txn_manager);

	page_id_t allocate_page(uint16_t table_id, PageType type, txn_id tran_id);
	bool free_page(page_id_t page_id, txn_id tran_id);
	bool link_page(page_id_t tail_page_id, page_id_t new_page_id, txn_id tran_id);
	bool unlink_page(page_id_t page_id, txn_id tran_id);
	bool free_page_chain(page_id_t first_page_id, txn_id tran_id);

   private:
	void initialize_new_db();
	void validate_existing_db();
	bool free_page_locked(page_id_t page_id, txn_id tran_id);
	lsn_t log_db_metadata_change(txn_id tran_id,
								 const DbMetadataState& before,
								 const DbMetadataState& after);
	lsn_t log_page_header_change(txn_id tran_id,
								 page_id_t page_id,
								 const PageHeader& before,
								 const PageHeader& after);
	bool apply_db_metadata_update(PageGuard& db_header_guard,
								  const DbMetadataState& before_state,
								  const DatabaseMetadata& modified_meta,
								  txn_id tran_id);
	bool apply_page_header_update(PageGuard& page_guard,
								  page_id_t page_id,
								  const PageHeader& current_header,
								  PageHeader modified_header,
								  txn_id tran_id);
	static DbMetadataState extract_state(const DatabaseMetadata& metadata);
	BufferPoolManager& bpm_;
	LogManager& log_manager_;
	TransactionManager& txn_manager_;
	std::mutex lock_;
  };
}
