#pragma once

// Timeline -> file.
//
// Pipeline (all on worker threads, the caller only waits or watches progress):
//
//   render thread (this call's thread)            encode thread
//   ------------------------------------          --------------------------------------------------
//   OffscreenRenderer frame i (blocking decode) -> RGB -> YUV (swscale) -> encoder -> muxer
//   readback in the delivery encoding            \  audio: AudioEngine chunk -> [loudness gain] -> encoder -> muxer
//   bounded queue (3 frames) ------------------->
//
// Video: the compositor delivers RGBA8 (8-bit output) or RGB10A2 read back as 16-bit (10-bit output) in the
// project's delivery encoding (sRGB / Rec.709 / PQ / HLG). The RGB -> YUV conversion is done by swscale
// (colour matrix and range taken from the frame tags: BT.709 or BT.2020 non-constant, limited range), which
// also scales when the output size differs. A GPU pass would save the readback's RGBA->YUV CPU cost but the
// swscale route is exact, tag-driven and one code path for every codec / bit depth; the compositor's own
// readback is the real cost either way.
//
// Time: constant frame rate at the project fps, encoder and stream time base 1/fps, frame i has pts i; audio is
// 48 kHz, time base 1/48000, sample-exact against frameToSample (audio/timebase.h), so the duration is exactly
// frames / fps and A/V sync holds to a sample. The output file starts at time zero whatever the range's in point.
//
// The file is written next to its destination as "<name>.sfpart" and renamed when complete, so a cancelled or
// failed export leaves nothing behind and an existing file is only replaced by a finished one.

#include "core/schema.h"
#include "export/export_settings.h"
#include "render/media_provider.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace sf::xport {

struct ExportProgress {
  qint64 frame = 0; // frames finished (encoded and muxed)
  qint64 totalFrames = 0;
  double fps = 0;   // measured since the start
  double etaSec = 0;
  double fraction = 0;
  QString stage; // "rendering", "measuring loudness", "finishing"
};

struct ExportResult {
  bool ok = false;
  bool cancelled = false;
  QString error; // set when !ok && !cancelled
  QString path;
  qint64 videoFrames = 0;
  qint64 audioSamples = 0;
  double seconds = 0;     // wall time
  double averageFps = 0;  // rendered + encoded frames per second
  QString videoEncoder;   // what actually ran (NVENC falls back to software)
  QString audioEncoder;
  double appliedGainDb = 0; // loudness normalisation
  qint64 missingFrames = 0; // frames that drew a "media unavailable" layer
  QStringList warnings;
};

using ProgressFn = std::function<void(const ExportProgress&)>;

// Blocks until done. `assets` maps asset ids to the ORIGINAL files: proxies never reach export (the
// provider runs in Blocking mode, which ignores AssetRef::proxyPath, and this function only reads `path`).
// `cancel` is polled between frames; cancelling removes the partial file. Never throws.
ExportResult runExport(const TimelineDoc& doc, const render::AssetTable& assets, const ExportSettings& settings,
                       const ProgressFn& progress = {}, const std::atomic<bool>* cancel = nullptr);

// The same on its own thread, for the UI. Signals are queued to the object's thread.
class ExportJob : public QObject {
  Q_OBJECT
public:
  ExportJob(TimelineDoc doc, render::AssetTable assets, ExportSettings settings, QObject* parent = nullptr);
  ~ExportJob() override;
  void start();
  void cancel();
  bool running() const { return running_; }

signals:
  void progress(const sf::xport::ExportProgress& p);
  void finished(const sf::xport::ExportResult& r);

private:
  TimelineDoc doc_;
  render::AssetTable assets_;
  ExportSettings settings_;
  std::atomic<bool> cancel_{false};
  std::atomic<bool> running_{false};
  std::thread thread_;
};

} // namespace sf::xport

Q_DECLARE_METATYPE(sf::xport::ExportProgress)
Q_DECLARE_METATYPE(sf::xport::ExportResult)
