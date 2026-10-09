#pragma once

#include "media/video_decoder.h"

#include <QtGlobal>

#include <list>
#include <mutex>
#include <unordered_map>

namespace sf {

using AssetId = quint64;

struct FrameKey {
  AssetId asset = 0;
  qint64 frame = 0;
  bool operator==(const FrameKey&) const = default;
};

struct FrameKeyHash {
  size_t operator()(const FrameKey& k) const noexcept {
    return std::hash<quint64>()(k.asset * 0x9E3779B97F4A7C15ull ^ static_cast<quint64>(k.frame));
  }
};

// Decoded frames, least-recently-used out first, bounded by the bytes of pixel data held.
// Entries are shared_ptr: evicting a frame never invalidates one a consumer (the compositor, an
// export job) is still using, it just stops the cache from keeping it alive. So the byte limit
// bounds the cache, not the frames in flight.
// Thread-safe: all members lock internally (short critical sections, no decoding under the lock).
class FrameCache {
public:
  explicit FrameCache(qint64 byteLimit = 512ll << 20) : limit_(byteLimit) {}

  // Moves the entry to most-recent. Null on miss.
  VideoFramePtr get(AssetId asset, qint64 frame);
  // Membership test that doesn't disturb the LRU order or the hit counters.
  bool contains(AssetId asset, qint64 frame) const;
  // Inserts (or refreshes) and evicts down to the limit. A frame larger than the whole limit is not kept.
  void put(AssetId asset, VideoFramePtr frame);
  void clearAsset(AssetId asset);
  void clear();

  void setByteLimit(qint64 bytes);
  qint64 byteLimit() const;
  qint64 bytes() const;
  qint64 count() const;

  struct Stats {
    qint64 hits = 0;
    qint64 misses = 0;
    qint64 evictions = 0;
  };
  Stats stats() const;

private:
  struct Entry {
    FrameKey key;
    VideoFramePtr frame;
  };
  void evictLocked();

  mutable std::mutex m_;
  std::list<Entry> lru_; // front = most recent
  std::unordered_map<FrameKey, std::list<Entry>::iterator, FrameKeyHash> map_;
  qint64 limit_;
  qint64 bytes_ = 0;
  mutable Stats stats_;
};

} // namespace sf
