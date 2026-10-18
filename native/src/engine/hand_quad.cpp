#include "engine/hand_quad.h"
#include "engine/one_euro.h"

#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <utility>

namespace sf::engine {

namespace {

QString handedness(const QJsonObject& h) { return h.value(QStringLiteral("handedness")).toString(); }
double handScore(const QJsonObject& h) { return h.value(QStringLiteral("score")).toDouble(); }
QPointF landmark(const QJsonObject& h, int idx) {
  const QJsonArray lm = h.value(QStringLiteral("landmarks")).toArray();
  const QJsonArray p = lm.at(idx).toArray();
  return QPointF(p.at(0).toDouble(), p.at(1).toDouble());
}

// c in 0..7 -> p0.x,p0.y,p1.x,p1.y,p2.x,p2.y,p3.x,p3.y
double coordOf(const QuadSample& s, int c) {
  const QPointF* p = s.quad.data();
  return (c % 2 == 0) ? p[c / 2].x() : p[c / 2].y();
}

} // namespace

std::vector<QuadSample> buildHandQuads(const QJsonArray& frames, double minScore) {
  std::vector<QuadSample> out;
  out.reserve(static_cast<size_t>(frames.size()));
  for (const QJsonValue& fv : frames) {
    const QJsonObject f = fv.toObject();
    QuadSample s;
    s.t = f.value(QStringLiteral("t")).toDouble();

    QJsonArray hands = f.value(QStringLiteral("hands")).toArray();
    std::vector<QJsonObject> good;
    good.reserve(static_cast<size_t>(hands.size()));
    for (const QJsonValue& hv : hands) {
      const QJsonObject h = hv.toObject();
      if (handScore(h) >= minScore) good.push_back(h);
    }
    if (good.size() != 2) {
      s.gap = true;
      out.push_back(s);
      continue;
    }

    // Left first; same label -> sort by wrist x (landmark 0)
    std::sort(good.begin(), good.end(), [](const QJsonObject& a, const QJsonObject& b) {
      const QString la = handedness(a), lb = handedness(b);
      if (la != lb) return la == QStringLiteral("Left");
      return landmark(a, 0).x() < landmark(b, 0).x();
    });
    const QJsonObject& left = good[0];
    const QJsonObject& right = good[1];

    // left thumb (4), left index (8), right index (8), right thumb (4)
    std::array<QPointF, 4> pts = {landmark(left, 4), landmark(left, 8), landmark(right, 8),
                                  landmark(right, 4)};
    // Sort by angle around the centroid so the quad never self-intersects.
    QPointF centroid = (pts[0] + pts[1] + pts[2] + pts[3]) / 4.0;
    std::sort(pts.begin(), pts.end(), [&](const QPointF& a, const QPointF& b) {
      return std::atan2(a.y() - centroid.y(), a.x() - centroid.x()) <
             std::atan2(b.y() - centroid.y(), b.x() - centroid.x());
    });
    s.quad = pts;
    out.push_back(s);
  }
  return out;
}

void smoothQuads(std::vector<QuadSample>& samples, double minCutoff, double beta) {
  OneEuroFilter shared{minCutoff, beta, 1.0}; // derivative cutoff fixed at 1.0
  std::array<OneEuroFilter, 8> filters;
  for (OneEuroFilter& f : filters) f = shared; // one filter per coordinate
  for (QuadSample& s : samples) {
    if (s.gap) continue; // no data: keep filter state, leave quad untouched
    for (int c = 0; c < 8; ++c) {
      const double sv = filters[c].filter(coordOf(s, c), s.t);
      if (c % 2 == 0) s.quad[c / 2].setX(sv);
      else s.quad[c / 2].setY(sv);
    }
  }
}

std::vector<Gap> classifyGaps(std::vector<QuadSample>& samples, double maxHoldSec) {
  std::vector<Gap> gaps;
  const int n = static_cast<int>(samples.size());
  int i = 0;
  while (i < n) {
    if (!samples[i].gap) {
      ++i;
      continue;
    }
    // run of consecutive gap samples [i, j]
    int j = i;
    while (j + 1 < n && samples[j + 1].gap) ++j;
    const double startSec = samples[i].t;
    const double endSec = samples[j].t;
    bool held = false;
    if (endSec - startSec <= maxHoldSec) {
      // hold the last good quad across the gap
      const QuadSample* good = nullptr;
      for (int k = i - 1; k >= 0; --k)
        if (!samples[k].gap) { good = &samples[k]; break; }
      if (good) {
        for (int k = i; k <= j; ++k) { samples[k].quad = good->quad; samples[k].gap = false; }
        held = true;
      }
    }
    if (!held) {
      gaps.push_back(Gap{startSec, endSec});
    }
    i = j + 1;
  }
  return gaps;
}

std::vector<int> keyframeIndices(const std::vector<QuadSample>& samples, double tolerance) {
  std::vector<int> idx;
  idx.reserve(samples.size());
  for (int i = 0; i < static_cast<int>(samples.size()); ++i)
    if (!samples[i].gap) idx.push_back(i);
  if (idx.size() <= 2) return idx; // 0, 1 or 2 samples: keep them all

  // Start with every non-gap sample as a keyframe, then drop the ones lying on the
  // linear interpolation between their (non-gap) neighbours.
  std::vector<int> kept = idx;
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t k = 1; k + 1 < kept.size(); ++k) {
      const int i = kept[k];
      const int prev = kept[k - 1];
      const int next = kept[k + 1];
      const double t0 = samples[prev].t, t1 = samples[next].t;
      const double tt = (t1 > t0) ? (samples[i].t - t0) / (t1 - t0) : 0.0;
      bool drop = true;
      for (int c = 0; c < 8 && drop; ++c) {
        const double v = coordOf(samples[i], c);
        const double v0 = coordOf(samples[prev], c);
        const double v1 = coordOf(samples[next], c);
        if (std::abs(v - (v0 + tt * (v1 - v0))) > tolerance) drop = false;
      }
      if (drop) {
        kept.erase(kept.begin() + static_cast<qsizetype>(k));
        changed = true;
        break;
      }
    }
  }
  return kept;
}

