#pragma once

// Pure functions behind safety.check (no renderer, no Qt GUI): photosensitive-flash detection per WCAG 2.3.1 / 2.3.2 (general and red
// flash, the same thresholds as ITU-R BT.1702 / Harding), stripe-pattern detection, WCAG contrast and rectangle maths.
//
// Flash method (documented approximation). A "transition" is a pair of consecutive frames. For every pixel the relative luminance
// Y (linear Rec.709 of the sRGB-decoded picture, 0..1) is compared: the pixel counts as an increase (decrease) when Y rises (falls) by
// at least minLumaDelta (10 % of the maximum luminance) and the darker of the two states is below darkerBelow (0.80).
// A transition is a general flash change in one direction when the fraction of the FRAME AREA changing that way is at least
// minAreaFraction (WCAG: 341 x 256 px of 1024 x 768 = 11.1 %; the area need not be contiguous: this is a whole-frame fraction, not a
// sliding-window region search, so scattered speckle can over-count and a small tight region is judged by its share of the frame).
// Red flash: a pixel is "saturated red" in a state when R/(R+G+B) >= redSat (0.8); it changes when (R-G-B)*320 (linear RGB 0..1, floored at 0)
// moves by more than redDelta (20) and either state is saturated red; the same area rule applies.
// Consecutive changes in the same direction collapse into one; opposing changes alternate: N alternating changes in a 1 s window are
// floor(N/2) flashes. Only frame-to-frame deltas are used (no accumulation of slow monotonic fades across many frames).

#include "core/schema.h"

#include <utility>
#include <vector>

namespace sf::engine::safety {

struct FlashParams {
  double minLumaDelta = 0.10;
  double darkerBelow = 0.80;
  double redDelta = 20.0;
  double redSat = 0.80;
  double minAreaFraction = (341.0 * 256.0) / (1024.0 * 768.0);
  int maxFlashesPerSecond = 3;
  // stripe pattern: this many light-dark pairs of swing >= patternSwing (luminance) along the frame's column or row mean profile
  int patternMinPairs = 5;
  double patternSwing = 0.20;
  int patternMinFrames = 3; // a pattern must hold this many consecutive frames
};

// Fed with the frames of a range in order (linear RGB floats, w*h*3, row-major).
class FlashDetector {
public:
  FlashDetector(int w, int h, const FlashParams& p = {});
  void push(const float* rgbLinear, Frame frame);
  // frames at which an alternating change landed (consecutive same-direction changes collapsed)
  const std::vector<Frame>& generalEvents() const { return general_; }
  const std::vector<Frame>& redEvents() const { return red_; }
  const std::vector<Frame>& patternFrames() const { return pattern_; }
  double worstGeneralArea() const { return worstGeneralArea_; }
  double worstRedArea() const { return worstRedArea_; }

private:
  int w_, h_;
  FlashParams p_;
  std::vector<float> prev_;
  int lastGeneral_ = 0, lastRed_ = 0;
  std::vector<Frame> general_, red_, pattern_;
  double worstGeneralArea_ = 0, worstRedArea_ = 0;
};

struct WindowSummary {
  std::vector<int> perSecond; // flashes in [rangeStart + s*fps, +fps)
  Frame worstStart = 0;       // start frame of the worst sliding 1 s window
  int worstFlashes = 0;
  std::vector<std::pair<Frame, Frame>> violating; // merged frame spans (first..last change) of windows with more than maxFlashes
};
WindowSummary summarize(const std::vector<Frame>& events, Frame rangeStart, Frame rangeEnd, int fps, int maxFlashes);

// Runs of consecutive frames (>= minFrames long) as {first, last}.
std::vector<std::pair<Frame, Frame>> runsOf(const std::vector<Frame>& frames, int minFrames);

// light-dark pairs of a 1-D luminance profile (zigzag with hysteresis `swing`)
int stripePairs(const std::vector<float>& profile, float swing);

double srgbToLinear(double v);                     // 0..1
double relLuminance(double r, double g, double b); // linear RGB
double contrastRatio(double l1, double l2);        // WCAG, (lighter + .05) / (darker + .05)
double percentile(std::vector<float> v, double p); // p in 0..1, nearest rank; 0 when empty

struct Rect {
  double x = 0, y = 0, w = 0, h = 0;
  double area() const { return w > 0 && h > 0 ? w * h : 0; }
};
double intersectArea(const Rect& a, const Rect& b);
Rect unite(const Rect& a, const Rect& b);

} // namespace sf::engine::safety
