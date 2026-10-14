// The colour measurement layer on synthetic pixels with known answers, and the proposals built on it.

#include "render/colour_analysis.h"

#include <QTest>

#include <cmath>

using namespace sf;
using namespace sf::render;

namespace {

PixelSet fromDisplay8(const std::vector<std::array<int, 3>>& px) {
  std::vector<float> lin;
  for (const auto& p : px)
    for (int c = 0; c < 3; ++c) lin.push_back(static_cast<float>(toLinear(Transfer::Srgb, p[static_cast<size_t>(c)] / 255.0)));
  return pixelsFromLinear(lin);
}

PixelSet fromLinearGrey(const std::vector<double>& levels) {
  std::vector<float> lin;
  for (double v : levels) lin.insert(lin.end(), {static_cast<float>(v), static_cast<float>(v), static_cast<float>(v)});
  return pixelsFromLinear(lin);
}

// A spread of colours and brightnesses (a 10 x 10 x 10 lattice, skewed towards the shadows like real pictures)
PixelSet testImage() {
  std::vector<float> lin;
  for (int i = 0; i < 10; ++i)
    for (int j = 0; j < 10; ++j)
      for (int k = 0; k < 10; ++k) {
        const auto v = [](int n) { return static_cast<float>(0.03 + 0.55 * std::pow(n / 9.0, 1.6)); };
        lin.insert(lin.end(), {v(i), v(j), v(k)});
      }
  return pixelsFromLinear(lin);
}

} // namespace

class TstColourAnalysis : public QObject {
  Q_OBJECT
private slots:
  void greyRampPercentilesAndHistogram() {
    std::vector<std::array<int, 3>> px;
    for (int i = 0; i < 256; ++i) px.push_back({i, i, i});
    const ColourMetrics m = analyze(fromDisplay8(px));
    QVERIFY(m.valid);
    QCOMPARE(m.pixels, qint64(256));
    QVERIFY(std::fabs(m.p50 - toLinear(Transfer::Srgb, 128.0 / 255.0)) < 0.004);
    QVERIFY(std::fabs(m.p1 - toLinear(Transfer::Srgb, 3.0 / 255.0)) < 0.0005);
    QVERIFY(std::fabs(m.p99 - toLinear(Transfer::Srgb, 252.0 / 255.0)) < 0.01);
    QVERIFY(m.p5 < m.p50 && m.p50 < m.p95);
    double sum = 0;
    for (double b : m.histogram) {
      QVERIFY(std::fabs(b - 1.0 / 64.0) < 1e-9); // 4 of the 256 levels per bin
      sum += b;
    }
    QVERIFY(std::fabs(sum - 1.0) < 1e-9);
    // levels 0,1,2 are crushed, 253..255 clipped
    QVERIFY(std::fabs(m.crushedPct.all - 300.0 / 256.0) < 1e-6);
    QVERIFY(std::fabs(m.clippedPct.all - 300.0 / 256.0) < 1e-6);
    // exposure meters levels 3..255 (crushed 0..2 skipped): their median is level 129
    QVERIFY(std::fabs(m.exposureStops - std::log2(toLinear(Transfer::Srgb, 129.0 / 255.0) / 0.18)) < 0.01);
  }

  void exposureIgnoresCrushedPixels() {
    // mostly black frame with a mid-grey patch: the plain median is black, the metered exposure is the patch
    std::vector<double> levels;
    for (int i = 0; i < 1000; ++i) levels.push_back(i < 850 ? 0.0 : 0.18);
    const ColourMetrics m = analyze(fromLinearGrey(levels));
    QVERIFY2(std::fabs(m.exposureStops) < 0.02, qPrintable(QString::number(m.exposureStops)));
    // three stops down on the patch moves exposure three stops, black stays black
    for (double& l : levels) l /= 8.0;
    const ColourMetrics d = analyze(fromLinearGrey(levels));
    QVERIFY2(std::fabs(d.exposureStops - (m.exposureStops - 3.0)) < 0.05, qPrintable(QString::number(d.exposureStops)));
    // a frame that is all crushed falls back to every pixel instead of failing
    const ColourMetrics b = analyze(fromLinearGrey(std::vector<double>(500, 0.0)));
    QVERIFY(b.valid && b.exposureStops < -10);
  }

