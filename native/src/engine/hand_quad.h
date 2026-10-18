#pragma once
// Hand-tracked quad mask helpers: pure, deterministic functions (no model runs here) used by
// mask.trackHands. The landmarks come from the B9 `hands` analysis (normalised frame coords).
//
//   buildHandQuads   -> one QuadSample per analysis frame (a 4-point quad, or a gap)
//   smoothQuads      -> One-Euro smoothing of the 8 coordinates over time
//   classifyGaps     -> short gaps hold the last good quad; long gaps stay flagged
//   keyframeIndices  -> thinning: drop samples on the linear interpolation between neighbours
#include <QJsonArray>
#include <QPointF>

#include <array>
#include <vector>

namespace sf::engine {

struct QuadSample {
  double t = 0;                 // seconds into the analysed asset
  std::array<QPointF, 4> quad{}; // p0..p3 (meaningful when !gap)
  bool gap = false;             // fewer than two scoring hands at this sample
};
struct Gap {
  double startSec = 0, endSec = 0; // seconds; long gaps only (short gaps are held)
};

// frames: analysis.hands.track result frames[{t, hands[{handedness, score, landmarks[21][x,y,z]]}]}
// Returns one sample per frame. When exactly two hands score >= minScore, the quad is
//   left thumb (4), left index (8), right index (8), right thumb (4)   (MediaPipe indices)
// with Left first (same label -> sorted by wrist x, landmark 0) and the four points sorted by
// angle around their centroid so the quad never self-intersects. Otherwise the sample is a gap.
std::vector<QuadSample> buildHandQuads(const QJsonArray& frames, double minScore);

// Smooth each of the 8 coordinates over time with a One-Euro filter (minCutoff, beta; derivative
// cutoff fixed at 1.0). Gap samples are skipped; their quad is left untouched.
void smoothQuads(std::vector<QuadSample>& samples, double minCutoff = 1.0, double beta = 0.05);

// Classify runs of gap samples. Gaps <= maxHoldSec (seconds) hold the last good quad
// (gap=false, quad copied). Longer gaps stay flagged and are returned as long gaps.
std::vector<Gap> classifyGaps(std::vector<QuadSample>& samples, double maxHoldSec = 0.3);

// Indices of samples to keep as keyframes. A non-gap sample is dropped when all 8 coordinates
// lie within `tolerance` of the linear interpolation between its (non-gap) neighbours. The first
// and last non-gap sample are always kept. Gaps are never keyframes.
std::vector<int> keyframeIndices(const std::vector<QuadSample>& samples, double tolerance = 0.002);

struct OpacityKeyframe {
  double t = 0.0;
  double opacity = 1.0;
};

// Builds opacity keyframes for long gaps:
// 1.0 on last tracked frame before long gap, 0.0 on next frame, hold 0.0 through gap,
// 1.0 when hands return. If clip starts in a gap, 0.0 at start, 1.0 on first tracked frame.
std::vector<OpacityKeyframe> buildOpacityKeyframes(const std::vector<QuadSample>& samples,
                                                  double frameIntervalSec = 1.0 / 30.0);

double interpolateOpacity(const std::vector<OpacityKeyframe>& kfs, double t);

} // namespace sf::engine
