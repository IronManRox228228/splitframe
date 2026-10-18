// Hand-tracked quad mask: pure, deterministic helpers (engine/hand_quad.h, engine/one_euro.h).
// No GPU, no model: builds synthetic `hands` analysis frames and checks quad construction,
// One-Euro smoothing, gap holding / fading and keyframe thinning.
#include "engine/hand_quad.h"
#include "engine/one_euro.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPointF>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace sf::engine;

namespace {

QJsonArray handPoint(double x, double y) { return QJsonArray{x, y, 0.0}; }

// One hand with only the landmarks the quad needs (0 wrist, 4 thumb tip, 8 index fingertip);
// the rest are zero.
QJsonObject hand(QString handedness, double score, QPointF wrist, QPointF thumb, QPointF index) {
  QJsonArray lm;
  for (int i = 0; i < 21; ++i) lm.append(QJsonArray{0.0, 0.0, 0.0});
  lm[0] = handPoint(wrist.x(), wrist.y());
  lm[4] = handPoint(thumb.x(), thumb.y());
  lm[8] = handPoint(index.x(), index.y());
  return QJsonObject{{QStringLiteral("handedness"), handedness}, {QStringLiteral("score"), score},
                     {QStringLiteral("landmarks"), lm}};
}

QJsonObject frame(double t, const QJsonArray& hands) {
  return QJsonObject{{QStringLiteral("t"), t}, {QStringLiteral("hands"), hands}};
}

QJsonArray bothHands() {
  return QJsonArray{
    hand(QStringLiteral("Left"), 0.9, QPointF{0.3, 0.5}, QPointF{0.25, 0.45}, QPointF{0.28, 0.55}),
    hand(QStringLiteral("Right"), 0.9, QPointF{0.7, 0.5}, QPointF{0.75, 0.55}, QPointF{0.72, 0.45})};
}

double stdDev(const std::vector<double>& v) {
  double m = 0;
  for (double x : v) m += x;
  m /= v.size();
  double s = 0;
  for (double x : v) s += (x - m) * (x - m);
  return std::sqrt(s / v.size());
}

} // namespace

class TstHandQuad : public QObject {
  Q_OBJECT

private slots:
  void quadOrderStable() {
    const QJsonArray ordered = bothHands(); // Left then Right
    const QJsonArray swapped{QJsonObject{hand(QStringLiteral("Right"), 0.9, QPointF{0.7, 0.5},
                                                QPointF{0.75, 0.55}, QPointF{0.72, 0.45})},
                             QJsonObject{hand(QStringLiteral("Left"), 0.9, QPointF{0.3, 0.5},
                                               QPointF{0.25, 0.45}, QPointF{0.28, 0.55})}};
    const QJsonArray framesOrdered{QJsonObject{{QStringLiteral("t"), 0.0}, {QStringLiteral("hands"), ordered}}};
    const QJsonArray framesSwapped{QJsonObject{{QStringLiteral("t"), 0.0}, {QStringLiteral("hands"), swapped}}};

    const auto qo = buildHandQuads(framesOrdered, 0.6).at(0).quad;
    const auto qs = buildHandQuads(framesSwapped, 0.6).at(0).quad;
    QVERIFY(!buildHandQuads(framesOrdered, 0.6).at(0).gap);
    QCOMPARE(qo, qs);
    // a convex quad: points all different
    QVERIFY(qo[0] != qo[1] && qo[1] != qo[2] && qo[2] != qo[3] && qo[3] != qo[0]);
  }

  void oneHandIsGap() {
    const QJsonArray frames{QJsonObject{{QStringLiteral("t"), 0.0},
                                        {QStringLiteral("hands"), QJsonArray{bothHands().at(0)}}}};
    const auto samples = buildHandQuads(frames, 0.6);
    QCOMPARE(samples.size(), size_t(1));
    QVERIFY(samples.at(0).gap);
  }

  void shortGapHeld() {
    // hands at t=0,0.1,0.2; gap at t=0.3,0.4 (0.1 s <= 0.3 s hold); hands at t=0.5,0.6
    const auto pair = bothHands();
    const QJsonArray frames{
      frame(0.0, pair), frame(0.1, pair), frame(0.2, pair),
      frame(0.3, QJsonArray{}), frame(0.4, QJsonArray{}),
      frame(0.5, pair), frame(0.6, pair)};
    auto samples = buildHandQuads(frames, 0.6);
    const QuadSample lastGood = samples.at(2); // t = 0.2
    const auto gaps = classifyGaps(samples, 0.3);
    QVERIFY(gaps.empty()); // short gap: no long gap recorded
    QVERIFY(!samples.at(3).gap);
    QVERIFY(!samples.at(4).gap);
    QCOMPARE(samples.at(3).quad, lastGood.quad); // held
    QCOMPARE(samples.at(4).quad, lastGood.quad); // held
  }

