#include "index/video_analyzer.h"

#include "media/probe.h"

#include <QDir>
#include <QFileInfo>
#include <QUuid>
#include <cmath>
#include <numeric>

namespace sf::index {

VideoAnalyzer::VideoAnalyzer() = default;
VideoAnalyzer::~VideoAnalyzer() = default;

double VideoAnalyzer::calculateSharpness(const QImage& image) {
  if (image.isNull() || image.width() < 3 || image.height() < 3) return 0.0;

  // Subsample large frames for high-throughput focus evaluation
  const QImage small = (image.width() > 640)
    ? image.scaledToWidth(640, Qt::FastTransformation)
    : image;
  const QImage gray = (small.format() == QImage::Format_Grayscale8)
    ? small
    : small.convertToFormat(QImage::Format_Grayscale8);

  const int w = gray.width();
  const int h = gray.height();
  const int bytesPerLine = static_cast<int>(gray.bytesPerLine());
  const uchar* bits = gray.bits();

  double sum = 0.0;
  double sumSq = 0.0;
  qint64 count = 0;

  for (int y = 1; y < h - 1; ++y) {
    const uchar* prevRow = bits + (y - 1) * bytesPerLine;
    const uchar* currRow = bits + y * bytesPerLine;
    const uchar* nextRow = bits + (y + 1) * bytesPerLine;

    for (int x = 1; x < w - 1; ++x) {
      // 2D discrete Laplacian filter
      const int laplacian = 4 * currRow[x] - currRow[x - 1] - currRow[x + 1] - prevRow[x] - nextRow[x];
      sum += laplacian;
      sumSq += (laplacian * laplacian);
      ++count;
    }
  }

  if (count == 0) return 0.0;
  const double mean = sum / count;
  const double variance = (sumSq / count) - (mean * mean);
  return std::max(0.0, variance);
}

void VideoAnalyzer::calculateLuma(const QImage& image, double* meanLuma, int* clippedPct) {
  if (image.isNull() || !meanLuma || !clippedPct) {
    if (meanLuma) *meanLuma = 0.0;
    if (clippedPct) *clippedPct = 0;
    return;
  }

  const QImage gray = (image.format() == QImage::Format_Grayscale8)
    ? image
    : image.convertToFormat(QImage::Format_Grayscale8);

  const int w = gray.width();
  const int h = gray.height();
  const int bytesPerLine = static_cast<int>(gray.bytesPerLine());
  const uchar* bits = gray.bits();

  qint64 totalLuma = 0;
  qint64 clippedCount = 0;
  const qint64 totalPixels = static_cast<qint64>(w) * h;

  for (int y = 0; y < h; ++y) {
    const uchar* row = bits + y * bytesPerLine;
    for (int x = 0; x < w; ++x) {
      const uchar val = row[x];
      totalLuma += val;
      if (val >= 245 || val <= 10) {
        ++clippedCount;
      }
    }
  }

  *meanLuma = (totalPixels > 0) ? (static_cast<double>(totalLuma) / totalPixels) : 0.0;
  *clippedPct = (totalPixels > 0) ? static_cast<int>((clippedCount * 100) / totalPixels) : 0;
}

double VideoAnalyzer::calculateMotion(const QImage& prev, const QImage& curr) {
  if (prev.isNull() || curr.isNull() || prev.size() != curr.size()) return 0.0;

  const QImage smallPrev = (prev.width() > 320) ? prev.scaledToWidth(320, Qt::FastTransformation) : prev;
  const QImage smallCurr = (curr.width() > 320) ? curr.scaledToWidth(320, Qt::FastTransformation) : curr;

  const QImage gPrev = smallPrev.convertToFormat(QImage::Format_Grayscale8);
  const QImage gCurr = smallCurr.convertToFormat(QImage::Format_Grayscale8);

  const int w = gPrev.width();
  const int h = gPrev.height();
  const int stridePrev = static_cast<int>(gPrev.bytesPerLine());
  const int strideCurr = static_cast<int>(gCurr.bytesPerLine());
  const uchar* bitsPrev = gPrev.bits();
  const uchar* bitsCurr = gCurr.bits();

  double diffSum = 0.0;
  const qint64 totalPixels = static_cast<qint64>(w) * h;

  for (int y = 0; y < h; ++y) {
    const uchar* rowP = bitsPrev + y * stridePrev;
    const uchar* rowC = bitsCurr + y * strideCurr;
    for (int x = 0; x < w; ++x) {
      diffSum += std::abs(static_cast<int>(rowC[x]) - static_cast<int>(rowP[x]));
    }
  }

  return (totalPixels > 0) ? (diffSum / totalPixels) : 0.0;
}

bool VideoAnalyzer::isBlackFrame(double meanLuma, double threshold) {
  return meanLuma < threshold;
}

bool VideoAnalyzer::isFreezeFrame(double motion, double threshold) {
  return motion < threshold;
}

std::vector<SceneRecord> VideoAnalyzer::analyzeVideo(const QString& filePath,
                                                     const QString& assetId,
                                                     const AnalysisOptions& opts,
                                                     std::function<void(double)> progressCb) {
  std::vector<SceneRecord> scenes;

  QString err;
  const auto info = sf::probeMedia(filePath, &err);
  if (!info || !info->hasVideo || info->durationMs <= 0) {
    return scenes;
  }

  const double fps = (info->fps > 0.0) ? info->fps : 25.0;
  const qint64 totalMs = info->durationMs;
  const qint64 totalFrames = (info->frameCount > 0)
    ? info->frameCount
    : static_cast<qint64>(std::round((totalMs / 1000.0) * fps));

  // Sample interval: e.g. 100ms for short clips, or 250ms for longer files
  const qint64 stepMs = (totalMs <= 5000) ? 100 : (totalMs <= 30000 ? 200 : 350);

  struct SamplePoint {
    qint64 timeMs = 0;
    qint64 frame = 0;
    double sharpness = 0.0;
    double luma = 0.0;
    int clippedPct = 0;
    double motion = 0.0;
    QImage frameImg;
  };

  std::vector<SamplePoint> samples;
  QImage prevFrame;

  for (qint64 t = 0; t < totalMs; t += stepMs) {
    QString decErr;
    QImage frame = sf::decodeFrame(filePath, t, &decErr);
    if (!frame.isNull()) {
      SamplePoint pt;
      pt.timeMs = t;
      pt.frame = static_cast<qint64>(std::round((t / 1000.0) * fps));
      pt.sharpness = calculateSharpness(frame);
      calculateLuma(frame, &pt.luma, &pt.clippedPct);
      pt.motion = prevFrame.isNull() ? 0.0 : calculateMotion(prevFrame, frame);
      pt.frameImg = frame;

      samples.push_back(pt);
      prevFrame = frame;
    }

    if (progressCb && totalMs > 0) {
      progressCb(static_cast<double>(t) / totalMs * 0.7); // 70% decode/metric pass
    }
  }

  if (samples.empty()) return scenes;

  // Cut threshold (motion spike between samples)
  // Base threshold is 20.0 modified by opts.sceneThreshold (0.35 -> ~22.0)
  const double cutThreshold = 15.0 + (opts.sceneThreshold * 20.0);

  size_t sceneStartIdx = 0;

  auto finalizeScene = [&](size_t startIdx, size_t endIdx) {
    if (startIdx >= samples.size()) return;
    if (endIdx >= samples.size()) endIdx = samples.size() - 1;

    SceneRecord rec;
    rec.id = QStringLiteral("sc_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
    rec.assetId = assetId;
    rec.startFrame = samples[startIdx].frame;
    rec.endFrame = (endIdx == samples.size() - 1) ? totalFrames : samples[endIdx].frame;
    rec.startSec = samples[startIdx].timeMs / 1000.0;
    rec.endSec = (endIdx == samples.size() - 1) ? (totalMs / 1000.0) : (samples[endIdx].timeMs / 1000.0);

    // Aggregate metrics over the scene interval
    double maxSharpness = 0.0;
    double sumLuma = 0.0;
    double sumMotion = 0.0;
    int maxClipped = 0;
    size_t bestKeyframeIdx = startIdx;
    size_t freezeCount = 0;
    size_t blackCount = 0;
    size_t spanCount = endIdx - startIdx + 1;

    for (size_t i = startIdx; i <= endIdx; ++i) {
      const auto& sp = samples[i];
      sumLuma += sp.luma;
      sumMotion += sp.motion;
      maxClipped = std::max(maxClipped, sp.clippedPct);
      if (sp.sharpness > maxSharpness) {
        maxSharpness = sp.sharpness;
        bestKeyframeIdx = i;
      }
      if (isFreezeFrame(sp.motion, 0.4)) ++freezeCount;
      if (isBlackFrame(sp.luma, 15.0)) ++blackCount;
    }

    rec.sharpness = maxSharpness;
    rec.lumaAvg = spanCount > 0 ? (sumLuma / spanCount) : 0.0;
    rec.lumaClippedPct = maxClipped;
    rec.motionScore = spanCount > 0 ? (sumMotion / spanCount) : 0.0;
    rec.isFreeze = (spanCount >= 3) && (freezeCount == spanCount);
    rec.isBlack = (spanCount >= 2) && (blackCount == spanCount);

    // Save thumbnail
    if (!opts.cacheDir.isEmpty() && !samples[bestKeyframeIdx].frameImg.isNull()) {
      QDir().mkpath(opts.cacheDir);
      const QString thumbFile = QStringLiteral("%1/%2_%3.jpg")
        .arg(opts.cacheDir, assetId, rec.id);
      samples[bestKeyframeIdx].frameImg.scaledToWidth(320, Qt::SmoothTransformation)
        .save(thumbFile, "JPG", 85);
      rec.thumbnailPath = thumbFile;
    }

    scenes.push_back(rec);
  };

  for (size_t i = 1; i < samples.size(); ++i) {
    if (samples[i].motion > cutThreshold) {
      // Scene cut detected before sample i
      finalizeScene(sceneStartIdx, i - 1);
      sceneStartIdx = i;
    }
  }

  // Final scene spanning to the end of the clip
  finalizeScene(sceneStartIdx, samples.size() - 1);

  if (progressCb) progressCb(1.0);
  return scenes;
}

} // namespace sf::index
