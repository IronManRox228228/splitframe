// Compositor / playback benchmark: how fast can a timeline of stacked video layers be decoded and
// composited, with the zero-copy GPU path against the two CPU paths?
//
//   zero-copy    D3D11VA decode, surfaces sampled in place by the shader (no download, no upload)
//   hw+download  D3D11VA decode, GPU->system download, swscale to RGBA, upload (the pre-M2 path)
//   software     libavcodec software decode, swscale, upload
//
// "pipeline" runs the real preview machinery flat out (FrameService read-ahead, Live provider, one
// composited frame at a time, waiting until every layer has its exact picture) and reports the
// sustained rate and per-frame time. "compose" is only the compositor: the pictures are already
// decoded and every layer's upload/copy is forced each frame, so it isolates the GPU-side cost.
//
// Generates its clips with the ffmpeg CLI into a temp dir (removed on exit). Run the release preset
// for real numbers: scripts\dev.cmd cmake --preset release -B build/render-rel, then
//   scripts\dev.cmd build\render-rel\bin\bench_compositor.exe [--quick]

#include "core/timeline_doc.h"
#include "media/gpu_frame.h"
#include "media/probe.h"
#include "render/offscreen.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <vector>

using namespace sf;
using namespace sf::render;

namespace {

struct ClipSpec {
  const char* label;
  int w, h;
  int seconds;
  const char* bitrate;
};

bool makeClip(const QString& ffmpeg, const ClipSpec& c, const QString& path, QString* log) {
  const QString src = QStringLiteral("testsrc2=s=%1x%2:r=30:d=%3,noise=alls=12:allf=t,format=nv12").arg(c.w).arg(c.h).arg(c.seconds);
  for (const QStringList& codec : {QStringList{"-c:v", "h264_nvenc", "-preset", "p4", "-bf", "2", "-b:v", c.bitrate},
                                   QStringList{"-c:v", "libopenh264", "-b:v", c.bitrate}}) {
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(ffmpeg, QStringList{"-v", "error", "-y", "-f", "lavfi", "-i", src} + codec + QStringList{"-g", "30", "-pix_fmt", "yuv420p", path});
    if (p.waitForFinished(300000) && p.exitCode() == 0) return true;
    *log = QString::fromUtf8(p.readAll());
  }
  return false;
}

double percentile(std::vector<double> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size() - 1) + 0.5))];
}

struct BenchScene {
  TimelineDoc doc;
  AssetTable assets;
};

// `layers` stacked video tracks of the same clip (each its own asset, so each its own decoder),
// scaled and offset so no layer hides another.
BenchScene makeScene(const QString& clip, int w, int h, int layers, int frames) {
  BenchScene s;
  s.doc = createEmptyDoc({.id = QStringLiteral("prj_0123456789abcdef"), .name = QStringLiteral("bench"), .fps = 30, .width = w, .height = h});
  s.doc.items.clear();
  QString mainTrack;
  for (const Track& t : s.doc.tracks) {
    if (t.kind == TrackKind::Video) mainTrack = t.id;
  }
  static const double scale[] = {1.0, 0.8, 0.6};
  static const double dx[] = {0, 0.05, -0.08};
  static const double dy[] = {0, 0.04, -0.06};
  for (int i = 0; i < layers; ++i) {
    QString track = mainTrack;
    if (i > 0) {
      Track t;
      t.id = QStringLiteral("trk_layer%1").arg(i);
      t.kind = TrackKind::Video;
      t.name = t.id;
      s.doc.tracks.insert(s.doc.tracks.begin(), t);
      track = t.id;
    }
    const QString asset = QStringLiteral("ast_%1").arg(i);
    s.assets[asset] = {clip, AssetKind::Video};
    ItemInit init;
    init.id = QStringLiteral("itm_%1").arg(i);
    init.trackId = track;
    init.startFrame = 0;
    init.durationFrames = frames;
    init.assetId = asset;
    init.sourceInFrame = 0;
    TransformPatch tp;
    tp.scale = scale[i];
    tp.x = dx[i] * w;
    tp.y = dy[i] * h;
    tp.opacity = i == 0 ? 1.0 : 0.9;
    init.transform = tp;
    s.doc.items.push_back(createItem(ItemType::Video, init));
  }
  return s;
}

