#pragma once

// Loudness per ITU-R BS.1770-4 / EBU R128 (Tech 3341/3342): K-weighted gated loudness (momentary 400 ms,
// short-term 3 s, integrated with the -70 LUFS absolute and -10 LU relative gates), loudness range, and
// true peak by 4x oversampling. 48 kHz stereo (L and R both weight 1.0), the engine's native format.
// Own implementation: the K-weighting coefficients are the ones the standard prints for 48 kHz.

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace sf::audio {

constexpr double kSilenceLufs = -std::numeric_limits<double>::infinity();

class LoudnessMeter {
public:
  LoudnessMeter();
  void reset();
  // Feed interleaved stereo frames in any chunking.
  void process(const float* interleaved, std::int64_t frames);

  // LUFS; -inf until a full window has been seen
  double momentary() const;
  double shortTerm() const;
  double integrated() const; // gated over everything fed so far
  double loudnessRange() const; // LU
  double truePeakDb() const;    // dBTP over everything fed so far, -inf for silence
  double samplePeakDb() const;

private:
  struct Tp {
    float hist[24] = {};
    int pos = 0;
  };
  void trueSample(int ch, float x);
  void finishSubBlock();

  // K-weighting state (two biquads in series per channel)
  struct K {
    double s1[2] = {}, s2[2] = {};
  } k_[2];
  Tp tp_[2];
  std::int64_t inSub_ = 0;
  double subSum_[2] = {};
  std::vector<std::array<double, 2>> subs_; // per 100 ms: sum of squares per channel (normalised to mean square)
  std::vector<double> blockEnergy_; // momentary block energies (sum of channel mean squares), one per 100 ms hop
  std::vector<double> shortEnergy_;
  double peak_ = 0, truePeak_ = 0;
};

struct LoudnessResult {
  double integrated = kSilenceLufs;
  double range = 0;
  double truePeakDb = kSilenceLufs;
  double samplePeakDb = kSilenceLufs;
};

LoudnessResult measureLoudness(const float* interleaved, std::int64_t frames);

// Gain in dB that brings `r` to `targetLufs` (-14 streaming, -16 podcasts, -23 EBU R128 broadcast). With
// `truePeakCeilingDb` the gain is reduced if the result would push the true peak above it. 0 for silence.
double normalizeGainDb(const LoudnessResult& r, double targetLufs, std::optional<double> truePeakCeilingDb = std::nullopt);

} // namespace sf::audio
