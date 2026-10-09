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

void FrameCache::eraseLocked(std::list<Entry>::iterator it) {
  bytes_ -= it->frame->byteSize();
  if (it->frame->gpu) {
    const auto g = gpuPerAsset_.find(it->key.asset);
    if (g != gpuPerAsset_.end() && --g->second <= 0) gpuPerAsset_.erase(g);
  }
  map_.erase(it->key);
  lru_.erase(it);
}

void FrameCache::put(AssetId asset, VideoFramePtr frame) {
  if (!frame) return;
  const FrameKey key{asset, frame->index};
  std::vector<VideoFramePtr> victims;
  Demoter demoter;
  {
    const std::lock_guard lock(m_);
    if (const auto it = map_.find(key); it != map_.end()) eraseLocked(it->second);
    if (frame->byteSize() > limit_) return;
    bytes_ += frame->byteSize();
    const bool gpu = static_cast<bool>(frame->gpu);
    lru_.push_front({key, std::move(frame)});
    map_[key] = lru_.begin();
    if (gpu) ++gpuPerAsset_[asset];
    evictLocked();
    // surfaces are scarcer than bytes: shed this asset's least recently used GPU frames
    if (gpu && map_.contains(key)) {
      for (auto it = lru_.end(); gpuPerAsset_[asset] > gpuLimit_ && it != lru_.begin();) {
        --it;
        if (it->key.asset != asset || !it->frame->gpu || it->key == key) continue;
        victims.push_back(it->frame);
        const auto next = std::next(it);
        eraseLocked(it);
        it = next;
      }
    }
    demoter = demoter_;
  }
  for (VideoFramePtr& v : victims) {
    VideoFramePtr replacement = demoter ? demoter(asset, *v) : nullptr;
    v.reset(); // release the surface before (possibly) taking the lock again
    if (!replacement || replacement->gpu) continue;
    const std::lock_guard lock(m_);
    const FrameKey k{asset, replacement->index};
    if (map_.contains(k) || replacement->byteSize() > limit_) continue;
    bytes_ += replacement->byteSize();
    lru_.push_back({k, std::move(replacement)}); // it was the least recent, so it stays that way
    map_[k] = std::prev(lru_.end());
    evictLocked();
  }
}

void FrameCache::evictLocked() {
  while (bytes_ > limit_ && !lru_.empty()) {
    eraseLocked(std::prev(lru_.end()));
    ++stats_.evictions;
  }
}

void FrameCache::setGpuLimit(int perAsset, Demoter demoter) {
  const std::lock_guard lock(m_);
  gpuLimit_ = perAsset;
  demoter_ = std::move(demoter);
}

int FrameCache::gpuLimit() const {
  const std::lock_guard lock(m_);
  return gpuLimit_;
}

qint64 FrameCache::gpuCount() const {
  const std::lock_guard lock(m_);
  qint64 n = 0;
  for (const auto& [asset, c] : gpuPerAsset_) n += c;
  return n;
}

void FrameCache::clearAsset(AssetId asset) {
  const std::lock_guard lock(m_);
  for (auto it = lru_.begin(); it != lru_.end();) {
    const auto next = std::next(it);
    if (it->key.asset == asset) eraseLocked(it);
    it = next;
  }
}

void FrameCache::clear() {
  const std::lock_guard lock(m_);
  lru_.clear();
  map_.clear();
  gpuPerAsset_.clear();
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
