#pragma once

#include "index/types.h"

#include <QImage>
#include <QString>
#include <functional>
#include <vector>

namespace sf::index {

class VideoAnalyzer {
public:
  VideoAnalyzer();
  ~VideoAnalyzer();

  // Low-level perceptual and quality metrics
  static double calculateSharpness(const QImage& image);
  static void calculateLuma(const QImage& image, double* meanLuma, int* clippedPct);
  static double calculateMotion(const QImage& prev, const QImage& curr);
  static bool isBlackFrame(double meanLuma, double threshold = 15.0);
  static bool isFreezeFrame(double motion, double threshold = 0.5);

  // Full asset video analysis (scene cuts, sharpness, luma, motion, thumbnails)
  std::vector<SceneRecord> analyzeVideo(const QString& filePath,
                                       const QString& assetId,
                                       const AnalysisOptions& opts = {},
                                       std::function<void(double)> progressCb = nullptr);
};

} // namespace sf::index