struct Path {
  const char* name;
  HwMode hw;
  bool gpuFrames; // keep hardware frames on the GPU
};

struct Result {
  bool ok = false;
  QString note;
  double fps = 0, p50 = 0, p95 = 0, max = 0;
  double composeP50 = 0, composeP95 = 0;
  double renderP50 = 0, renderP95 = 0, renderMax = 0; // time inside renderAndWait during the pipeline run
  QString decoder;
  int gpuLayers = 0, cpuLayers = 0;
};

Result run(OffscreenRenderer& off, const BenchScene& scene, const Path& path, int measured) {
  Result r;
  FrameService::Options o;
  o.hw = path.hw;
  o.gpuFrames = path.gpuFrames;
  o.cacheBytes = 768ll << 20;
  o.workerThreads = 4;
  FrameService svc(o);
  FrameServiceProvider live(svc, scene.assets, FrameServiceProvider::Mode::Live);
  live.openAll();
  off.compositor().setGpuEnabled(path.gpuFrames);

  const int warmup = 20;
  std::vector<double> frameMs, renderMs;
  QElapsedTimer total, one, call;
  RenderStats stats;
  for (int f = 0; f < warmup + measured; ++f) {
    if (f == warmup) total.start();
    one.start();
    // composite until every layer has its exact picture: that is when a player could show the frame
    for (;;) {
      live.prepare(scene.doc, f, 1);
      call.start();
      stats = off.renderQueued(scene.doc, f, live);
      if (f >= warmup) renderMs.push_back(static_cast<double>(call.nsecsElapsed()) / 1e6);
      if (stats.complete() && stats.layers > 0) break;
      if (one.elapsed() > 10000) {
        r.note = QStringLiteral("frame %1 never completed").arg(f);
        return r;
      }
      // give the decoders a moment (and their device lock) instead of recompositing a stale frame flat out
      QElapsedTimer pause;
      pause.start();
      while (pause.nsecsElapsed() < 400000) QThread::yieldCurrentThread();
    }
    if (f >= warmup) frameMs.push_back(static_cast<double>(one.nsecsElapsed()) / 1e6);
    if (f % 8 == 7) off.waitForGpu(); // bound the queue: the GPU is part of what is being measured
    if (qEnvironmentVariableIsSet("SF_BENCH_TRACE") && one.nsecsElapsed() > 20'000'000) std::printf("    slow frame %d: %.1f ms\n", f, static_cast<double>(one.nsecsElapsed()) / 1e6);
  }
  off.waitForGpu();
  const double secs = static_cast<double>(total.nsecsElapsed()) / 1e9;
  r.fps = measured / secs;
  r.p50 = percentile(frameMs, 0.5);
  r.p95 = percentile(frameMs, 0.95);
  r.max = *std::max_element(frameMs.begin(), frameMs.end());
  r.renderP50 = percentile(renderMs, 0.5);
  r.renderP95 = percentile(renderMs, 0.95);
  r.renderMax = *std::max_element(renderMs.begin(), renderMs.end());
  r.decoder = live.decoderSummary();
  r.gpuLayers = stats.gpuLayers;
  r.cpuLayers = stats.cpuLayers;

  // compose only: same frame again and again, with every layer's upload / copy forced
  FrameServiceProvider blocking(svc, scene.assets, FrameServiceProvider::Mode::Blocking);
  const Frame still = warmup + measured / 2;
  off.renderAndWait(scene.doc, still, blocking); // fetch the pictures
  std::vector<double> composeMs;
  for (int i = 0; i < 120; ++i) {
    off.compositor().invalidate();
    one.start();
    off.renderAndWait(scene.doc, still, blocking);
    composeMs.push_back(static_cast<double>(one.nsecsElapsed()) / 1e6);
  }
  r.composeP50 = percentile(composeMs, 0.5);
  r.composeP95 = percentile(composeMs, 0.95);
  r.ok = true;
  return r;
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  QString ffmpeg = qEnvironmentVariable("SF_FFMPEG_EXE");
  if (ffmpeg.isEmpty()) ffmpeg = QStringLiteral(SF_FFMPEG_EXE_DEFAULT);
  const bool quick = app.arguments().contains(QStringLiteral("--quick"));
  const bool detail = app.arguments().contains(QStringLiteral("--detail")); // per-render timing inside the pipeline run
  QString only; // --only <text>: just the rows whose "clip layers path" contains it
  if (const int i = static_cast<int>(app.arguments().indexOf(QStringLiteral("--only"))); i >= 0 && i + 1 < app.arguments().size()) only = app.arguments().at(i + 1);

  QString err;
  auto off = OffscreenRenderer::create(&err);
  if (!off) {
    std::printf("no D3D11 QRhi: %s\n", qPrintable(err));
    return 2;
  }
  std::printf("FFmpeg %s, %s build, adapter \"%s\", QRhi D3D11 %s\n\n", qPrintable(ffmpegVersion()),
#ifdef NDEBUG
              "optimised",
#else
              "DEBUG (numbers understate the app code; use build/render-rel for real figures)",
#endif
              qPrintable(sharedD3D11Device() ? sharedD3D11Device()->adapterName : QStringLiteral("?")),
              off->compositor().gpuCapable() ? "on the shared decode device" : "NOT on the decode device (zero-copy unavailable)");

  const std::vector<ClipSpec> clips = {{"1080p30", 1920, 1080, 8, "16M"}, {"4K30", 3840, 2160, 8, "50M"}};
  const std::vector<Path> paths = {{"zero-copy", HwMode::Required, true}, {"hw+download", HwMode::Required, false}, {"software", HwMode::Off, false}};
  const int measured = quick ? 90 : 150;

  QTemporaryDir dir;
  if (!dir.isValid()) return 2;
  std::printf("%-8s %-7s %-12s %9s %10s %10s %10s | %11s %11s | %-24s\n", "clip", "layers", "path", "fps", "frame p50", "frame p95", "frame max",
              "compose p50", "compose p95", "decoder");
  for (const ClipSpec& c : clips) {
    const QString clip = dir.filePath(QStringLiteral("%1.mp4").arg(c.label));
    QString log;
    if (!makeClip(ffmpeg, c, clip, &log)) {
      std::printf("%-8s cannot generate clip: %s\n", c.label, qPrintable(log.left(300)));
      continue;
    }
    for (const int layers : {1, 3}) {
      const BenchScene scene = makeScene(clip, c.w, c.h, layers, 30 * c.seconds);
      for (const Path& p : paths) {
        if (!only.isEmpty() && !QStringLiteral("%1 %2 %3").arg(QLatin1String(c.label)).arg(layers).arg(QLatin1String(p.name)).contains(only)) continue;
        const Result r = run(*off, scene, p, measured);
        if (!r.ok) {
          std::printf("%-8s %-7d %-12s unavailable: %s\n", c.label, layers, p.name, qPrintable(r.note));
          continue;
        }
        std::printf("%-8s %-7d %-12s %9.1f %7.1f ms %7.1f ms %7.1f ms | %8.2f ms %8.2f ms | %s [gpu %d / cpu %d layers]\n", c.label, layers, p.name, r.fps,
                    r.p50, r.p95, r.max, r.composeP50, r.composeP95, qPrintable(r.decoder), r.gpuLayers, r.cpuLayers);
        if (detail) std::printf("    inside renderAndWait during the run: p50 %.1f ms, p95 %.1f ms, max %.1f ms\n", r.renderP50, r.renderP95, r.renderMax);
        std::fflush(stdout);
      }
    }
  }
  return 0;
}
