#pragma once

#include <list>
#include <optional>
#include <unordered_map>
#include "buffer/page_replacer.h"

namespace letty {

/**
 * @class LRUPageReplacer
 * @brief Eviction policy using Least Recently Used (LRU) ordering.
 *
 * Tracks unpinned frames in a std::list ordered by access time.
 * Front = least recently used (evict first),
 * Back = most recently used.
 * When an entry needs to be evicted it will be from front of list. The caller
 * needs to call the victim api.
 *
 * All operations are O(1) via a hash map from frame_id to list iterator.
 *
 * Thread safety: None. Caller (BufferPoolManager) must hold its own latch.
 * Note:
 */
class LRUPageReplacer : public PageReplacer {
 public:
  LRUPageReplacer() = default;

  std::optional<frame_id_t> identify_frame_to_evict() override;
  void mark_not_evictable(frame_id_t frame_id) override;
  void mark_evictable(frame_id_t frame_id) override;
  size_t size() const override { return lru_list_.size(); }

 private:
  // Front = least recently used (evict first), Back = most recently used.
  std::list<frame_id_t> lru_list_;

  // Maps frame_id -> iterator into lru_list_ for O(1) removal.
  std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator> map_;
};

}
