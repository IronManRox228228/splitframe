#include "media/frame_service.h"

#include <QThreadPool>

#include <algorithm>
#include <future>

namespace sf {

struct FrameService::Pool {
  QThreadPool pool;
};

struct FrameService::Asset {
  struct Waiter {
    qint64 index;
    std::promise<VideoFramePtr> promise;
  };

  AssetId id = 0;
  QString path;
  std::shared_ptr<std::atomic<int>> hwCount;
  bool countedHw = false;

  mutable std::mutex m; // guards everything below except `dec`
  bool running = false; // a worker owns the decoder right now
  std::atomic<bool> closed{false};
  bool failed = false;
  bool opened = false;
  VideoStreamInfo info;
  QString decoderName;
  std::optional<qint64> target;
  qint64 play = -1;
  int dir = 0;
  std::vector<Waiter> waiters;

  std::atomic<quint64> targetSeq{0}; // bumped by each new scrub target: abandons the one in flight
  std::atomic<quint64> anySeq{0};    // bumped by any change of demand: abandons read-ahead
  std::unique_ptr<VideoDecoder> dec; // touched only by the worker holding `running`

  ~Asset() {
    if (countedHw && hwCount) --*hwCount;
  }
};

FrameService::FrameService(const Options& options, QObject* parent)
    : QObject(parent), options_(options), cache_(options.cacheBytes), pool_(std::make_unique<Pool>()),
      hwCount_(std::make_shared<std::atomic<int>>(0)) {
  pool_->pool.setMaxThreadCount(std::max(1, options.workerThreads));
  if (options.gpuFrames) {
    cache_.setGpuLimit(options.gpuFramesPerAsset, [this](AssetId id, const VideoFrame& f) -> VideoFramePtr {
      const auto a = find(id);
      if (!a) return nullptr;
      {
        // while playing, what falls out is behind the playhead: not worth a download. When paused or
        // scrubbing, the neighbourhood is what the user is about to step through.
        const std::lock_guard al(a->m);
        if (a->dir != 0) return nullptr;
      }
      auto out = std::make_shared<VideoFrame>();
      out->index = f.index;
      out->ptsSec = f.ptsSec;
      out->hardware = f.hardware;
      out->trc = f.trc;
      out->prim = f.prim;
      out->image = frameImage(f);
      return out->image.isNull() ? nullptr : out;
    });
  }
}

FrameService::~FrameService() {
  {
    const std::lock_guard lock(m_);
    for (auto& [id, a] : assets_) {
      const std::lock_guard al(a->m);
      a->closed = true;
      ++a->anySeq;
      ++a->targetSeq;
      for (auto& w : a->waiters) w.promise.set_value(nullptr);
      a->waiters.clear();
    }
  }
  pool_->pool.waitForDone();
}

std::shared_ptr<FrameService::Asset> FrameService::find(AssetId id) const {
  const std::lock_guard lock(m_);
  const auto it = assets_.find(id);
  return it == assets_.end() ? nullptr : it->second;
}

AssetId FrameService::openAsset(const QString& path) {
  auto a = std::make_shared<Asset>();
  a->path = path;
  a->hwCount = hwCount_;
  {
    const std::lock_guard lock(m_);
    a->id = nextId_++;
    assets_[a->id] = a;
  }
  const std::lock_guard al(a->m);
  scheduleLocked(a); // the worker opens the decoder before looking for work
  return a->id;
}

void FrameService::closeAsset(AssetId id) {
  std::shared_ptr<Asset> a;
  {
    const std::lock_guard lock(m_);
    const auto it = assets_.find(id);
    if (it == assets_.end()) return;
    a = it->second;
    assets_.erase(it);
  }
  {
    const std::lock_guard al(a->m);
    a->closed = true;
    ++a->anySeq;
    ++a->targetSeq;
    for (auto& w : a->waiters) w.promise.set_value(nullptr);
    a->waiters.clear();
  }
  cache_.clearAsset(id);
}

std::optional<VideoStreamInfo> FrameService::info(AssetId id) const {
  const auto a = find(id);
  if (!a) return std::nullopt;
  const std::lock_guard al(a->m);
  if (!a->opened) return std::nullopt;
  return a->info;
}

bool FrameService::failed(AssetId id) const {
  const auto a = find(id);
  if (!a) return true;
  const std::lock_guard al(a->m);
  return a->failed;
}

QString FrameService::decoderName(AssetId id) const {
  const auto a = find(id);
  if (!a) return {};
  const std::lock_guard al(a->m);
  return a->decoderName;
}

std::optional<qint64> FrameService::indexAtTime(AssetId id, double sec) const {
  const auto a = find(id);
  if (!a) return std::nullopt;
  {
    const std::lock_guard al(a->m);
    if (!a->opened) return std::nullopt;
  }
  return a->dec->indexAtTime(sec); // the frame table is immutable once opened
}

VideoFramePtr FrameService::cached(AssetId id, qint64 index) const {
  return const_cast<FrameCache&>(cache_).get(id, index);
}

void FrameService::requestFrame(AssetId id, qint64 index) {
  const auto a = find(id);
  if (!a) return;
  if (cache_.contains(id, index)) {
    cache_.get(id, index); // refresh LRU so the frame being looked at survives read-ahead
    emit frameReady(id, index);
    return;
  }
  const std::lock_guard al(a->m);
  if (a->closed) return;
  a->target = index;
  ++a->targetSeq;
  ++a->anySeq;
  scheduleLocked(a);
}

void FrameService::setPlayhead(AssetId id, qint64 index, int direction) {
  const auto a = find(id);
  if (!a) return;
  const std::lock_guard al(a->m);
  if (a->closed) return;
  const bool jump = direction != a->dir || index < a->play || index > a->play + options_.readAhead;
  a->play = index;
  a->dir = direction;
  if (jump) ++a->anySeq;
  if (direction != 0) scheduleLocked(a);
}

VideoFramePtr FrameService::frameBlocking(AssetId id, qint64 index) {
  if (auto f = cache_.get(id, index)) return f;
  const auto a = find(id);
  if (!a) return nullptr;
  std::future<VideoFramePtr> result;
  {
    const std::lock_guard al(a->m);
    if (a->closed) return nullptr;
    Asset::Waiter w{index, {}};
    result = w.promise.get_future();
    a->waiters.push_back(std::move(w));
    ++a->anySeq;
    scheduleLocked(a);
  }
  return result.get();
}

void FrameService::scheduleLocked(const std::shared_ptr<Asset>& a) {
  if (a->running || a->closed) return;
  a->running = true;
  pool_->pool.start([this, a] { runAsset(a); });
}

void FrameService::runAsset(const std::shared_ptr<Asset>& a) {
  auto finish = [&](const QString& why) {
    // fails everything queued: nobody is going to decode for them
    const std::lock_guard al(a->m);
    a->running = false;
    for (auto& w : a->waiters) w.promise.set_value(nullptr);
    a->waiters.clear();
    if (!why.isEmpty()) emit assetFailed(a->id, why);
  };

  if (!a->dec) {
    VideoOpenOptions o;
    o.hw = options_.hw;
    o.gpu = options_.gpuFrames;
    if (o.hw == HwMode::Auto && hwCount_->load() >= options_.maxHardwareDecoders) o.hw = HwMode::Off;
    QString error;
    a->dec = VideoDecoder::open(a->path, o, &error);
    if (!a->dec) {
      {
        const std::lock_guard al(a->m);
        a->failed = true;
      }
      finish(error);
      return;
    }
    {
      const std::lock_guard al(a->m);
      a->info = a->dec->info();
      a->decoderName = a->dec->decoderName();
      a->opened = true;
      if (a->decoderName.contains(QLatin1String("d3d11va"))) {
        a->countedHw = true;
        ++*hwCount_;
      }
    }
    emit assetOpened(a->id);
  }

  const qint64 count = a->dec->frameCount();
  enum class Kind { Waiter, Target, Ahead };
  while (true) {
    Kind kind = Kind::Ahead;
    qint64 index = -1;
    int dir = 0;
    quint64 targetSeq = 0;
    quint64 anySeq = 0;
    {
      std::unique_lock al(a->m);
      if (a->closed) {
        a->running = false;
        al.unlock();
        cache_.clearAsset(a->id); // a frame decoded during close() may have been put after its clear
        return;
      }
      // waiters already satisfied by read-ahead
      for (auto it = a->waiters.begin(); it != a->waiters.end();) {
        if (auto f = cache_.get(a->id, it->index)) {
          it->promise.set_value(std::move(f));
          it = a->waiters.erase(it);
        } else {
          ++it;
        }
      }
      anySeq = a->anySeq;
      targetSeq = a->targetSeq;
      dir = a->dir;
      if (!a->waiters.empty()) {
        kind = Kind::Waiter;
        index = std::min(a->waiters.front().index, count - 1);
      } else if (a->target) {
        kind = Kind::Target;
        index = std::clamp<qint64>(*a->target, 0, count - 1);
        a->target.reset();
        if (cache_.contains(a->id, index)) {
          al.unlock();
          emit frameReady(a->id, index);
          continue;
        }
      } else if (a->dir != 0 && a->play >= 0) {
        // GPU frames pin decoder surfaces and the cache keeps only gpuFramesPerAsset of them: reading
        // further ahead than that would evict the very frames about to be shown
        const bool onGpu = options_.gpuFrames && a->decoderName.contains(QLatin1String("d3d11va"));
        const int ahead = onGpu ? std::clamp(options_.gpuFramesPerAsset - 3, 1, options_.readAhead) : options_.readAhead;
        for (int i = 1; i <= ahead; ++i) {
          const qint64 idx = a->play + static_cast<qint64>(a->dir) * i;
          if (idx < 0 || idx >= count) break;
          if (!cache_.contains(a->id, idx)) {
            index = idx;
            break;
          }
        }
      }
      if (index < 0) {
        a->running = false;
        return;
      }
    }

    VideoDecoder::Cancel cancel;
    qint64 keepFrom = -1;
    if (kind == Kind::Target) {
      cancel = [&a, targetSeq] { return a->targetSeq.load() != targetSeq; };
      keepFrom = std::max<qint64>(0, index - options_.scrubKeepBehind);
    } else if (kind == Kind::Ahead) {
      cancel = [&a, anySeq] { return a->anySeq.load() != anySeq; };
      if (dir < 0) keepFrom = std::max<qint64>(0, index - options_.readAhead);
    } else {
      cancel = [&a] { return a->closed.load(); };
      keepFrom = dir < 0 ? std::max<qint64>(0, index - options_.readAhead) : -1;
    }
    const AssetId id = a->id;
    VideoFramePtr frame = a->dec->frameAt(index, cancel, keepFrom, [this, id](VideoFramePtr f) { cache_.put(id, std::move(f)); });
    if (frame) {
      cache_.put(id, frame);
      if (kind == Kind::Target) emit frameReady(id, index);
      if (kind == Kind::Waiter) {
        // hand it over directly: a frame bigger than the cache limit is never cached
        const std::lock_guard al(a->m);
        for (auto it = a->waiters.begin(); it != a->waiters.end();) {
          if (std::min(it->index, count - 1) == index) {
            it->promise.set_value(frame);
            it = a->waiters.erase(it);
          } else {
            ++it;
          }
        }
      }
    } else if (kind == Kind::Waiter) {
      // hard failure: release the waiters for this frame instead of retrying forever
      const std::lock_guard al(a->m);
      for (auto it = a->waiters.begin(); it != a->waiters.end();) {
        if (std::min(it->index, count - 1) == index) {
          it->promise.set_value(nullptr);
          it = a->waiters.erase(it);
        } else {
          ++it;
        }
      }
    } else if (kind == Kind::Ahead && a->anySeq.load() == anySeq) {
      // read-ahead failed for real (not cancelled): stop trying until the playhead moves
      const std::lock_guard al(a->m);
      a->dir = 0;
    }
  }
}

} // namespace sf
