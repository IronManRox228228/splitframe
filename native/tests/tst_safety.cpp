// safety.check maths on synthetic frames: flash detection (general / red / small area), stripe patterns, WCAG contrast, rectangles.
// No renderer: frames are float buffers.

#include "engine/safety_analysis.h"

#include <QTest>

using namespace sf::engine::safety;

namespace {
constexpr int W = 64, H = 36;

// A frame of `bg` with a white-ish `fg` rectangle covering the left fraction `frac` of the width (full height).
std::vector<float> frame(float bgR, float bgG, float bgB, float fgR, float fgG, float fgB, double frac) {
  std::vector<float> f(static_cast<size_t>(W * H * 3));
  const int cut = static_cast<int>(W * frac);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      float* p = &f[static_cast<size_t>(y * W + x) * 3];
      const bool in = x < cut;
      p[0] = in ? fgR : bgR;
      p[1] = in ? fgG : bgG;
      p[2] = in ? fgB : bgB;
    }
  return f;
}

// frames alternate between `on` and `off` every `period` frames, `n` frames in all
FlashDetector run(const std::vector<float>& on, const std::vector<float>& off, int period, int n) {
  FlashDetector d(W, H);
  for (int i = 0; i < n; ++i) d.push(((i / period) % 2 == 0 ? on : off).data(), i);
  return d;
}
} // namespace

class TstSafety : public QObject {
  Q_OBJECT
private slots:
  void fastWhiteBlackIsAViolation() {
    const FlashDetector d = run(frame(1, 1, 1, 1, 1, 1, 1), frame(0, 0, 0, 0, 0, 0, 1), 3, 60);
    const WindowSummary s = summarize(d.generalEvents(), 0, 60, 30, 3);
    QVERIFY2(s.worstFlashes > 3, qPrintable(QString::number(s.worstFlashes)));
    QVERIFY(!s.violating.empty());
    QCOMPARE(s.perSecond.size(), size_t(2));
    QVERIFY(s.perSecond[0] > 3);
  }
  void slowWhiteBlackIsFine() {
    const FlashDetector d = run(frame(1, 1, 1, 1, 1, 1, 1), frame(0, 0, 0, 0, 0, 0, 1), 15, 90);
    const WindowSummary s = summarize(d.generalEvents(), 0, 90, 30, 3);
    QVERIFY(s.violating.empty());
    QVERIFY(s.worstFlashes <= 1);
  }
  void exactlyThreeFlashesIsAllowed() { // period 5 at 30 fps: 6 changes a second = 3 flashes
    const FlashDetector d = run(frame(1, 1, 1, 1, 1, 1, 1), frame(0, 0, 0, 0, 0, 0, 1), 5, 90);
    QVERIFY(summarize(d.generalEvents(), 0, 90, 30, 3).violating.empty());
  }
  void redFlash() {
    const FlashDetector d = run(frame(1, 0, 0, 1, 0, 0, 1), frame(0, 0, 0, 0, 0, 0, 1), 3, 60);
    QVERIFY(summarize(d.redEvents(), 0, 60, 30, 3).worstFlashes > 3);
    // a white/black flash is no red flash
    const FlashDetector w = run(frame(1, 1, 1, 1, 1, 1, 1), frame(0, 0, 0, 0, 0, 0, 1), 3, 60);
    QVERIFY(w.redEvents().empty());
  }
  void smallAreaIsIgnored() { // 10 % of the frame flashing
    const FlashDetector d = run(frame(0, 0, 0, 1, 1, 1, 0.10), frame(0, 0, 0, 0, 0, 0, 0.10), 3, 60);
    QVERIFY(d.generalEvents().empty());
    QVERIFY(d.worstGeneralArea() > 0.09 && d.worstGeneralArea() < 0.111);
    // 30 % does count
    const FlashDetector big = run(frame(0, 0, 0, 1, 1, 1, 0.30), frame(0, 0, 0, 0, 0, 0, 0.30), 3, 60);
    QVERIFY(summarize(big.generalEvents(), 0, 60, 30, 3).worstFlashes > 3);
  }
  void brightStatesDoNotCount() { // 0.85 <-> 1.0: the darker state is not below 0.80 and the delta is under 10 %
    const FlashDetector d = run(frame(1, 1, 1, 1, 1, 1, 1), frame(0.92f, 0.92f, 0.92f, 0.92f, 0.92f, 0.92f, 1), 3, 60);
    QVERIFY(d.generalEvents().empty());
  }
  void stripes() {
    std::vector<float> striped(64);
    for (size_t i = 0; i < striped.size(); ++i) striped[i] = (i / 4) % 2 ? 1.f : 0.f;
    QVERIFY(stripePairs(striped, 0.2f) >= 5);
    std::vector<float> ramp(64);
    for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i) / 63.f;
    QCOMPARE(stripePairs(ramp, 0.2f), 0);
    QCOMPARE(stripePairs(std::vector<float>(64, 0.5f), 0.2f), 0);
    // a striped frame sets the pattern flag, a plain one does not
    FlashDetector d(W, H);
    std::vector<float> f(static_cast<size_t>(W * H * 3));
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
        for (int c = 0; c < 3; ++c) f[static_cast<size_t>(y * W + x) * 3 + static_cast<size_t>(c)] = (x / 3) % 2 ? 1.f : 0.f;
    for (int i = 0; i < 4; ++i) d.push(f.data(), i);
    QCOMPARE(runsOf(d.patternFrames(), 3).size(), size_t(1));
  }
  void contrast() {
    QVERIFY(std::abs(contrastRatio(1.0, 0.0) - 21.0) < 1e-9);
    QVERIFY(std::abs(contrastRatio(0.2, 0.2) - 1.0) < 1e-9);
    QVERIFY(contrastRatio(srgbToLinear(0.54), srgbToLinear(0.50)) < 1.2); // grey on grey
    QVERIFY(std::abs(srgbToLinear(1.0) - 1.0) < 1e-9);
    QCOMPARE(percentile({5.f, 1.f, 3.f, 2.f, 4.f}, 0.0), 1.0);
    QCOMPARE(percentile({5.f, 1.f, 3.f, 2.f, 4.f}, 1.0), 5.0);
    QCOMPARE(percentile({}, 0.5), 0.0);
  }
  void rectangles() {
    const Rect a{0.8, 0.4, 0.2, 0.2}, zone{0.84, 0.42, 0.16, 0.46};
    QVERIFY(std::abs(intersectArea(a, zone) - 0.16 * 0.18) < 1e-12);
    QCOMPARE(intersectArea(Rect{0, 0, 0.1, 0.1}, zone), 0.0);
    const Rect u = unite(Rect{0, 0, 0.1, 0.1}, Rect{0.5, 0.5, 0.1, 0.1});
    QVERIFY(std::abs(u.w - 0.6) < 1e-12 && std::abs(u.h - 0.6) < 1e-12);
  }
};

QTEST_APPLESS_MAIN(TstSafety)
#include "tst_safety.moc"
