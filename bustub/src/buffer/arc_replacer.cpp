// :bustub-keep-private:
//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// arc_replacer.cpp
//
// Identification: src/buffer/arc_replacer.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "buffer/arc_replacer.h"
#include <optional>
#include "common/config.h"
#include "common/macros.h"

namespace bustub {

/**
 *
 * TODO(P1): Add implementation
 *
 * @brief a new ArcReplacer, with lists initialized to be empty and target size to 0
 * @param num_frames the maximum number of frames the ArcReplacer will be required to cache
 */
ArcReplacer::ArcReplacer(size_t num_frames) : replacer_size_(num_frames) {}

/**
 * TODO(P1): Add implementation
 *
 * @brief Performs the Replace operation as described by the writeup
 * that evicts from either mfu_ or mru_ into its corresponding ghost list
 * according to balancing policy.
 *
 * If you wish to refer to the original ARC paper, please note that there are
 * two changes in our implementation:
 * 1. When the size of mru_ equals the target size, we don't check
 * the last access as the paper did when deciding which list to evict from.
 * This is fine since the original decision is stated to be arbitrary.
 * 2. Entries that are not evictable are skipped. If all entries from the desired side
 * (mru_ / mfu_) are pinned, we instead try victimize the other side (mfu_ / mru_),
 * and move it to its corresponding ghost list (mfu_ghost_ / mru_ghost_).
 *
 * @return frame id of the evicted frame, or std::nullopt if cannot evict
 */

auto ArcReplacer::Evict() -> std::optional<frame_id_t> {
  std::scoped_lock lock(latch_);

  bool prefer_mru = mru_.size() >= mru_target_size_;
  auto &preferred = prefer_mru ? mru_ : mfu_;
  auto &fallback = prefer_mru ? mfu_ : mru_;

  auto find_victim = [this](std::list<frame_id_t> &list) -> std::optional<frame_id_t> {
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
      if (alive_map_[*it]->evictable_) {
        return *it;
      }
    }
    return std::nullopt;
  };

  auto victim_opt = find_victim(preferred);
  if (!victim_opt.has_value()) {
    victim_opt = find_victim(fallback);
  }
  if (!victim_opt.has_value()) {
    return std::nullopt;
  }

  frame_id_t victim = *victim_opt;
  auto fs = alive_map_[victim];
  page_id_t evicted_pid = fs->page_id_;

  // removing from alive list
  if (fs->arc_status_ == ArcStatus::MRU) {
    mru_.erase(alive_list_it_[victim]);
  } else {
    mfu_.erase(alive_list_it_[victim]);
  }

  alive_list_it_.erase(victim);
  alive_map_.erase(victim);

  // add to ghost list
  if (fs->arc_status_ == ArcStatus::MRU) {
    mru_ghost_.push_front(evicted_pid);
    ghost_list_it_[evicted_pid] = mru_ghost_.begin();
    ghost_map_[evicted_pid] = std::make_shared<FrameStatus>(evicted_pid, -1, false, ArcStatus::MRU_GHOST);
  } else {
    mfu_ghost_.push_front(evicted_pid);
    ghost_list_it_[evicted_pid] = mfu_ghost_.begin();
    ghost_map_[evicted_pid] = std::make_shared<FrameStatus>(evicted_pid, -1, false, ArcStatus::MFU_GHOST);
  }

  --curr_size_;
  return victim;
}

/**
 * TODO(P1): Add implementation
 *
 * @brief Record access to a frame, adjusting ARC bookkeeping accordingly
 * by bring the accessed page to the front of mfu_ if it exists in any of the lists
 * or the front of mru_ if it does not.
 *
 * Performs the operations EXCEPT REPLACE described in original paper, which is
 * handled by `Evict()`.
 *
 * Consider the following four cases, handle accordingly:
 * 1. Access hits mru_ or mfu_
 * 2/3. Access hits mru_ghost_ / mfu_ghost_
 * 4. Access misses all the lists
 *
 * This routine performs all changes to the four lists as preperation
 * for `Evict()` to simply find and evict a victim into ghost lists.
 *
 * Note that frame_id is used as identifier for alive pages and
 * page_id is used as identifier for the ghost pages, since page_id is
 * the unique identifier to the page after it's dead.
 * Using page_id for alive pages should be the same since it's one to one mapping,
 * but using frame_id is slightly more intuitive.
 *
 * @param frame_id id of frame that received a new access.
 * @param page_id id of page that is mapped to the frame.
 * @param access_type type of access that was received. This parameter is only needed for
 * leaderboard tests.
 */
