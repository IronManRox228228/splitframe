#pragma once

#include "index/types.h"

#include <QString>
#include <functional>
#include <vector>

namespace sf::index {

struct AudioAnalysisResult {
  std::vector<SpeechSpan> spans;
  std::vector<qint64> beatFrames;
  double integratedLufs = -100.0;
  double truePeakDb = -100.0;
  double noiseFloorDb = -100.0;
};

class AudioAnalyzer {
public:
  AudioAnalyzer();
  ~AudioAnalyzer();

  // Low-level metrics
  static double calculateRmsDb(const float* interleaved, qint64 frameCount, int channels = 2);

  // Full asset audio analysis (silence/speech segmentation, LUFS loudness, transient beats)
  AudioAnalysisResult analyzeAudio(const QString& filePath,
                                   const QString& assetId,
                                   double videoFps = 25.0,
                                   const AnalysisOptions& opts = {},
                                   std::function<void(double)> progressCb = nullptr);
};

} // namespace sf::index