std::vector<OpacityKeyframe> buildOpacityKeyframes(const std::vector<QuadSample>& samples,
                                                  double frameIntervalSec) {
  std::vector<OpacityKeyframe> out;
  if (samples.empty()) return out;

  auto addKey = [&](double t, double val) {
    if (!out.empty() && std::abs(out.back().t - t) < 1e-5) {
      out.back().opacity = val;
      return;
    }
    out.push_back({t, val});
  };

  int firstTrackedIdx = -1;
  for (int i = 0; i < static_cast<int>(samples.size()); ++i) {
    if (!samples[i].gap) {
      firstTrackedIdx = i;
      break;
    }
  }

  if (firstTrackedIdx == -1) {
    addKey(samples.front().t, 0.0);
    return out;
  }

  const double firstTrackedT = samples[firstTrackedIdx].t;
  if (firstTrackedT > samples.front().t + 1e-4) {
    addKey(samples.front().t, 0.0);
    if (firstTrackedT - frameIntervalSec > samples.front().t + 1e-4) {
      addKey(firstTrackedT - frameIntervalSec, 0.0);
    }
    addKey(firstTrackedT, 1.0);
  } else {
    addKey(firstTrackedT, 1.0);
  }

  int i = firstTrackedIdx;
  while (i < static_cast<int>(samples.size())) {
    int lastTrackedInRun = i;
    while (lastTrackedInRun + 1 < static_cast<int>(samples.size()) && !samples[lastTrackedInRun + 1].gap) {
      ++lastTrackedInRun;
    }
    int gapStartIdx = lastTrackedInRun + 1;
    if (gapStartIdx >= static_cast<int>(samples.size())) break;

    int gapEndIdx = gapStartIdx;
    while (gapEndIdx + 1 < static_cast<int>(samples.size()) && samples[gapEndIdx + 1].gap) {
      ++gapEndIdx;
    }

    if (samples[gapStartIdx].gap) {
      const double lastT = samples[lastTrackedInRun].t;
      addKey(lastT, 1.0);
      addKey(lastT + frameIntervalSec, 0.0);

      int nextTrackedIdx = gapEndIdx + 1;
      if (nextTrackedIdx < static_cast<int>(samples.size())) {
        const double returnT = samples[nextTrackedIdx].t;
        if (returnT - frameIntervalSec > lastT + frameIntervalSec + 1e-5) {
          addKey(returnT - frameIntervalSec, 0.0);
        }
        addKey(returnT, 1.0);
        i = nextTrackedIdx;
      } else {
        break;
      }
    } else {
      i = gapEndIdx + 1;
    }
  }

  return out;
}

double interpolateOpacity(const std::vector<OpacityKeyframe>& kfs, double t) {
  if (kfs.empty()) return 1.0;
  if (t <= kfs.front().t) return kfs.front().opacity;
  if (t >= kfs.back().t) return kfs.back().opacity;
  for (size_t i = 1; i < kfs.size(); ++i) {
    if (t <= kfs[i].t) {
      const double dt = kfs[i].t - kfs[i - 1].t;
      if (dt <= 1e-6) return kfs[i].opacity;
      const double frac = (t - kfs[i - 1].t) / dt;
      return kfs[i - 1].opacity + frac * (kfs[i].opacity - kfs[i - 1].opacity);
    }
  }
  return kfs.back().opacity;
}

} // namespace sf::engine
