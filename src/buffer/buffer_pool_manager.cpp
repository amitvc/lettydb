#include "buffer/buffer_pool_manager.h"
#include "common/logger.h"
#include <stdexcept>

namespace letty {

BufferPoolManager::BufferPoolManager(IDiskManager& disk_manager, size_t pool_size,
                                     std::unique_ptr<PageReplacer> replacer)
    : disk_manager_(disk_manager),
      pool_size_(pool_size),
      pages_(pool_size_),
      replacer_(std::move(replacer)) {

  if (pool_size_ == 0) {
	LOG_BPM_ERROR("Cannot have Buffer pool manager with pool size of 0");
	throw std::invalid_argument("Cannot have Buffer pool manager with pool size of 0");
  }

  if (replacer_ == nullptr) {
	LOG_BPM_ERROR("PageReplacer cannot be null");
	throw std::invalid_argument("PageReplacer cannot be null");
  }

  page_data_buffer_ = static_cast<char*>(std::aligned_alloc(PAGE_SIZE, pool_size_ * PAGE_SIZE));
  LOG_BPM_INFO("Allocating {} bytes of memory to load disk pages into the RAM", (pool_size_ * PAGE_SIZE));
  if (page_data_buffer_ == nullptr) {
	LOG_BPM_ERROR("Problem allocating initial memory required for loading disk pages into RAM");
	throw std::bad_alloc();
  }

  // Attach each Page metadata object to its fixed PAGE_SIZE slice in the aligned frame buffer.
  for (size_t i = 0; i < pool_size_; ++i) {
    pages_[i].data_ = page_data_buffer_ + (i * PAGE_SIZE);
    std::memset(pages_[i].data_, 0, PAGE_SIZE);
  }

  for (frame_id_t i = 0; i < static_cast<frame_id_t>(pool_size); ++i) {
    free_list_.push_back(i);
  }
}

BufferPoolManager::~BufferPoolManager() {
  if (!checkpoint()) {
	LOG_BPM_ERROR("Checkpoint failure encountered while BPM is being shutdown.");
  }
  std::free(page_data_buffer_);
}

Page* BufferPoolManager::fetch_page(page_id_t page_id) {
  std::lock_guard<std::mutex> lock(latch_);

  auto it = page_table_.find(page_id);
  if (it != page_table_.end()) {
	// Found the page in cache
    frame_id_t frame_id = it->second;
    Page& page = pages_[frame_id];
    page.increment_pin();
	replacer_->mark_not_evictable(frame_id); // Page is in use so do not evict.
    ++cache_hits_;
    return &page;
  }

  // Cache miss — need a frame
  ++cache_misses_;
  auto new_frame = acquire_frame();
  if (!new_frame) {
	LOG_BPM_WARN("Failed to fetch page {}: no buffer frame available", page_id);
	return nullptr;
  }
  frame_id_t frame_id = new_frame.value();

  Page& page = pages_[frame_id];
  page.reset(); // Clear the current page
  LOG_BPM_DEBUG("Buffer pool miss: page {}, frame {}", page_id, frame_id);
  IOResult result = disk_manager_.read_page(page_id, page.get_data());
  if (result != IOResult::SUCCESS) {
    LOG_BPM_ERROR("Failed to read page {} into frame {}", page_id, frame_id);
    page.reset();
    free_list_.push_back(frame_id);
	return nullptr;
  }
  page.set_page_id(page_id);
  page.increment_pin();

  page_table_[page_id] = frame_id;

  return &page;
}

bool BufferPoolManager::unpin_page(page_id_t page_id, bool is_dirty) {
  std::lock_guard<std::mutex> lock(latch_);

  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return false;
  }

  frame_id_t frame_id = it->second;
  Page& page = pages_[frame_id];

  if (page.get_pin_count() <= 0) {
    LOG_BPM_WARN("Failed to unpin page {}: pin count is already {}", page_id, page.get_pin_count());
    return false;
  }

  if (is_dirty) {
    page.set_dirty(true);
  }

  page.decrement_pin();

  // If a Page's pin count (aka reference count) drops to 0 then it is safe to be evicted from cache
  if (page.get_pin_count() == 0) {
	replacer_->mark_evictable(frame_id);
  }

  return true;
}

Page* BufferPoolManager::new_page(page_id_t page_id) {
  std::lock_guard<std::mutex> lock(latch_);

  // Don't allow duplicate pages
  if (page_table_.find(page_id) != page_table_.end()) {
	LOG_BPM_WARN("Failed to create page {}: page is already in buffer pool", page_id);
    return nullptr;
  }

  // Find a frame (free list or eviction)
  auto maybe_frame = acquire_frame();
  if (!maybe_frame) {
	LOG_BPM_WARN("Failed to create page {}: no buffer frame available", page_id);
	return nullptr;
  }
  frame_id_t frame_id = *maybe_frame;

  // Initialize the new page
  Page& page = pages_[frame_id];
  page.reset();
  page.set_page_id(page_id);
  page.set_dirty(true);
  page.increment_pin();

  page_table_[page_id] = frame_id;

  return &page;
}

