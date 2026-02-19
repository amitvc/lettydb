#pragma once

#include <optional>
#include "storage/config.h"

namespace letty {

/**
 * @class PageReplacer
 * @brief Abstract interface for buffer pool eviction policies.
 *
 * Tracks which frames are eligible for eviction and selects victims when
 * the BufferPoolManager needs to reclaim a frame. Implementations (e.g.,
 * LRUPageReplacer, ClockReplacer) define the eviction policy.
 *
 * Thread safety: None. Caller (BufferPoolManager) must hold its own latch.
 */
class PageReplacer {
 public:
  virtual ~PageReplacer() = default;

  /**
   * @brief Select a victim frame for eviction and remove it from the replacer.
   * @return The victim's frame ID, or std::nullopt if no evictable frames exist.
   */
  virtual std::optional<frame_id_t> identify_frame_to_evict() = 0;

  /**
   * @brief Remove a frame from the replacer — it is pinned and cannot be evicted.
   * No-op if the frame is not in the replacer.
   */
  virtual void mark_not_evictable(frame_id_t frame_id) = 0;

  /**
   * @brief Add a frame to the replacer — it is unpinned and can be evicted.
   * No-op if the frame is already in the replacer.
   */
  virtual void mark_evictable(frame_id_t frame_id) = 0;

  /** @brief Number of frames currently tracked (evictable). */
  virtual size_t size() const = 0;
};

}  // namespace letty
