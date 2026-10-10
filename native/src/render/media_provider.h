#pragma once

// What the compositor draws media from. The compositor asks for "the picture of this item at this
// source frame" and gets pixels (or a GPU surface) back; where they come from is the provider's
// business: FrameService with its read-ahead for preview, blocking decodes for export and tests.

#include "core/schema.h"
#include "media/frame_service.h"

#include <QImage>
#include <QSize>
#include <QString>

#include <atomic>
#include <map>
#include <optional>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace sf::render {

struct AssetRef {
  QString path;
  AssetKind kind = AssetKind::Video;
  // A lighter stand-in for `path` (export/proxy.h) that Live (preview) providers decode instead when
  // proxies are on. Blocking providers (export, tests) never look at it: export always uses the original.
  QString proxyPath;
};
using AssetTable = std::map<QString, AssetRef>; // asset id -> file

struct Visual {
  enum class State {
    Missing, // no such asset or the file can't be read: the compositor draws the "media unavailable" card
    Pending, // exists, no picture yet: draws nothing (or `video` when a stale frame is held for it)
    Ready,
  };
  State state = State::Missing;
  VideoFramePtr video;                 // video items: a frame with `gpu` or `image`
  std::shared_ptr<const QImage> image; // image items
  QSize size;                          // size as displayed (after rotation); valid unless Missing
  int rotation = 0;                    // degrees clockwise to turn `video` upright
  bool exact = true;                   // false: `video` is an older frame standing in for the right one
};

class MediaProvider {
public:
  virtual ~MediaProvider() = default;
  // `sourceFrame` is in project-fps units (sourceFrameAt), as in the TS reference where it becomes
  // sourceFrame / projectFps seconds.
  virtual Visual visual(const Item& item, Frame sourceFrame, double projectFps) = 0;
};

// Decoded still images, loaded once with EXIF orientation applied. Thread-safe.
class ImageStore {
public:
  std::shared_ptr<const QImage> load(const QString& path);

private:
  std::mutex m_;
  std::unordered_map<QString, std::shared_ptr<const QImage>> images_;
};

// Media from a FrameService. `Live` never blocks (a miss requests the frame and shows the previous
// one meanwhile); `Blocking` waits for exact frames, for export and tests.
class FrameServiceProvider final : public MediaProvider {
public:
  enum class Mode { Live, Blocking };
  FrameServiceProvider(FrameService& service, AssetTable assets, Mode mode = Mode::Live);

  Visual visual(const Item& item, Frame sourceFrame, double projectFps) override;

  // Live mode: call once per presented frame before compositing. Points the decoders' read-ahead at
  // what is about to be needed. `direction` is the playhead's: +1/-1 playing, 0 paused or scrubbing.
  void prepare(const TimelineDoc& doc, Frame frame, int direction);

  // Starts opening every video asset (the packet scan is the slow part: do it before the first play)
  void openAll();
  AssetTable assets() const;
  // Preview only: decode AssetRef::proxyPath where there is one (Live mode; ignored in Blocking mode).
  // Open decoders are closed and reopen on the file that now applies. Thread-safe.
  void setUseProxies(bool on);
  bool useProxies() const { return useProxies_; }
  // Replaces the asset table (media imported, removed or relinked while the project is open). Thread-safe;
  // decoders of files that changed are closed and open again on demand.
  void setAssets(AssetTable assets);
  // "h264 (d3d11va), hevc (software)": what the opened video assets decode with
  QString decoderSummary();

private:
  AssetId assetFor(const QString& assetId); // opens on first use; 0 = unknown/failed
  std::optional<qint64> sourceIndex(AssetId id, const VideoStreamInfo& info, Frame sourceFrame, double fps);

  std::optional<AssetRef> assetRef(const QString& assetId) const;
  QString effectivePath(const AssetRef& ref) const; // the proxy when it applies, else the original

  FrameService& service_;
  mutable std::mutex am_; // guards assets_ (the UI thread replaces it while the render thread reads)
  AssetTable assets_;
  Mode mode_;
  std::atomic<bool> useProxies_{false};
  ImageStore images_;
  std::mutex m_;
  std::unordered_map<QString, AssetId> ids_;
  std::unordered_map<AssetId, VideoFramePtr> lastShown_;
};

} // namespace sf::render