  void crushedAndClippedPercentages() {
    std::vector<std::array<int, 3>> px;
    for (int i = 0; i < 2000; ++i) px.push_back(i < 200 ? std::array<int, 3>{0, 0, 0} : i < 300 ? std::array<int, 3>{255, 255, 255} : std::array<int, 3>{118, 118, 118});
    const ColourMetrics m = analyze(fromDisplay8(px));
    QVERIFY(std::fabs(m.crushedPct.all - 10.0) < 1e-6);
    QVERIFY(std::fabs(m.clippedPct.all - 5.0) < 1e-6);
    QVERIFY(std::fabs(m.crushedPct.g - 10.0) < 1e-6 && std::fabs(m.clippedPct.b - 5.0) < 1e-6);
    QVERIFY(m.histogram[0] > 0.0999 && m.histogram[63] > 0.0499);
  }

  void contrastOfTwoLevelsIsExact() {
    std::vector<double> levels;
    for (int i = 0; i < 400; ++i) levels.push_back(i % 2 ? 0.4 : 0.05);
    const ColourMetrics m = analyze(fromLinearGrey(levels));
    QVERIFY2(std::fabs(m.contrastStops - 3.0) < 1e-5, qPrintable(QString::number(m.contrastStops)));
    QVERIFY(m.rmsContrast > 0.1);
  }

  void warmGradeIsMeasuredAndAutoBalanceInvertsIt() {
    std::vector<double> levels;
    for (int i = 0; i < 200; ++i) levels.push_back(0.08 + 0.2 * i / 199.0); // mid-range greys, median 0.18
    PrimaryGrade warm;
    warm.temperature = 0.3;
    warm.tint = -0.1;
    const PixelSet graded = gradePixels(fromLinearGrey(levels), warm);
    const ColourMetrics m = analyze(graded);
    QVERIFY2(std::fabs(m.castTemperature - 0.3) < 0.02, qPrintable(QString::number(m.castTemperature)));
    QVERIFY2(std::fabs(m.castTint + 0.1) < 0.02, qPrintable(QString::number(m.castTint)));
    QVERIFY(m.neutralPct > 90);
    const QJsonObject cast = m.toJson().value("cast").toObject();
    QCOMPARE(cast.value("warmCool").toString(), QStringLiteral("warm"));
    QVERIFY(std::fabs(cast.value("proposedCorrection").toObject().value("temperature").toDouble() + 0.3) < 0.02);

    const Proposal p = proposeAutoBalance(graded);
    QVERIFY2(std::fabs(p.primaries.temperature + 0.3) < 0.02, qPrintable(QString::number(p.primaries.temperature)));
    QVERIFY2(std::fabs(p.primaries.tint - 0.1) < 0.02, qPrintable(QString::number(p.primaries.tint)));
    QVERIFY(std::fabs(p.predicted.castTemperature) < 0.02 && std::fabs(p.predicted.castTint) < 0.02);
    QVERIFY(std::fabs(p.primaries.exposure) < 0.3); // median was already close to mid grey
  }

  void autoBalanceLiftsDarkPicturesWithinTheLimit() {
    std::vector<double> levels;
    for (int i = 0; i < 200; ++i) levels.push_back(0.01 + 0.04 * i / 199.0); // very dark: median 0.03
    const Proposal p = proposeAutoBalance(fromLinearGrey(levels));
    QVERIFY(p.primaries.exposure > 1.5 && p.primaries.exposure <= 2.0);
    QVERIFY(std::fabs(p.primaries.temperature) < 1e-9);
    QVERIFY(p.predicted.exposureStops > p.before.exposureStops);
    // a picture already at the target is left alone
    const Proposal q = proposeAutoBalance(fromLinearGrey(std::vector<double>(100, 0.18)));
    QCOMPARE(q.primaries, PrimaryGrade{});
  }

  void nightFrameWithLightsKeepsItsKey() {
    // 1988 dark background pixels (median 0.012) and 12 small bright light sources (0.9, 0.6 % of the picture: p99 stays dark, so the key comes from the bright share): a night shot, not an underexposed one
    std::vector<double> levels;
    for (int i = 0; i < 1988; ++i) levels.push_back(0.008 + 0.008 * (i % 10) / 9.0);
    for (int i = 0; i < 12; ++i) levels.push_back(0.9);
    const PixelSet night = fromLinearGrey(levels);
    const Proposal p = proposeAutoBalance(night);
    QCOMPARE(p.sceneKey.requested, QStringLiteral("auto"));
    QVERIFY2(p.sceneKey.detected == "low" && !p.sceneKey.reason.isEmpty(), qPrintable(p.sceneKey.detected + ": " + p.sceneKey.reason));
    QVERIFY2(p.primaries.exposure <= 0.5 + 1e-9 && p.primaries.exposure >= -0.5, qPrintable(QString::number(p.primaries.exposure)));
    // the same pixels forced to normal get the full mid-grey correction the key is there to prevent
    AutoBalanceOptions forced;
    forced.sceneKey = SceneKey::Normal;
    const Proposal n = proposeAutoBalance(night, forced);
    QCOMPARE(n.sceneKey.detected, QStringLiteral("normal"));
    QCOMPARE(n.sceneKey.requested, QStringLiteral("normal"));
    QVERIFY2(n.primaries.exposure > 1.0, qPrintable(QString::number(n.primaries.exposure)));
  }

