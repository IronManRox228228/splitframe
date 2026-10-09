#pragma once

#include <QImage>
#include <QString>
#include <optional>

namespace sf {

// Colour description as tagged in the stream. Untagged fields are "unspecified": decoders then fall
// back to BT.709 (>= 720p) or BT.601, as players do.
struct ColorInfo {
  QString primaries = QStringLiteral("unspecified"); // bt709, bt2020, smpte432 ...
  QString transfer = QStringLiteral("unspecified");  // bt709, smpte2084 (PQ), arib-std-b67 (HLG) ...
  QString matrix = QStringLiteral("unspecified");    // bt709, bt470bg, bt2020nc ...
  QString range = QStringLiteral("unspecified");     // tv (limited), pc (full)

  bool isHdr() const { return transfer == QLatin1String("smpte2084") || transfer == QLatin1String("arib-std-b67"); }
};

struct MediaInfo {
  qint64 durationMs = 0;
  int width = 0; // coded size, before rotation
  int height = 0;
  double fps = 0; // nominal rate (container's guess)
  bool hasVideo = false;
  bool hasAudio = false;
  QString videoCodec;

  // Display orientation. Phones store landscape pixels plus a display matrix.
  int rotation = 0;      // degrees clockwise to turn the decoded frame upright: 0, 90, 180, 270
  int displayWidth = 0;  // size after rotation
  int displayHeight = 0;

  QString pixelFormat; // "yuv420p", "yuv420p10le" ...
  int bitDepth = 0;
  ColorInfo color;

  // Frame timing, measured from the packet timestamps (all of them up to vfrScanLimit packets).
  bool vfr = false;          // frame gaps vary beyond container rounding jitter
  double avgFps = 0;         // measured: frames / span
  double minFps = 0;         // from the longest frame gap
  double maxFps = 0;         // from the shortest frame gap
  qint64 frameCount = 0;     // frames seen by the scan (nb_frames when the scan was cut short)
  bool timingComplete = false; // false: only the first vfrScanLimit packets were inspected

  // Where each stream's first sample sits, in seconds from the container start. A/V sync needs this:
  // mp4 edit lists and MPEG-TS routinely start video 0.04..1.4 s after audio.
  double videoStartSec = 0;
  double audioStartSec = 0;

  int audioSampleRate = 0;
  int audioChannels = 0;
  QString audioCodec;
};

struct ProbeOptions {
  // Packets inspected for the VFR/frame-count measurement. 0 = the whole file (reads all of it).
  // The default covers ~16 s at 60 fps, which is plenty to tell a constant rate from a variable one.
  int vfrScanLimit = 1000;
};

QString ffmpegVersion();

// Reads container and stream headers. nullopt (with *error set) when the file can't be opened.
// Thread-safe (no shared state); call it from a worker, it touches the disk.
std::optional<MediaInfo> probeMedia(const QString& path, QString* error = nullptr, const ProbeOptions& options = {});

// Decodes the first video frame at or after atMs into an RGBA image. Null image on failure.
// Convenience for one-offs; use VideoDecoder for anything repeated.
QImage decodeFrame(const QString& path, qint64 atMs = 0, QString* error = nullptr);

} // namespace sf
