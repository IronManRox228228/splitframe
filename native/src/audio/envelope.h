#pragma once

// A piecewise curve over sample positions: keyframes (value + easing into it) converted to samples.
// Same semantics as the renderer's resolveKeyframes and the Electron export's volumeExpression:
// flat before the first and after the last point, linear between points unless the later point's
// easing says otherwise. Evaluated per sample, so a ramp lands on exactly the sample its keyframes name.

#include "core/schema.h"

#include <cstdint>
#include <vector>

namespace sf::audio {

class Envelope {
public:
  Envelope() = default;
  // Points are keyframe frames + `offsetFrames` (item-local keyframes pass the item start, absolute
  // ones 0), converted at `fps`. Unsorted input is sorted.
  static Envelope fromKeyframes(const KeyframeList& kfs, std::int64_t fps, Frame offsetFrames = 0);
  static Envelope constant(double v);

  bool empty() const { return pts_.empty(); }
  bool isConstant() const { return pts_.size() <= 1; }
  double at(double samplePos) const;
  // out[i] = at(start + i)
  void fill(std::int64_t start, int count, float* out) const;

private:
  struct Pt {
    double pos;
    double value;
    Easing easing;
  };
  std::vector<Pt> pts_;
};

double applyEasing(double t, Easing e);

} // namespace sf::audio