  void underexposedGreyFrameIsStillCorrected() {
    std::vector<double> levels;
    for (int i = 0; i < 200; ++i) levels.push_back(0.02 + 0.04 * i / 199.0); // median 0.04, nothing bright in it
    const Proposal p = proposeAutoBalance(fromLinearGrey(levels));
    QCOMPARE(p.sceneKey.detected, QStringLiteral("normal"));
    QVERIFY2(p.primaries.exposure > 1.5, qPrintable(QString::number(p.primaries.exposure)));
  }

  void brightFrameKeepsItsKeyAndNeverMovesMoreThanHalfAStop() {
    std::vector<double> levels;
    for (int i = 0; i < 200; ++i) levels.push_back(0.3 + 0.45 * i / 199.0); // median 0.52, blacks lifted, nothing clipped
    const PixelSet bright = fromLinearGrey(levels);
    const Proposal p = proposeAutoBalance(bright);
    QCOMPARE(p.sceneKey.detected, QStringLiteral("high"));
    QVERIFY2(p.primaries.exposure >= -0.5 - 1e-9 && p.primaries.exposure <= 0.0, qPrintable(QString::number(p.primaries.exposure)));
    AutoBalanceOptions low;
    low.sceneKey = SceneKey::Low;
    const Proposal l = proposeAutoBalance(bright, low); // an explicit key is obeyed, and still capped
    QCOMPARE(l.sceneKey.detected, QStringLiteral("low"));
    QVERIFY(std::fabs(l.primaries.exposure) <= 0.5 + 1e-9);
  }

  void skinPatchIsFoundWithItsHueOffset() {
    std::vector<std::array<int, 3>> px(200, {224, 172, 140});
    for (int i = 0; i < 200; ++i) px.push_back({20, 60, 160}); // a blue sky is not skin
    const ColourMetrics m = analyze(fromDisplay8(px));
    QVERIFY2(std::fabs(m.skinPct - 50.0) < 1e-6, qPrintable(QString::number(m.skinPct)));
    QVERIFY2(std::fabs(m.skinHueDeg - 128.8) < 1.0, qPrintable(QString::number(m.skinHueDeg)));
    QVERIFY(std::fabs(m.skinOffsetDeg - (m.skinHueDeg - 123.0)) < 1e-9);
    const ColourMetrics none = analyze(fromDisplay8(std::vector<std::array<int, 3>>(50, {20, 60, 160})));
    QCOMPARE(none.skinPct, 0.0);
  }

  void shotMatchRecoversAKnownGrade() {
    const PixelSet original = testImage();
    PrimaryGrade known;
    known.exposure = 0.4;
    known.temperature = 0.15;
    known.tint = -0.08;
    known.contrast = 1.15;
    known.saturation = 1.2;
    const PixelSet graded = gradePixels(original, known);
    const ColourMetrics ref = analyze(original);
    const Proposal p = proposeMatch(graded, ref);
    QVERIFY2(p.error < 0.05, qPrintable(QString::number(p.error)));
    // and the statistics really moved onto the reference's
    QVERIFY(std::fabs(p.predicted.lumaLogMean - ref.lumaLogMean) < 0.05);
    QVERIFY(std::fabs(p.predicted.lumaLogStd - ref.lumaLogStd) < 0.05);
    QVERIFY(std::fabs(p.predicted.chromaMean - ref.chromaMean) < 0.02);
    // before the match the copies differed clearly
    QVERIFY(std::fabs(analyze(graded).lumaLogMean - ref.lumaLogMean) > 0.2);
    // the inverse of the known grade is what comes out (exposure -0.4 within what the other terms trade)
    QVERIFY2(p.primaries.exposure < 0 && p.primaries.temperature < 0, qPrintable(QStringLiteral("%1 %2").arg(p.primaries.exposure).arg(p.primaries.temperature)));
  }

