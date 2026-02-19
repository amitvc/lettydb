#include "buffer/buffer_pool_manager.h"
#include "common/logger.h"

#include <cassert>

namespace letty {

BufferPoolManager::BufferPoolManager(IDiskManager& disk_manager, size_t pool_size,
                                     std::unique_ptr<PageReplacer> replacer)
    : disk_manager_(disk_manager),
      pool_size_(pool_size),
      pages_(pool_size_),
      replacer_(std::move(replacer)) {

  // All frames start on the free list
  for (frame_id_t i = 0; i < static_cast<frame_id_t>(pool_size); ++i) {
    free_list_.push_back(i);
  }
}

BufferPoolManager::~BufferPoolManager() {
  checkpoint();
}

Page* BufferPoolManager::fetch_page(page_id_t page_id) {
  std::lock_guard<std::mutex> lock(latch_);

  // 1. Check if page is already in the pool (cache hit)
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

  // 2. Cache miss — need a frame.
  ++cache_misses_;
  auto new_frame = acquire_frame();
  if (!new_frame) return nullptr;
  frame_id_t frame_id = new_frame.value();

  // 3. Load the requested page into the frame
  Page& page = pages_[frame_id];
  page.reset(); // Clear the current page
  LOG_STORAGE_DEBUG("Buffer pool miss: page {}, frame {}", page_id, frame_id);
  disk_manager_.read_page(page_id, page.get_data());
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

  if (is_dirty) {
    page.set_dirty(true);
  }

  page.decrement_pin();

  if (page.get_pin_count() == 0) {
	replacer_->mark_evictable(frame_id);
  }

  return true;
}

Page* BufferPoolManager::new_page(page_id_t page_id) {
  std::lock_guard<std::mutex> lock(latch_);

  // Don't allow duplicate pages
  if (page_table_.find(page_id) != page_table_.end()) {
    return nullptr;
  }

  // Find a frame (free list or eviction)
  auto maybe_frame = acquire_frame();
  if (!maybe_frame) return nullptr;
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

std::optional<frame_id_t> BufferPoolManager::acquire_frame() {
  if (!free_list_.empty()) {
    frame_id_t frame_id = free_list_.front();
    free_list_.pop_front();
    return frame_id;
  }

  // No free frames available. We need to evict least recently used frame. Identify frame to evict.
  auto victim_id = replacer_->identify_frame_to_evict();
  if (!victim_id) {
    LOG_STORAGE_WARN("Buffer pool full: {} frames, all pinned", pool_size_);
    return std::nullopt;
  }
  frame_id_t frame_id = victim_id.value();

  // Evict the victim
  ++evictions_;
  Page& victim = pages_[frame_id];
  if (victim.is_dirty()) {
    ++dirty_evictions_;
    LOG_STORAGE_DEBUG("Dirty eviction: writing page {} to disk", victim.get_page_id());
    disk_manager_.write_page(victim.get_page_id(), victim.get_data());
  }
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
    disk_manager_.write_page(page_id, page.get_data());
    page.set_dirty(false);
    ++flushes_;
  }

  return true;
}

void BufferPoolManager::flush_all_pages() {
  std::lock_guard<std::mutex> lock(latch_);

  uint64_t flushed_count = 0;
  for (auto& [page_id, frame_id] : page_table_) {
    Page& page = pages_[frame_id];
    if (page.is_dirty()) {
      disk_manager_.write_page(page_id, page.get_data());
      page.set_dirty(false);
      ++flushed_count;
    }
  }

  if (flushed_count > 0) {
    flushes_ += flushed_count;
    LOG_STORAGE_INFO("Flushed {} dirty pages", flushed_count);
  }
}

void BufferPoolManager::checkpoint() {
  flush_all_pages();
  disk_manager_.sync();
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

}  // namespace letty
