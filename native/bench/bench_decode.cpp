// Decode throughput and seek latency, hardware (D3D11VA) vs software, on 1080p and 4K H.264.
// Generates its clips with the ffmpeg CLI (NVENC, a few seconds each) into a temp dir that is removed
// on exit. Frame content is a moving test pattern with noise so the bitstream isn't trivially small.
//
// "decode+convert" includes everything a consumer of VideoFrame pays: codec work, the GPU->system
// download for hardware frames, and YUV->RGBA conversion. "convert" is reported separately so the
// codec part can be read off.

#include "media/probe.h"
#include "media/video_decoder.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QRandomGenerator>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace sf;

namespace {

struct ClipSpec {
  const char* label;
  int w, h;
  int fps;
  int seconds;
  const char* bitrate;
};

bool makeClip(const QString& ffmpeg, const ClipSpec& c, const QString& path, QString* log) {
  const QString src = QStringLiteral("testsrc2=s=%1x%2:r=%3:d=%4,noise=alls=12:allf=t,format=nv12").arg(c.w).arg(c.h).arg(c.fps).arg(c.seconds);
  QProcess p;
  p.setProcessChannelMode(QProcess::MergedChannels);
  p.start(ffmpeg, {"-v", "error", "-y", "-f", "lavfi", "-i", src, "-c:v", "h264_nvenc", "-preset", "p4", "-g", QString::number(c.fps), "-bf", "2",
                   "-b:v", c.bitrate, "-pix_fmt", "yuv420p", path});
  if (!p.waitForFinished(300000) || p.exitCode() != 0) {
    *log = QString::fromUtf8(p.readAll());
    return false;
  }
  return true;
}

double percentile(std::vector<double> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const size_t i = std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size() - 1) + 0.5));
  return v[i];
}

struct Result {
  double openMs = 0;
  double seqFps = 0;
  double convertShare = 0; // fraction of the sequential time spent converting
  double seekP50 = 0, seekP95 = 0, seekMax = 0;
  QString decoder;
  bool ok = false;
};

Result run(const QString& path, HwMode mode, int seekSamples) {
  Result r;
  QElapsedTimer t;
  t.start();
  QString err;
  auto dec = VideoDecoder::open(path, {mode, 0}, &err);
  r.openMs = t.nsecsElapsed() / 1e6;
  if (!dec) {
    r.decoder = err;
    return r;
  }

  // warm-up: first frame pays one-off costs (device creation, thread start, JIT-ish caches)
  dec->frameAt(0);

  // sequential playback over the whole clip
  const qint64 n = dec->frameCount();
  dec->frameAt(0);
  const auto before = dec->stats();
  t.restart();
  qint64 got = 0;
  for (qint64 i = 1; i < n; ++i) {
    if (!dec->next()) break;
    ++got;
  }
  const double secs = t.nsecsElapsed() / 1e9;
  r.seqFps = got / secs;
  r.convertShare = (dec->stats().convertSeconds - before.convertSeconds) / secs;
  r.decoder = dec->decoderName();

  // random seeks: latency to a displayable frame at a position the decoder isn't near
  QRandomGenerator rng(42);
  std::vector<double> lat;
  for (int i = 0; i < seekSamples; ++i) {
    const qint64 target = rng.bounded(static_cast<quint32>(n));
    t.restart();
    if (!dec->frameAt(target)) continue;
    lat.push_back(t.nsecsElapsed() / 1e6);
  }
  r.seekP50 = percentile(lat, 0.5);
  r.seekP95 = percentile(lat, 0.95);
  r.seekMax = lat.empty() ? 0 : *std::max_element(lat.begin(), lat.end());
  r.ok = true;
  return r;
}

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QString ffmpeg = qEnvironmentVariable("SF_FFMPEG_EXE");
  if (ffmpeg.isEmpty()) ffmpeg = QStringLiteral(SF_FFMPEG_EXE_DEFAULT);
  const bool quick = app.arguments().contains(QStringLiteral("--quick"));

  const std::vector<ClipSpec> clips = quick ? std::vector<ClipSpec>{{"1080p30 H.264", 1920, 1080, 30, 4, "16M"}, {"4K30 H.264", 3840, 2160, 30, 3, "50M"}}
                                            : std::vector<ClipSpec>{{"1080p30 H.264", 1920, 1080, 30, 10, "16M"}, {"4K30 H.264", 3840, 2160, 30, 6, "50M"}};
  QTemporaryDir dir;
  if (!dir.isValid()) return 2;

  std::printf("FFmpeg %s, %s build\n\n", qPrintable(ffmpegVersion()),
#ifdef NDEBUG
              "optimised"
#else
              "DEBUG (numbers understate the app code; build the release preset for real figures)"
#endif
  );
  std::printf("%-15s %-6s %-26s %9s %10s %9s %10s %10s %10s\n", "clip", "mode", "decoder", "open ms", "seq fps", "convert%", "seek p50", "seek p95",
              "seek max");

  for (const ClipSpec& c : clips) {
    const QString path = dir.filePath(QStringLiteral("%1x%2.mp4").arg(c.w).arg(c.h));
    QString log;
    if (!makeClip(ffmpeg, c, path, &log)) {
      std::printf("%-15s cannot generate clip: %s\n", c.label, qPrintable(log.left(300)));
      continue;
    }
    const auto info = probeMedia(path);
    for (const HwMode mode : {HwMode::Required, HwMode::Off}) {
      const Result r = run(path, mode, quick ? 20 : 60);
      const char* name = mode == HwMode::Off ? "sw" : "hw";
      if (!r.ok) {
        std::printf("%-15s %-6s unavailable: %s\n", c.label, name, qPrintable(r.decoder));
        continue;
      }
      std::printf("%-15s %-6s %-26s %9.0f %10.1f %8.0f%% %8.1f ms %8.1f ms %8.1f ms\n", c.label, name, qPrintable(r.decoder), r.openMs, r.seqFps,
                  r.convertShare * 100, r.seekP50, r.seekP95, r.seekMax);
    }
    if (info) std::printf("    (%d frames, GOP %d, %s)\n", int(info->frameCount), c.fps, qPrintable(info->pixelFormat));
  }
  return 0;
}