  void matchToADifferentSceneIsFlagged() {
    // a bright warm frame matched onto a nearly black, blue one fits the statistics only with extreme values
    std::vector<std::array<int, 3>> day, night;
    for (int i = 0; i < 600; ++i) {
      day.push_back({std::min(255, 140 + i % 100), 120 + i % 80, 60 + i % 50});
      night.push_back({4 + i % 6, 6 + i % 8, 20 + i % 30});
    }
    const Proposal p = proposeMatch(fromDisplay8(day), analyze(fromDisplay8(night)));
    QVERIFY(!p.plausible);
    QVERIFY(p.notes.join(QLatin1Char(' ')).contains(QStringLiteral("Not a natural match")));
    // a mild grade of the same picture stays plausible
    PrimaryGrade mild;
    mild.exposure = -0.5;
    mild.temperature = 0.2;
    QVERIFY(proposeMatch(gradePixels(testImage(), mild), analyze(testImage())).plausible);
  }

  void checkFlagsACrushingGrade() {
    std::vector<double> levels;
    for (int i = 0; i < 400; ++i) levels.push_back(0.004 + 0.12 * i / 399.0); // dark content
    const PixelSet dark = fromLinearGrey(levels);
    PrimaryGrade crush;
    crush.liftMaster = -0.1;
    crush.contrast = 1.8;
    const ColourMetrics before = analyze(dark), after = analyze(gradePixels(dark, crush));
    QVERIFY(after.crushedPct.all > before.crushedPct.all + 5);
    const auto v = checkColour(before, after);
    bool found = false;
    for (const Violation& x : v) found = found || x.code == QStringLiteral("blacksCrushedIncrease");
    QVERIFY(found);
    QVERIFY(checkColour(before, before).empty());
    // thresholds are parameters: generous limits let it pass
    CheckThresholds loose;
    loose.blacksCrushedIncrease = 100;
    loose.exposureShiftStops = 100;
    loose.highlightsClippedIncrease = 100;
    QVERIFY(checkColour(before, after, loose).empty());
    // the JSON route colour.check uses keeps what it needs
    const ColourMetrics b2 = ColourMetrics::fromJson(before.toJson()), a2 = ColourMetrics::fromJson(after.toJson());
    QVERIFY(!checkColour(b2, a2).empty());
  }

  void castOnBeforeMaskIgnoresReselection() {
    // a warm-tinted lattice: grading changes which pixels count as near-neutral, so the cast must be read on the BEFORE selection
    std::vector<float> lin;
    const PixelSet base = testImage();
    for (size_t i = 0; i < base.size(); ++i) lin.insert(lin.end(), {base.lin[i * 3] * 1.12f, base.lin[i * 3 + 1], base.lin[i * 3 + 2] * 0.9f});
    const PixelSet src = pixelsFromLinear(lin);
    const std::vector<unsigned char> mask = neutralMask(src);
    const ColourMetrics before = analyze(src, &mask);
    QVERIFY(before.neutralPct > 0.5);
    QVERIFY(std::fabs(before.castTemperature - analyze(src).castTemperature) < 1e-9); // default selection == its own mask
    PrimaryGrade cs;
    cs.contrast = 1.05;
    cs.saturation = 1.04;
    const PixelSet graded = gradePixels(src, cs);
    const ColourMetrics same = analyze(graded, &mask);
    QVERIFY2(std::fabs(same.castTemperature - before.castTemperature) < 0.03, qPrintable(QStringLiteral("%1 -> %2").arg(before.castTemperature).arg(same.castTemperature)));
    QVERIFY(std::fabs(same.castTint - before.castTint) < 0.03);
    PrimaryGrade warm;
    warm.temperature = 0.14;
    const double shift = analyze(gradePixels(src, warm), &mask).castTemperature - before.castTemperature;
    QVERIFY2(std::fabs(shift - 0.14) < 0.03, qPrintable(QString::number(shift)));
    // frames API: masks per frame, pooled
    const std::vector<std::vector<unsigned char>> masks{mask, mask};
    const ColourMetrics pooled = analyzeFrames({graded, graded}, &masks);
    QVERIFY(std::fabs(pooled.castTemperature - same.castTemperature) < 1e-6);
  }

  void multiFrameAggregationKeepsRange() {
    const ColourMetrics m = analyzeFrames({fromLinearGrey(std::vector<double>(100, 0.09)), fromLinearGrey(std::vector<double>(100, 0.36))});
    QCOMPARE(m.frames, 2);
    QVERIFY(std::fabs(m.exposureMin + 1.0) < 1e-3 && std::fabs(m.exposureMax - 1.0) < 1e-3);
    QCOMPARE(m.pixels, qint64(200));
  }
};

QTEST_GUILESS_MAIN(TstColourAnalysis)
#include "tst_colour_analysis.moc"
