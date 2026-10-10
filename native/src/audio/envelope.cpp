#include "audio/envelope.h"

#include "audio/timebase.h"

#include <algorithm>

namespace sf::audio {

double applyEasing(double t, Easing e) {
  switch (e) {
    case Easing::Hold: return 0;
    case Easing::EaseIn: return t * t;
    case Easing::EaseOut: return 1 - (1 - t) * (1 - t);
    case Easing::EaseInOut: {
      const double u = -2 * t + 2;
      return t < 0.5 ? 2 * t * t : 1 - u * u / 2;
    }
    case Easing::Linear: break;
  }
  return t;
}

Envelope Envelope::fromKeyframes(const KeyframeList& kfs, std::int64_t fps, Frame offsetFrames) {
  Envelope e;
  e.pts_.reserve(kfs.size());
  for (const Keyframe& k : kfs) e.pts_.push_back({static_cast<double>(frameToSample(k.frame + offsetFrames, fps)), k.value, k.easing});
  std::stable_sort(e.pts_.begin(), e.pts_.end(), [](const Pt& a, const Pt& b) { return a.pos < b.pos; });
  return e;
}

Envelope Envelope::constant(double v) {
  Envelope e;
  e.pts_.push_back({0, v, Easing::Linear});
  return e;
}

double Envelope::at(double pos) const {
  if (pts_.empty()) return 0;
  if (pos <= pts_.front().pos) return pts_.front().value;
  if (pos >= pts_.back().pos) return pts_.back().value;
  const auto it = std::upper_bound(pts_.begin(), pts_.end(), pos, [](double p, const Pt& pt) { return p < pt.pos; });
  const Pt& b = *it;
  const Pt& a = *(it - 1);
  const double span = std::max(1.0, b.pos - a.pos);
  return a.value + (b.value - a.value) * applyEasing((pos - a.pos) / span, b.easing);
}

void Envelope::fill(std::int64_t start, int count, float* out) const {
  for (int i = 0; i < count; ++i) out[i] = static_cast<float>(at(static_cast<double>(start + i)));
}

} // namespace sf::audio