Page* BufferPoolManager::new_or_fetch_page(page_id_t page_id) {
  std::lock_guard<std::mutex> lock(latch_);

  auto it = page_table_.find(page_id);
  if (it != page_table_.end()) {
    frame_id_t frame_id = it->second;
    Page& page = pages_[frame_id];
    page.increment_pin();
    replacer_->mark_not_evictable(frame_id);
    return &page;
  }

  auto maybe_frame = acquire_frame();
  if (!maybe_frame) {
    LOG_BPM_WARN("Failed to create page {}: no buffer frame available", page_id);
    return nullptr;
  }

  frame_id_t frame_id = *maybe_frame;
  Page& page = pages_[frame_id];
  page.reset();
  page.set_page_id(page_id);
  page.set_dirty(true);
  page.increment_pin();

  page_table_[page_id] = frame_id;
  return &page;
}

std::optional<frame_id_t> BufferPoolManager::acquire_frame() {
  if (!free_list_.empty()) {
    frame_id_t frame_id = free_list_.front();
    free_list_.pop_front();
    return frame_id;
  }

  // No free frames available. We need to evict least recently used frame. Identify frame to evict.
  auto victim_id = replacer_->identify_frame_to_evict();
  if (!victim_id) {
	LOG_BPM_WARN("Buffer pool full: {} frames, all pinned", pool_size_);
    return std::nullopt;
  }
  frame_id_t frame_id = victim_id.value();

  Page& victim = pages_[frame_id];
  if (victim.is_dirty()) {
    LOG_BPM_DEBUG("Dirty eviction: writing page {} to disk", victim.get_page_id());
    IOResult result = disk_manager_.write_page(victim.get_page_id(), victim.get_data());
    if (result != IOResult::SUCCESS) {
      LOG_BPM_ERROR("Failed to evict dirty page {} from frame {}", victim.get_page_id(), frame_id);
      replacer_->mark_evictable(frame_id);
      return std::nullopt;
    }
    ++dirty_evictions_;
  }

  ++evictions_;
  page_table_.erase(victim.get_page_id());
  return frame_id;
}

bool BufferPoolManager::flush_page(page_id_t page_id) {
  std::lock_guard<std::mutex> lock(latch_);

  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return false;
  }

  frame_id_t frame_id = it->second;
  Page& page = pages_[frame_id];

  if (page.is_dirty()) {
    IOResult result = disk_manager_.write_page(page_id, page.get_data());
    if (result != IOResult::SUCCESS) {
      LOG_BPM_ERROR("Failed to flush dirty page {}", page_id);
      return false;
    }
    page.set_dirty(false);
    ++flushes_;
  }

  return true;
}

bool BufferPoolManager::flush_all_pages() {
  std::lock_guard<std::mutex> lock(latch_);

  uint64_t flushed_count = 0;
  uint64_t failed_count = 0;
  for (auto& [page_id, frame_id] : page_table_) {
    Page& page = pages_[frame_id];
    if (page.is_dirty()) {
      IOResult result = disk_manager_.write_page(page_id, page.get_data());
      if (result != IOResult::SUCCESS) {
        ++failed_count;
        LOG_BPM_ERROR("Failed to flush dirty page {}", page_id);
        continue;
      }
      page.set_dirty(false);
      ++flushed_count;
    }
  }

  if (flushed_count > 0) {
    flushes_ += flushed_count;
    LOG_BPM_INFO("Flushed {} dirty pages", flushed_count);
  }
  if (failed_count > 0) {
    LOG_BPM_ERROR("Failed to flush {} dirty page(s)", failed_count);
  }

  return failed_count == 0;
}

bool BufferPoolManager::checkpoint() {
  bool flushed = flush_all_pages();
  bool synced = disk_manager_.sync();
  return flushed && synced;
}

bool BufferPoolManager::delete_page(page_id_t page_id) {
  std::lock_guard<std::mutex> lock(latch_);

  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return false;
  }

  frame_id_t frame_id = it->second;
  Page& page = pages_[frame_id];

  // Cannot delete a pinned page
  if (page.get_pin_count() > 0) {
    return false;
  }

  // Remove from replacer (it's unpinned, so it's tracked there)
  replacer_->mark_not_evictable(frame_id);

  // Clean up
  page.reset();
  page_table_.erase(it);
  free_list_.push_back(frame_id);

  return true;
}

CacheStats BufferPoolManager::get_cache_stats() const {
  return {
    cache_hits_.load(),
    cache_misses_.load(),
    evictions_.load(),
    dirty_evictions_.load(),
    flushes_.load()
  };
}

void BufferPoolManager::reset_cache_stats() {
  cache_hits_ = 0;
  cache_misses_ = 0;
  evictions_ = 0;
  dirty_evictions_ = 0;
  flushes_ = 0;
}

}
