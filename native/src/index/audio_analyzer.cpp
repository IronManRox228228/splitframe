#include "index/audio_analyzer.h"

#include "audio/loudness.h"
#include "media/audio_decoder.h"

#include <QUuid>
#include <algorithm>
#include <cmath>

namespace sf::index {

AudioAnalyzer::AudioAnalyzer() = default;
AudioAnalyzer::~AudioAnalyzer() = default;

double AudioAnalyzer::calculateRmsDb(const float* interleaved, qint64 frameCount, int channels) {
  if (!interleaved || frameCount <= 0 || channels <= 0) return -100.0;

  double sumSq = 0.0;
  const qint64 totalSamples = frameCount * channels;
  for (qint64 i = 0; i < totalSamples; ++i) {
    const double val = interleaved[i];
    sumSq += (val * val);
  }

  const double rms = std::sqrt(sumSq / totalSamples);
  if (rms <= 1e-5) return -100.0;
  return 20.0 * std::log10(rms);
}

AudioAnalysisResult AudioAnalyzer::analyzeAudio(const QString& filePath,
                                               const QString& assetId,
                                               double videoFps,
                                               const AnalysisOptions& opts,
                                               std::function<void(double)> progressCb) {
  AudioAnalysisResult result;

  QString err;
  auto decoder = sf::AudioDecoder::open(filePath, &err);
  if (!decoder) {
    return result;
  }

  const auto& info = decoder->info();
  if (info.totalSamples <= 0) {
    return result;
  }

  const double fps = (videoFps > 0.0) ? videoFps : 25.0;
  sf::audio::LoudnessMeter meter;

  // Analysis block: 50 ms chunks (2400 frames at 48 kHz)
  constexpr qint64 kBlockFrames = 2400;
  constexpr double kBlockSec = static_cast<double>(kBlockFrames) / sf::AudioDecoder::kSampleRate; // 0.05s

  struct BlockInfo {
    qint64 startFrame = 0;
    double startSec = 0.0;
    double rmsDb = -100.0;
  };

  std::vector<BlockInfo> blocks;
  std::vector<float> buffer(kBlockFrames * sf::AudioDecoder::kChannels);

  qint64 samplePos = 0;
  const qint64 total = info.totalSamples;

  while (samplePos < total) {
    const qint64 toRead = std::min(kBlockFrames, total - samplePos);
    const qint64 readFrames = decoder->read(samplePos, toRead, buffer.data());
    if (readFrames <= 0) break;

    // Feed loudness meter
    meter.process(buffer.data(), readFrames);

    // Compute block metrics
    BlockInfo b;
    b.startSec = static_cast<double>(samplePos) / sf::AudioDecoder::kSampleRate;
    b.startFrame = static_cast<qint64>(std::round(b.startSec * fps));
    b.rmsDb = calculateRmsDb(buffer.data(), readFrames, sf::AudioDecoder::kChannels);
    blocks.push_back(b);

    samplePos += readFrames;

    if (progressCb && total > 0) {
      progressCb(static_cast<double>(samplePos) / total * 0.8);
    }
  }

  result.integratedLufs = meter.integrated();
  result.truePeakDb = meter.truePeakDb();

  if (blocks.empty()) {
    if (progressCb) progressCb(1.0);
    return result;
  }

  // Estimate ambient noise floor (10th percentile of block RMS)
  std::vector<double> sortedRms;
  sortedRms.reserve(blocks.size());
  for (const auto& b : blocks) {
    sortedRms.push_back(b.rmsDb);
  }
  std::sort(sortedRms.begin(), sortedRms.end());
  const size_t p10Idx = blocks.size() / 10;
  result.noiseFloorDb = sortedRms[p10Idx];

  // Effective silence threshold
  const double effectiveSilenceThreshold = std::max(opts.silenceThresholdDb, result.noiseFloorDb + 5.0);
  const size_t minSilenceBlocks = std::max<size_t>(1, static_cast<size_t>(std::round(opts.minSilenceDurationSec / kBlockSec)));

  // Segmentation pass: speech vs silence
  size_t spanStartIdx = 0;
  bool inSpeech = (blocks[0].rmsDb >= effectiveSilenceThreshold);

  auto emitSpan = [&](size_t startIdx, size_t endIdx, bool isSpeech) {
    if (startIdx >= blocks.size() || endIdx >= blocks.size()) return;

    SpeechSpan sp;
    sp.id = QStringLiteral("sp_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
    sp.assetId = assetId;
    sp.startFrame = blocks[startIdx].startFrame;
    sp.startSec = blocks[startIdx].startSec;

    // End boundary: end of last block
    const double endSec = blocks[endIdx].startSec + kBlockSec;
    sp.endSec = endSec;
    sp.endFrame = static_cast<qint64>(std::round(endSec * fps));
    const size_t count = endIdx - startIdx + 1;
    // Short silences below minimum duration threshold are bridged as speech
    const bool finalIsSpeech = isSpeech || (count < minSilenceBlocks);
    sp.isSpeech = finalIsSpeech;

    double sumDb = 0.0;
    double maxDb = -100.0;
    for (size_t i = startIdx; i <= endIdx; ++i) {
      sumDb += blocks[i].rmsDb;
      maxDb = std::max(maxDb, blocks[i].rmsDb);
    }
    sp.meanVolumeDb = count > 0 ? (sumDb / count) : -100.0;
    sp.maxVolumeDb = maxDb;

    result.spans.push_back(sp);
  };

  for (size_t i = 1; i < blocks.size(); ++i) {
    const bool currentIsSpeech = (blocks[i].rmsDb >= effectiveSilenceThreshold);
    if (currentIsSpeech != inSpeech) {
      // Transition detected
      emitSpan(spanStartIdx, i - 1, inSpeech);
      spanStartIdx = i;
      inSpeech = currentIsSpeech;
    }
  }
  emitSpan(spanStartIdx, blocks.size() - 1, inSpeech);

  // Transient / Beat detection pass (energy flux spikes)
  double runningDeltaAvg = 0.0;
  qint64 lastBeatFrame = -100;
  const qint64 debounceFrames = static_cast<qint64>(std::round(0.15 * fps)); // 150 ms debounce

  for (size_t i = 1; i < blocks.size(); ++i) {
    const double delta = std::max(0.0, blocks[i].rmsDb - blocks[i - 1].rmsDb);
    runningDeltaAvg = (runningDeltaAvg * 0.9) + (delta * 0.1);

    if (delta > std::max(6.0, runningDeltaAvg * 2.2) && blocks[i].rmsDb > effectiveSilenceThreshold) {
      if (blocks[i].startFrame - lastBeatFrame >= debounceFrames) {
        result.beatFrames.push_back(blocks[i].startFrame);
        lastBeatFrame = blocks[i].startFrame;
      }
    }
  }

  if (progressCb) progressCb(1.0);
  return result;
}

} // namespace sf::index