void ArcReplacer::RecordAccess(frame_id_t frame_id, page_id_t page_id, [[maybe_unused]] AccessType access_type) {
  std::scoped_lock lock(latch_);

  BUSTUB_ASSERT(frame_id >= 0, "Invalid frame_id");

  auto it = alive_map_.find(frame_id);
  if (it == alive_map_.end()) {
    auto ghost_it = ghost_map_.find(page_id);
    if (ghost_it == ghost_map_.end()) {
      size_t mru_total = mru_.size() + mru_ghost_.size();
      size_t total = mru_total + mfu_.size() + mfu_ghost_.size();

      if (mru_total == replacer_size_) {
        page_id_t victim = mru_ghost_.back();
        mru_ghost_.pop_back();
        ghost_list_it_.erase(victim);
        ghost_map_.erase(victim);
      } else if (total == 2 * replacer_size_) {
        page_id_t victim = mfu_ghost_.back();
        mfu_ghost_.pop_back();
        ghost_list_it_.erase(victim);
        ghost_map_.erase(victim);
      }

      mru_.push_front(frame_id);
      auto fs = std::make_shared<FrameStatus>(page_id, frame_id, false, ArcStatus::MRU);
      alive_map_[frame_id] = fs;
      alive_list_it_[frame_id] = mru_.begin();
    } else {
      // Ghost Map
      auto ghost_fs = ghost_it->second;
      if (ghost_fs->arc_status_ == ArcStatus::MRU_GHOST) {
        size_t delta;
        if (mru_ghost_.size() >= mfu_ghost_.size()) {
          delta = 1;
        } else {
          delta = mfu_ghost_.size() / mru_ghost_.size();
        }
        mru_target_size_ = std::min(mru_target_size_ + delta, replacer_size_);
        mru_ghost_.erase(ghost_list_it_[page_id]);
      } else {
        size_t delta;
        if (mfu_ghost_.size() >= mru_ghost_.size()) {
          delta = 1;
        } else {
          delta = mru_ghost_.size() / mfu_ghost_.size();
        }
        mru_target_size_ = (delta >= mru_target_size_) ? 0 : mru_target_size_ - delta;
        mfu_ghost_.erase(ghost_list_it_[page_id]);
      }
      ghost_list_it_.erase(page_id);
      ghost_map_.erase(page_id);
      mfu_.push_front(frame_id);
      auto fs = std::make_shared<FrameStatus>(page_id, frame_id, false, ArcStatus::MFU);
      alive_map_[frame_id] = fs;
      alive_list_it_[frame_id] = mfu_.begin();
    }
  } else {
    // Alive Map
    // Cache hit
    auto fs = it->second;
    if (it->second->arc_status_ == ArcStatus::MRU) {
      mru_.erase(alive_list_it_[frame_id]);
    } else {
      mfu_.erase(alive_list_it_[frame_id]);
    }
    alive_list_it_.erase(frame_id);
    mfu_.push_front(frame_id);
    alive_list_it_[frame_id] = mfu_.begin();
    fs->arc_status_ = ArcStatus::MFU;
  }
}

/**
 * TODO(P1): Add implementation
 *
 * @brief Toggle whether a frame is evictable or non-evictable. This function also
 * controls replacer's size. Note that size is equal to number of evictable entries.
 *
 * If a frame was previously evictable and is to be set to non-evictable, then size should
 * decrement. If a frame was previously non-evictable and is to be set to evictable,
 * then size should increment.
 *
 * If frame id is invalid, throw an exception or abort the process.
 *
 * For other scenarios, this function should terminate without modifying anything.
 *
 * @param frame_id id of frame whose 'evictable' status will be modified
 * @param set_evictable whether the given frame is evictable or not
 */
void ArcReplacer::SetEvictable(frame_id_t frame_id, bool set_evictable) {
  std::scoped_lock lock(latch_);

  BUSTUB_ASSERT(frame_id >= 0, "Invalid frame_id");

  auto it = alive_map_.find(frame_id);
  if (it == alive_map_.end()) {
    return;
  }

  if (it->second->evictable_ == set_evictable) {
    return;
  }

  it->second->evictable_ = set_evictable;
  if (set_evictable) {
    ++curr_size_;
  } else {
    --curr_size_;
  }
}

/**
 * TODO(P1): Add implementation
 *
 * @brief Remove an evictable frame from replacer.
 * This function should also decrement replacer's size if removal is successful.
 *
 * Note that this is different from evicting a frame, which always remove the frame
 * decided by the ARC algorithm.
 *
 * If Remove is called on a non-evictable frame, throw an exception or abort the
 * process.
 *
 * If specified frame is not found, directly return from this function.
 *
 * @param frame_id id of frame to be removed
 */
void ArcReplacer::Remove(frame_id_t frame_id) {
  std::scoped_lock lock(latch_);
  BUSTUB_ASSERT(frame_id >= 0, "Invalid frame_id");

  auto it = alive_map_.find(frame_id);
  if (it == alive_map_.end()) {
    return;
  }

  auto fs = it->second;
  BUSTUB_ASSERT(fs->evictable_, "Cant remove non evictable frame");

  if (fs->arc_status_ == ArcStatus::MRU) {
    mru_.erase(alive_list_it_[frame_id]);
  } else {
    mfu_.erase(alive_list_it_[frame_id]);
  }
  alive_list_it_.erase(frame_id);
  alive_map_.erase(frame_id);
  --curr_size_;
}

/**
 * TODO(P1): Add implementation
 *
 * @brief Return replacer's size, which tracks the number of evictable frames.
 *
 * @return size_t
 */
auto ArcReplacer::Size() -> size_t {
  std::scoped_lock lock(latch_);
  return this->curr_size_;
}

}  // namespace bustub
