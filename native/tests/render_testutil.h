#pragma once

// Helpers for the compositor tests: tiny documents, generated clips, pixel checks.

#include "core/timeline_doc.h"
#include "media_testutil.h"
#include "render/media_provider.h"

#include <QImage>
#include <QTemporaryDir>
#include <QTest>

#include <stdexcept>

namespace sf::test {

struct Rgb {
  int r, g, b;
};

inline bool closeTo(QRgb px, Rgb want, int tol) {
  return std::abs(qRed(px) - want.r) <= tol && std::abs(qGreen(px) - want.g) <= tol && std::abs(qBlue(px) - want.b) <= tol;
}

inline QString describe(QRgb px) { return QStringLiteral("(%1,%2,%3)").arg(qRed(px)).arg(qGreen(px)).arg(qBlue(px)); }

#define SF_VERIFY_PIXEL(img, x, y, want, tol)                                                                                                    \
  QVERIFY2(::sf::test::closeTo((img).pixel((x), (y)), want, tol),                                                                                   \
           qPrintable(QStringLiteral("pixel (%1,%2) is %3, wanted %4").arg(x).arg(y).arg(::sf::test::describe((img).pixel((x), (y)))).arg(          \
               ::sf::test::describe(qRgb((want).r, (want).g, (want).b)))))

// Mean absolute difference per channel between two same-size images (alpha ignored).
inline double meanAbsDiff(const QImage& a, const QImage& b, int step = 1) {
  if (a.size() != b.size()) return 1e9;
  const QImage ca = a.convertToFormat(QImage::Format_RGBA8888);
  const QImage cb = b.convertToFormat(QImage::Format_RGBA8888);
  qint64 sum = 0, n = 0;
  for (int y = 0; y < ca.height(); y += step) {
    const uchar* pa = ca.constScanLine(y);
    const uchar* pb = cb.constScanLine(y);
    for (int x = 0; x < ca.width(); x += step) {
      for (int c = 0; c < 3; ++c) sum += std::abs(pa[x * 4 + c] - pb[x * 4 + c]);
      n += 3;
    }
  }
  return n ? double(sum) / double(n) : 0;
}

inline int maxAbsDiff(const QImage& a, const QImage& b) {
  if (a.size() != b.size()) return 255;
  const QImage ca = a.convertToFormat(QImage::Format_RGBA8888);
  const QImage cb = b.convertToFormat(QImage::Format_RGBA8888);
  int worst = 0;
  for (int y = 0; y < ca.height(); ++y) {
    const uchar* pa = ca.constScanLine(y);
    const uchar* pb = cb.constScanLine(y);
    for (int x = 0; x < ca.width() * 4; ++x) {
      if (x % 4 != 3) worst = std::max(worst, std::abs(pa[x] - pb[x]));
    }
  }
  return worst;
}

inline TimelineDoc newDoc(int w, int h, int fps = 25, const QString& background = QStringLiteral("#000000")) {
  TimelineDoc doc = createEmptyDoc({.id = QStringLiteral("prj_0123456789abcdef"), .name = QStringLiteral("t"), .fps = fps, .width = w, .height = h});
  doc.project.styleConfig.backgroundColor = background;
  return doc;
}

// A video track above all others (tracks are stored top-first).
inline QString addTopTrack(TimelineDoc& doc, const QString& id, TrackKind kind = TrackKind::Video) {
  Track t;
  t.id = id;
  t.kind = kind;
  t.name = id;
  doc.tracks.insert(doc.tracks.begin(), t);
  return id;
}

inline QString mainTrack(const TimelineDoc& doc) {
  for (const Track& t : doc.tracks) {
    if (t.kind == TrackKind::Video) return t.id;
  }
  return {};
}

inline Item& addMedia(TimelineDoc& doc, ItemType type, const QString& track, const QString& id, const QString& asset, Frame start, Frame duration,
                      const std::function<void(ItemInit&)>& tweak = {}) {
  ItemInit init;
  init.id = id;
  init.trackId = track;
  init.startFrame = start;
  init.durationFrames = duration;
  init.assetId = asset;
  if (type == ItemType::Video) init.sourceInFrame = 0;
  if (tweak) tweak(init);
  doc.items.push_back(createItem(type, init));
  return doc.items.back();
}

inline Item& findItem(TimelineDoc& doc, const QString& id) {
  for (Item& i : doc.items) {
    if (i.id == id) return i;
  }
  throw std::runtime_error("no item " + id.toStdString());
}

// Encodes a clip from an lavfi graph. `codec` as for makeIndexClip.
inline bool makeClip(const QString& out, const QString& lavfi, const QStringList& codec, const QStringList& before = {}, QString* log = nullptr) {
  QStringList args{QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), lavfi};
  args += before;
  args += codec;
  args << out;
  return runFfmpeg(args, log);
}

inline QStringList h264() { return {QStringLiteral("-c:v"), QStringLiteral("libopenh264"), QStringLiteral("-g"), QStringLiteral("30")}; }

// Four coloured quadrants: red top-left, green top-right, blue bottom-left, yellow bottom-right.
inline QString quadrantsGraph(int w, int h, int fps = 25, double seconds = 2) {
  return QStringLiteral("color=c=0xff0000:s=%1x%2:r=%3:d=%4,"
                        "drawbox=x=%5:y=0:w=%5:h=%6:color=0x00ff00:t=fill,"
                        "drawbox=x=0:y=%6:w=%5:h=%6:color=0x0000ff:t=fill,"
                        "drawbox=x=%5:y=%6:w=%5:h=%6:color=0xffff00:t=fill,format=yuv420p")
      .arg(w).arg(h).arg(fps).arg(seconds).arg(w / 2).arg(h / 2);
}

inline QString solidGraph(const QString& color, int w, int h, int fps = 25, double seconds = 2) {
  return QStringLiteral("color=c=%1:s=%2x%3:r=%4:d=%5,format=yuv420p").arg(color).arg(w).arg(h).arg(fps).arg(seconds);
}

} // namespace sf::test
