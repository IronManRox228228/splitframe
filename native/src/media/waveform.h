#pragma once

#include <QString>

#include <functional>
#include <vector>

namespace sf {

// Min/max sample values per bucket of `samplesPerPeak` output samples (48 kHz), channels folded
// together (min of mins, max of maxes), which is what a timeline clip draws.
struct WaveformPeaks {
  int sampleRate = 48000;
  int samplesPerPeak = 0;
  qint64 totalSamples = 0;
  std::vector<float> minMax; // [min0, max0, min1, max1, ...]

  qint64 bucketCount() const { return static_cast<qint64>(minMax.size() / 2); }
  float minAt(qint64 i) const { return minMax[static_cast<size_t>(i) * 2]; }
  float maxAt(qint64 i) const { return minMax[static_cast<size_t>(i) * 2 + 1]; }

  // Coarser level for zoomed-out drawing: every `factor` buckets merged. Cheap, no re-decode, so
  // keep one fine level per clip and derive the rest.
  WaveformPeaks reduced(int factor) const;
};

// Decodes the whole file once (decode-bound, so run on a worker). Empty result + *error on failure;
// `cancel` is polled between chunks and yields an empty result.
WaveformPeaks extractWaveformPeaks(const QString& path, int samplesPerPeak = 480, const std::function<bool()>& cancel = {},
                                   QString* error = nullptr);

} // namespace sf
