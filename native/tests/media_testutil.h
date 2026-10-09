#pragma once

// Shared helpers for the media tests: generate clips with the ffmpeg CLI and read back what they say.
//
// Test clips carry their own frame number: frame N is painted as 16 black/white blocks across the
// picture, block i = bit i of N (via the `geq` filter, which this LGPL build has; `drawtext` would
// need a font file). Blocks are wide enough to survive lossy codecs, so "decode frame N" can be
// checked by reading the pixels instead of trusting timestamps.

#include <QImage>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTest>

namespace sf::test {

inline QString ffmpegExe() { return qEnvironmentVariable("SF_FFMPEG_EXE"); }

inline bool runFfmpeg(const QStringList& args, QString* log = nullptr, int timeoutMs = 180000) {
  QProcess p;
  p.setProcessChannelMode(QProcess::MergedChannels);
  p.start(ffmpegExe(), QStringList{QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-y")} + args);
  if (!p.waitForFinished(timeoutMs)) {
    p.kill();
    p.waitForFinished();
    if (log) *log = QStringLiteral("ffmpeg timed out");
    return false;
  }
  if (log) *log = QString::fromUtf8(p.readAll());
  return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

// lavfi input description for a frame-number clip.
inline QString indexSource(int w, int h, double fps, double seconds, const QString& pixFmt = QStringLiteral("yuv420p")) {
  const int bw = w / 16;
  return QStringLiteral("color=c=gray:s=%1x%2:r=%3:d=%4,format=yuv420p,"
                        "geq=lum='255*mod(floor(N/pow(2,floor(X/%5))),2)':cb=128:cr=128,format=%6")
      .arg(w).arg(h).arg(fps).arg(seconds).arg(bw).arg(pixFmt);
}

// The frame number painted into an image, or -1 for a null image.
inline int readIndex(const QImage& img) {
  if (img.isNull()) return -1;
  const int bw = img.width() / 16;
  int n = 0;
  for (int i = 0; i < 16; ++i) {
    const QRgb p = img.pixel(i * bw + bw / 2, img.height() / 2);
    if (qGray(p) > 127) n |= 1 << i;
  }
  return n;
}

// Encodes a frame-number clip. `codec` is the encoder arguments (-c:v ..., -g ..., -pix_fmt ...).
inline bool makeIndexClip(const QString& out, const QStringList& codec, int w = 320, int h = 240, double fps = 25,
                          double seconds = 4, const QStringList& beforeOutput = {}, QString* log = nullptr,
                          const QString& pixFmt = QStringLiteral("yuv420p")) {
  QStringList args{QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), indexSource(w, h, fps, seconds, pixFmt)};
  args += beforeOutput;
  args += codec;
  args << out;
  return runFfmpeg(args, log);
}

} // namespace sf::test
