#pragma once

// Timeline frames <-> 48 kHz samples, in integer arithmetic only. A frame boundary f sits at sample
// round(f * 48000 / fps), computed from the absolute frame so there is no accumulated rounding: after
// two hours the audio is exactly where the video is, and two clips that butt at a frame boundary
// butt at the same sample. Rounding is half up (what jsRound does on the video side).

#include "core/time.h"

#include <cstdint>

namespace sf::audio {

constexpr int kRate = 48000;
constexpr int kChannels = 2;

using Sample = std::int64_t;

inline std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
  std::int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
  return q;
}

// Sample index of timeline frame `f` at `fps` frames per second (fps >= 1).
inline Sample frameToSample(Frame f, std::int64_t fps) { return floorDiv(f * kRate * 2 + fps, 2 * fps); }
// The frame containing `s` (floor).
inline Frame sampleToFrame(Sample s, std::int64_t fps) { return floorDiv(s * fps, kRate); }
// Fractional samples for a frame count (fade lengths and the like).
inline double framesToSamplesF(Frame f, std::int64_t fps) { return static_cast<double>(f) * kRate / static_cast<double>(fps); }

} // namespace sf::audio