  void longGapFadesOpacity() {
    // hands at t=0 and t=1.2; gap at t=0.4..0.8 (0.4 s > 0.3 s long gap)
    const auto pair = bothHands();
    const QJsonArray frames{
      frame(0.0, pair),
      frame(0.4, QJsonArray{}), frame(0.8, QJsonArray{}),
      frame(1.2, pair)};
    auto samples = buildHandQuads(frames, 0.6);
    const auto gaps = classifyGaps(samples, 0.3);
    QCOMPARE(gaps.size(), size_t(1));
    QCOMPARE(gaps.at(0).startSec, 0.4);
    QCOMPARE(gaps.at(0).endSec, 0.8);
    QVERIFY(samples.at(1).gap);
    QVERIFY(samples.at(2).gap);

    const auto opKfs = buildOpacityKeyframes(samples, 0.05);
    QVERIFY(!opKfs.empty());
    // Just before gap (t=0.0): opacity = 1.0
    QCOMPARE(interpolateOpacity(opKfs, 0.0), 1.0);
    // Mid-gap (t=0.6): opacity = 0.0
    QCOMPARE(interpolateOpacity(opKfs, 0.6), 0.0);
    // On return (t=1.2): opacity = 1.0
    QCOMPARE(interpolateOpacity(opKfs, 1.2), 1.0);
  }

  void handsPresentGapPresentAgain() {
    // hands at 0.0..0.2, gap at 0.4..0.8, hands return at 1.0..1.2
    const auto pair = bothHands();
    const QJsonArray frames{
      frame(0.0, pair), frame(0.1, pair), frame(0.2, pair),
      frame(0.4, QJsonArray{}), frame(0.6, QJsonArray{}), frame(0.8, QJsonArray{}),
      frame(1.0, pair), frame(1.1, pair), frame(1.2, pair)};
    auto samples = buildHandQuads(frames, 0.6);
    const auto gaps = classifyGaps(samples, 0.3);
    QCOMPARE(gaps.size(), size_t(1));

    const auto opKfs = buildOpacityKeyframes(samples, 0.05);
    QCOMPARE(interpolateOpacity(opKfs, 0.2), 1.0);
    QCOMPARE(interpolateOpacity(opKfs, 0.6), 0.0);
    QCOMPARE(interpolateOpacity(opKfs, 1.0), 1.0);
  }

  void smoothingReducesJitter() {
    // p0.x wanders around 0.5 with fixed-seed +/-0.01 jitter; One-Euro must reduce the spread
    uint32_t seed = 12345u;
    auto rnd = [&]() {
      seed = seed * 1664525u + 1013904223u;
      return ((seed >> 16) / 65536.0) * 2.0 - 1.0;
    };
    std::vector<double> raw, smoothed;
    raw.reserve(200);
    for (int k = 0; k < 200; ++k) raw.push_back(0.5 + 0.01 * rnd());
    OneEuroFilter filt; // minCutoff 1.0, beta 0.05, derivative cutoff 1.0
    double t = 0;
    for (double v : raw) {
      smoothed.push_back(filt.filter(v, t));
      t += 0.1;
    }
    QVERIFY(stdDev(smoothed) < stdDev(raw));
  }

  void thinningKeepsShape() {
    // linear motion over 30 samples -> 2 keyframes
    {
      std::vector<QuadSample> samples;
      for (int k = 0; k < 30; ++k) {
        QuadSample s;
        s.t = k;
        s.quad.fill(QPointF{0, 0});
        s.quad[0] = QPointF{k / 29.0, 0.5};
        samples.push_back(s);
      }
      QCOMPARE(keyframeIndices(samples).size(), size_t(2));
    }
    // sine motion -> more than 5 keyframes
    {
      std::vector<QuadSample> samples;
      for (int k = 0; k < 30; ++k) {
        QuadSample s;
        s.t = k * 0.2;
        s.quad.fill(QPointF{0, 0});
        s.quad[0] = QPointF{0.5 + 0.3 * std::sin(s.t), 0.5};
        samples.push_back(s);
      }
      QVERIFY(keyframeIndices(samples).size() > 5);
    }
  }
};

QTEST_MAIN(TstHandQuad)
#include "tst_hand_quad.moc"
