#include "engine/safety_analysis.h"

#include <algorithm>
#include <cmath>

namespace sf::engine::safety {

double srgbToLinear(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
double relLuminance(double r, double g, double b) { return 0.2126 * r + 0.7152 * g + 0.0722 * b; }
double contrastRatio(double l1, double l2) {
  const double hi = std::max(l1, l2), lo = std::min(l1, l2);
  return (hi + 0.05) / (lo + 0.05);
}
double percentile(std::vector<float> v, double p) {
  if (v.empty()) return 0;
  const size_t k = std::min(v.size() - 1, static_cast<size_t>(std::floor(std::clamp(p, 0.0, 1.0) * static_cast<double>(v.size() - 1) + 0.5)));
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
  return v[k];
}
double intersectArea(const Rect& a, const Rect& b) {
  const double x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y), x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
  return x1 > x0 && y1 > y0 ? (x1 - x0) * (y1 - y0) : 0;
}
Rect unite(const Rect& a, const Rect& b) {
  if (a.area() <= 0) return b;
  if (b.area() <= 0) return a;
  const double x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y), x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
  return {x0, y0, x1 - x0, y1 - y0};
}

int stripePairs(const std::vector<float>& p, float swing) {
  if (p.size() < 2) return 0;
  float lo = p[0], hi = p[0], ext = p[0];
  int dir = 0, turns = 0;
  for (float v : p) {
    if (dir == 0) {
      lo = std::min(lo, v);
      hi = std::max(hi, v);
      if (hi - lo >= swing) {
        dir = v >= hi ? 1 : -1;
        ++turns;
        ext = v;
      }
    } else if (dir == 1) {
      ext = std::max(ext, v);
      if (ext - v >= swing) {
        dir = -1;
        ++turns;
        ext = v;
      }
    } else {
      ext = std::min(ext, v);
      if (v - ext >= swing) {
        dir = 1;
        ++turns;
        ext = v;
      }
    }
  }
  return turns / 2;
}

FlashDetector::FlashDetector(int w, int h, const FlashParams& p) : w_(w), h_(h), p_(p) {}

void FlashDetector::push(const float* cur, Frame frame) {
  const size_t n = static_cast<size_t>(w_) * static_cast<size_t>(h_);
  if (n == 0) return;
  // stripe pattern on this frame (column and row mean luminance)
  {
    std::vector<float> cols(static_cast<size_t>(w_), 0.f), rows(static_cast<size_t>(h_), 0.f);
    for (int y = 0; y < h_; ++y)
      for (int x = 0; x < w_; ++x) {
        const float* px = cur + (static_cast<size_t>(y) * static_cast<size_t>(w_) + static_cast<size_t>(x)) * 3;
        const float Y = static_cast<float>(relLuminance(px[0], px[1], px[2]));
        cols[static_cast<size_t>(x)] += Y;
        rows[static_cast<size_t>(y)] += Y;
      }
    for (float& c : cols) c /= static_cast<float>(h_);
    for (float& r : rows) r /= static_cast<float>(w_);
    const float swing = static_cast<float>(p_.patternSwing);
    if (stripePairs(cols, swing) >= p_.patternMinPairs || stripePairs(rows, swing) >= p_.patternMinPairs) pattern_.push_back(frame);
  }
  if (!prev_.empty()) {
    size_t incN = 0, decN = 0, rIncN = 0, rDecN = 0;
    for (size_t i = 0; i < n; ++i) {
      const float* a = prev_.data() + i * 3;
      const float* b = cur + i * 3;
      const double ya = relLuminance(a[0], a[1], a[2]), yb = relLuminance(b[0], b[1], b[2]);
      const double d = yb - ya;
      if (std::fabs(d) >= p_.minLumaDelta && std::min(ya, yb) < p_.darkerBelow) {
        if (d > 0) ++incN;
        else ++decN;
      }
      const double sa = static_cast<double>(a[0]) + a[1] + a[2], sb = static_cast<double>(b[0]) + b[1] + b[2];
      const bool redA = sa > 1e-6 && a[0] / sa >= p_.redSat, redB = sb > 1e-6 && b[0] / sb >= p_.redSat;
      if (redA || redB) {
        const double ra = std::max(0.0, (static_cast<double>(a[0]) - a[1] - a[2]) * 320.0), rb = std::max(0.0, (static_cast<double>(b[0]) - b[1] - b[2]) * 320.0);
        if (std::fabs(rb - ra) > p_.redDelta) {
          if (rb > ra) ++rIncN;
          else ++rDecN;
        }
      }
    }
    const auto dirOf = [&](size_t inc, size_t dec, double* worst) {
      const double ai = static_cast<double>(inc) / static_cast<double>(n), ad = static_cast<double>(dec) / static_cast<double>(n);
      *worst = std::max(*worst, std::max(ai, ad));
      if (ai >= p_.minAreaFraction && ai >= ad) return 1;
      if (ad >= p_.minAreaFraction) return -1;
      return 0;
    };
    const int g = dirOf(incN, decN, &worstGeneralArea_), r = dirOf(rIncN, rDecN, &worstRedArea_);
    if (g != 0 && g != lastGeneral_) general_.push_back(frame);
    if (g != 0) lastGeneral_ = g;
    if (r != 0 && r != lastRed_) red_.push_back(frame);
    if (r != 0) lastRed_ = r;
  }
  prev_.assign(cur, cur + n * 3);
}

namespace {
int countIn(const std::vector<Frame>& ev, Frame from, Frame to) { // [from, to)
  const auto lo = std::lower_bound(ev.begin(), ev.end(), from), hi = std::lower_bound(ev.begin(), ev.end(), to);
  return static_cast<int>(hi - lo);
}
} // namespace

WindowSummary summarize(const std::vector<Frame>& ev, Frame rs, Frame re, int fps, int maxFlashes) {
  WindowSummary s;
  fps = std::max(1, fps);
  for (Frame f = rs; f < re; f += fps) s.perSecond.push_back(countIn(ev, f, f + fps) / 2);
  s.worstStart = rs;
  for (size_t k = 0; k < ev.size(); ++k) {
    const int flashes = countIn(ev, ev[k], ev[k] + fps) / 2;
    if (flashes > s.worstFlashes) {
      s.worstFlashes = flashes;
      s.worstStart = ev[k];
    }
    if (flashes > maxFlashes) {
      const Frame last = *(std::lower_bound(ev.begin(), ev.end(), ev[k] + fps) - 1);
      if (!s.violating.empty() && ev[k] <= s.violating.back().second) s.violating.back().second = std::max(s.violating.back().second, last);
      else s.violating.push_back({ev[k], last});
    }
  }
  return s;
}

std::vector<std::pair<Frame, Frame>> runsOf(const std::vector<Frame>& frames, int minFrames) {
  std::vector<std::pair<Frame, Frame>> out;
  size_t i = 0;
  while (i < frames.size()) {
    size_t j = i;
    while (j + 1 < frames.size() && frames[j + 1] == frames[j] + 1) ++j;
    if (static_cast<int>(j - i + 1) >= minFrames) out.push_back({frames[i], frames[j]});
    i = j + 1;
  }
  return out;
}

} // namespace sf::engine::safety
