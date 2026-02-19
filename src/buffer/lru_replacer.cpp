#include "buffer/lru_replacer.h"

namespace letty {

std::optional<frame_id_t> LRUPageReplacer::identify_frame_to_evict() {
  if (lru_list_.empty()) {
    return std::nullopt;
  }

  // Evict from the front (least recently used)
  frame_id_t frame_id = lru_list_.front();
  map_.erase(frame_id);
  lru_list_.pop_front();
  return frame_id;
}

void LRUPageReplacer::mark_not_evictable(frame_id_t frame_id) {
  auto it = map_.find(frame_id);
  if (it == map_.end()) {
    return;  // Not in replacer — already pinned or never tracked
  }

  lru_list_.erase(it->second);
  map_.erase(it);
}

void LRUPageReplacer::mark_evictable(frame_id_t frame_id) {
  if (map_.count(frame_id) > 0) {
    return;  // Already in replacer — idempotent
  }

  // Most recently used goes at the back of the list.
  lru_list_.push_back(frame_id);
  map_[frame_id] = std::prev(lru_list_.end());
}

}
