#include "media/waveform.h"

#include "media/audio_decoder.h"

#include <algorithm>

namespace sf {

WaveformPeaks WaveformPeaks::reduced(int factor) const {
  WaveformPeaks out;
  out.sampleRate = sampleRate;
  out.samplesPerPeak = samplesPerPeak * std::max(factor, 1);
  out.totalSamples = totalSamples;
  if (factor <= 1) {
    out = *this;
    return out;
  }
  const qint64 n = bucketCount();
  out.minMax.reserve(static_cast<size_t>((n + factor - 1) / factor) * 2);
  for (qint64 i = 0; i < n; i += factor) {
    float lo = minAt(i);
    float hi = maxAt(i);
    for (qint64 j = i + 1; j < std::min<qint64>(i + factor, n); ++j) {
      lo = std::min(lo, minAt(j));
      hi = std::max(hi, maxAt(j));
    }
    out.minMax.push_back(lo);
    out.minMax.push_back(hi);
  }
  return out;
}

WaveformPeaks extractWaveformPeaks(const QString& path, int samplesPerPeak, const std::function<bool()>& cancel, QString* error) {
  WaveformPeaks peaks;
  auto dec = AudioDecoder::open(path, error);
  if (!dec || samplesPerPeak <= 0) return peaks;
  peaks.samplesPerPeak = samplesPerPeak;

  constexpr qint64 kChunk = 1 << 16;
  std::vector<float> buf(static_cast<size_t>(kChunk) * AudioDecoder::kChannels);
  float lo = 0, hi = 0;
  int inBucket = 0;
  qint64 pos = 0;
  while (true) {
    if (cancel && cancel()) return {};
    const qint64 got = dec->read(pos, kChunk, buf.data());
    for (qint64 i = 0; i < got; ++i) {
      const float l = buf[static_cast<size_t>(i) * 2];
      const float r = buf[static_cast<size_t>(i) * 2 + 1];
      const float mn = std::min(l, r), mx = std::max(l, r);
      if (inBucket == 0) {
        lo = mn;
        hi = mx;
      } else {
        lo = std::min(lo, mn);
        hi = std::max(hi, mx);
      }
      if (++inBucket == samplesPerPeak) {
        peaks.minMax.push_back(lo);
        peaks.minMax.push_back(hi);
        inBucket = 0;
      }
    }
    pos += got;
    if (got < kChunk) break;
  }
  if (inBucket > 0) { // the partial last bucket still gets drawn
    peaks.minMax.push_back(lo);
    peaks.minMax.push_back(hi);
  }
  peaks.totalSamples = pos;
  return peaks;
}

} // namespace sf
