#pragma once

// Asynchronous frames for the UI and the compositor: nothing here decodes on the caller's thread.
//
// Each opened asset gets one VideoDecoder, driven by at most one worker at a time (decoder state is
// sequential), so N assets decode in parallel on a small pool but one asset's requests never race.
// Requests are priorities, not a FIFO:
//   1. blocking requests (export threads waiting on frameBlocking),
//   2. the latest scrub target (newer targets replace older ones, stale seeks are abandoned mid-GOP),
//   3. playback read-ahead: the next frames in the play direction, nearest first.
// Everything decoded lands in the shared FrameCache, which is what the compositor reads each paint.

#include "media/frame_cache.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <optional>
#include <unordered_map>

namespace sf {

class FrameService : public QObject {
  Q_OBJECT

public:
  struct Options {
    qint64 cacheBytes = 512ll << 20;
    int workerThreads = 3;     // concurrent decoders; hardware decoders are further capped below
    int readAhead = 12;        // frames to keep decoded ahead of (or, in reverse, behind) the playhead (GPU frames: fewer, see gpuFramesPerAsset)
    int scrubKeepBehind = 8;   // frames before a scrub target kept from the GOP walk (cheap back-stepping)
    int maxHardwareDecoders = 4; // GPU surface pools are big (4K NV12 x ~20); beyond this, software
    HwMode hw = HwMode::Auto;
    // Zero-copy: hardware frames stay on the GPU as VideoFrame::gpu (no `image`). Only for a consumer
    // that renders on the shared D3D11 device (the compositor); everything else wants RGBA images.
    bool gpuFrames = false;
    int gpuFramesPerAsset = 6; // cache keeps this many GPU frames per asset, then demotes/drops the oldest
  };

  explicit FrameService(const Options& options, QObject* parent = nullptr);
  FrameService() : FrameService(Options{}) {}
  // Cancels everything in flight and waits for the workers.
  ~FrameService() override;

  // Thread-safe: every public member may be called from any thread.

  // Starts opening in the background (packet scan + decoder setup) and returns at once.
  AssetId openAsset(const QString& path);
  void closeAsset(AssetId asset);
  std::optional<VideoStreamInfo> info(AssetId asset) const; // after assetOpened
  bool failed(AssetId asset) const;                          // opening failed (or the id is unknown)
  QString decoderName(AssetId asset) const;
  // The frame on screen at `sec` seconds from the container start (see VideoDecoder::indexAtTime).
  // nullopt until the asset has opened.
  std::optional<qint64> indexAtTime(AssetId asset, double sec) const;

  // Non-blocking: the frame if it is already decoded.
  VideoFramePtr cached(AssetId asset, qint64 index) const;

  // Scrubbing / seeking: decode this frame soon, replacing any earlier unfinished request. Emits
  // frameReady when done (immediately if already cached).
  void requestFrame(AssetId asset, qint64 index);

  // Playback: where the playhead is and which way it moves (+1, -1, or 0 to stop read-ahead). Cheap to
  // call every tick; only a jump or a direction change abandons in-flight read-ahead.
  void setPlayhead(AssetId asset, qint64 index, int direction);

  // For export and tests: waits for the frame. Never call from the UI thread. Null if it can't be decoded.
  VideoFramePtr frameBlocking(AssetId asset, qint64 index);

  FrameCache& cache() { return cache_; }
  const FrameCache& cache() const { return cache_; }

signals:
  void assetOpened(quint64 asset);
  void assetFailed(quint64 asset, const QString& error);
  void frameReady(quint64 asset, qint64 index);

private:
  struct Asset;
  std::shared_ptr<Asset> find(AssetId id) const;
  void scheduleLocked(const std::shared_ptr<Asset>& a);
  void runAsset(const std::shared_ptr<Asset>& a);

  Options options_;
  FrameCache cache_;
  struct Pool;
  std::unique_ptr<Pool> pool_;
  mutable std::mutex m_;
  std::unordered_map<AssetId, std::shared_ptr<Asset>> assets_;
  AssetId nextId_ = 1;
  std::shared_ptr<std::atomic<int>> hwCount_;
};

} // namespace sf
