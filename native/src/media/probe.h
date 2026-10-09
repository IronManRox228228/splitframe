#pragma once

#include <QImage>
#include <QString>
#include <optional>

namespace sf {

struct MediaInfo {
  qint64 durationMs = 0;
  int width = 0;
  int height = 0;
  double fps = 0;
  bool hasVideo = false;
  bool hasAudio = false;
  QString videoCodec;
};

QString ffmpegVersion();

// Reads container and stream headers. nullopt (with *error set) when the file can't be opened.
std::optional<MediaInfo> probeMedia(const QString& path, QString* error = nullptr);

// Decodes the first video frame at or after atMs into an RGBA image. Null image on failure.
QImage decodeFrame(const QString& path, qint64 atMs = 0, QString* error = nullptr);

} // namespace sf
