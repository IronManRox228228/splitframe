#include "media/frame_cache.h"

namespace sf {

VideoFramePtr FrameCache::get(AssetId asset, qint64 frame) {
  const std::lock_guard lock(m_);
  const auto it = map_.find({asset, frame});
  if (it == map_.end()) {
    ++stats_.misses;
    return nullptr;
  }
  ++stats_.hits;
  lru_.splice(lru_.begin(), lru_, it->second);
  return it->second->frame;
}

bool FrameCache::contains(AssetId asset, qint64 frame) const {
  const std::lock_guard lock(m_);
  return map_.contains({asset, frame});
}

void FrameCache::put(AssetId asset, VideoFramePtr frame) {
  if (!frame) return;
  const FrameKey key{asset, frame->index};
  const std::lock_guard lock(m_);
  if (const auto it = map_.find(key); it != map_.end()) {
    bytes_ -= it->second->frame->byteSize();
    lru_.erase(it->second);
    map_.erase(it);
  }
  if (frame->byteSize() > limit_) return;
  bytes_ += frame->byteSize();
  lru_.push_front({key, std::move(frame)});
  map_[key] = lru_.begin();
  evictLocked();
}

void FrameCache::evictLocked() {
  while (bytes_ > limit_ && !lru_.empty()) {
    const Entry& e = lru_.back();
    bytes_ -= e.frame->byteSize();
    map_.erase(e.key);
    lru_.pop_back();
    ++stats_.evictions;
  }
}

void FrameCache::clearAsset(AssetId asset) {
  const std::lock_guard lock(m_);
  for (auto it = lru_.begin(); it != lru_.end();) {
    if (it->key.asset == asset) {
      bytes_ -= it->frame->byteSize();
      map_.erase(it->key);
      it = lru_.erase(it);
    } else {
      ++it;
    }
  }
}

void FrameCache::clear() {
  const std::lock_guard lock(m_);
  lru_.clear();
  map_.clear();
  bytes_ = 0;
}

void FrameCache::setByteLimit(qint64 bytes) {
  const std::lock_guard lock(m_);
  limit_ = bytes;
  evictLocked();
}

qint64 FrameCache::byteLimit() const {
  const std::lock_guard lock(m_);
  return limit_;
}
qint64 FrameCache::bytes() const {
  const std::lock_guard lock(m_);
  return bytes_;
}
qint64 FrameCache::count() const {
  const std::lock_guard lock(m_);
  return static_cast<qint64>(lru_.size());
}
FrameCache::Stats FrameCache::stats() const {
  const std::lock_guard lock(m_);
  return stats_;
}

} // namespace sf
