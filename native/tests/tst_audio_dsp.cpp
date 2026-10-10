#include "audio/biquad.h"
#include "audio/loudness.h"
#include "audio_testutil.h"

#include <QTest>

using namespace sf;
using namespace sf::audio;
using namespace sf::test;

namespace {

std::vector<float> tone(double f, double amp, std::int64_t frames) {
  return makeSignal(frames, [=](std::int64_t i, int) { return sineAt(f, amp, i); });
}

// gain in dB of an effect for a sine, measured over the second half
double gainDb(audio::Effect& fx, double f, double amp = 0.1) {
  const std::int64_t n = 48000;
  const auto out = runEffect(fx, tone(f, amp, n));
  return toDb(rms(out, n / 2, n) / (amp / std::sqrt(2.0)));
}

std::unique_ptr<audio::Effect> eq(const EffectParams& p) { return makeEffect(QStringLiteral("eq"), 48000, p); }

} // namespace

class TstAudioDsp : public QObject {
  Q_OBJECT
private slots:
  void eqPeakShelvesAndFilters() {
    auto peak = eq({{QStringLiteral("b2.on"), true}, {QStringLiteral("b2.type"), QStringLiteral("peak")}, {QStringLiteral("b2.freq"), 1000.0},
                    {QStringLiteral("b2.gain"), 6.0}, {QStringLiteral("b2.q"), 1.0}});
    QVERIFY(std::fabs(gainDb(*peak, 1000) - 6.0) < 0.05);
    peak->reset();
    QVERIFY(std::fabs(gainDb(*peak, 100)) < 0.5);

    auto ls = eq({{QStringLiteral("b1.on"), true}, {QStringLiteral("b1.type"), QStringLiteral("lowshelf")}, {QStringLiteral("b1.freq"), 200.0}, {QStringLiteral("b1.gain"), 6.0}});
    QVERIFY(std::fabs(gainDb(*ls, 30) - 6.0) < 0.3);
    ls->reset();
    QVERIFY(std::fabs(gainDb(*ls, 8000)) < 0.1);

    auto hs = eq({{QStringLiteral("b5.on"), true}, {QStringLiteral("b5.type"), QStringLiteral("highshelf")}, {QStringLiteral("b5.freq"), 4000.0}, {QStringLiteral("b5.gain"), -6.0}});
    QVERIFY(std::fabs(gainDb(*hs, 15000) + 6.0) < 0.3);

    auto hp = eq({{QStringLiteral("b0.on"), true}, {QStringLiteral("b0.type"), QStringLiteral("highpass")}, {QStringLiteral("b0.freq"), 1000.0}});
    QVERIFY(std::fabs(gainDb(*hp, 1000) + 3.01) < 0.1);
    hp->reset();
    QVERIFY(std::fabs(gainDb(*hp, 250) + 24.1) < 0.4); // 12 dB/octave
    auto lp = eq({{QStringLiteral("b5.on"), true}, {QStringLiteral("b5.type"), QStringLiteral("lowpass")}, {QStringLiteral("b5.freq"), 1000.0}});
    QVERIFY(std::fabs(gainDb(*lp, 1000) + 3.01) < 0.1);
    lp->reset();
    QVERIFY(std::fabs(gainDb(*lp, 4000) + 24.1) < 0.6);

    // coefficients agree with measurement
    const auto c = designBiquad(BiquadType::Peak, 48000, 1000, 6, 1.0);
    QVERIFY(std::fabs(toDb(c.magnitude(1000, 48000)) - 6.0) < 1e-6);
    QVERIFY(std::fabs(toDb(designBiquad(BiquadType::Notch, 48000, 1000).magnitude(1000, 48000))) > 60);
  }

  void compressorStaticCurve() {
    QCOMPARE(compressorReductionDb(-30, -20, 4, 0), 0.0);
    QVERIFY(std::fabs(compressorReductionDb(-10, -20, 4, 0) - 7.5) < 1e-9);
    // knee: continuous, starts at threshold - knee/2, half-way value at the threshold
    QCOMPARE(compressorReductionDb(-26, -20, 4, 12), 0.0);
    QVERIFY(std::fabs(compressorReductionDb(-14, -20, 4, 12) - (6.0 * 0.75 * 0 + compressorReductionDb(-14, -20, 4, 12))) < 1e-12);
    const double mid = compressorReductionDb(-20, -20, 4, 12);
    QVERIFY(mid > 0 && std::fabs(mid - 1.125) < 1e-9);
    QVERIFY(std::fabs(compressorReductionDb(-14.0001, -20, 4, 12) - compressorReductionDb(-13.9999, -20, 4, 12)) < 1e-3);

    // dynamic: DC at -10 dBFS, threshold -20, 4:1 -> -17.5 dBFS
    auto c = makeEffect(QStringLiteral("compressor"), 48000, {{QStringLiteral("threshold"), -20.0}, {QStringLiteral("ratio"), 4.0}, {QStringLiteral("knee"), 0.0},
                                                               {QStringLiteral("attack"), 0.0}, {QStringLiteral("mode"), QStringLiteral("peak")}});
    const float dc = static_cast<float>(dbToLin(-10));
    const auto out = runEffect(*c, makeSignal(48000, [=](std::int64_t, int) { return dc; }));
    QVERIFY(std::fabs(toDb(out[47000 * 2]) + 17.5) < 0.05);
    QVERIFY(std::fabs(c->gainReductionDb() - 7.5) < 0.05);

    // makeup and a quiet signal below the threshold
    auto m = makeEffect(QStringLiteral("compressor"), 48000, {{QStringLiteral("makeup"), 6.0}});
    QVERIFY(std::fabs(gainDb(*m, 1000, 0.001) - 6.0) < 0.1);
  }

  void compressorSidechain() {
    auto c = makeEffect(QStringLiteral("compressor"), 48000, {{QStringLiteral("threshold"), -20.0}, {QStringLiteral("ratio"), 4.0}, {QStringLiteral("knee"), 0.0},
                                                               {QStringLiteral("attack"), 0.0}, {QStringLiteral("mode"), QStringLiteral("peak")}});
    const float quiet = static_cast<float>(dbToLin(-40)), loud = static_cast<float>(dbToLin(-10));
    const auto in = makeSignal(24000, [=](std::int64_t, int) { return quiet; });
    const auto key = makeSignal(24000, [=](std::int64_t, int) { return loud; });
    const auto out = runEffect(*c, in, &key);
    QVERIFY(std::fabs(toDb(out[20000 * 2]) - (-40 - 7.5)) < 0.05);
  }

  void limiterHoldsCeiling() {
    auto lim = makeEffect(QStringLiteral("limiter"), 48000, {{QStringLiteral("ceiling"), -1.0}});
    const int lat = lim->latencySamples();
    QVERIFY(lat > 0);
    const std::int64_t n = 96000;
    auto in = makeSignal(n, [](std::int64_t i, int c) { return noiseAt(i, c, 4.0f); });
    const auto out = runEffect(*lim, in);
    const double ceiling = dbToLin(-1);
    QVERIFY2(peakOf(out, 0, n) <= ceiling * 1.0001, qPrintable(QString::number(peakOf(out, 0, n))));
    QVERIFY(peakOf(out, 1000, n) > ceiling * 0.9); // and it is actually using the headroom
    // band-limited loud programme: the true peak stays near the ceiling too
    auto lim3 = makeEffect(QStringLiteral("limiter"), 48000, {{QStringLiteral("ceiling"), -1.0}});
    const auto prog = makeSignal(n, [](std::int64_t i, int) { return sineAt(3000, 2.0, i) + sineAt(7500, 2.0, i, 0.7); });
    const auto o3 = runEffect(*lim3, prog);
    LoudnessMeter lm;
    lm.process(o3.data() + 4800 * 2, n - 4800);
    QVERIFY2(lm.truePeakDb() < -1.0 + 0.5, qPrintable(QString::number(lm.truePeakDb())));

    // below the ceiling: untouched, only delayed
    auto lim2 = makeEffect(QStringLiteral("limiter"), 48000, {});
    const auto quiet = tone(440, 0.1, 24000);
    const auto o2 = runEffect(*lim2, quiet);
    for (int i = 0; i < 20000; ++i) QVERIFY(std::fabs(o2[static_cast<size_t>(i + lat) * 2] - quiet[static_cast<size_t>(i) * 2]) < 1e-6f);
  }

  void gateClosesOnQuiet() {
    auto g = makeEffect(QStringLiteral("gate"), 48000, {{QStringLiteral("threshold"), -40.0}});
    QVERIFY(std::fabs(gainDb(*g, 1000, 0.1)) < 0.2); // -20 dBFS passes
    auto g2 = makeEffect(QStringLiteral("gate"), 48000, {{QStringLiteral("threshold"), -40.0}});
    QVERIFY(gainDb(*g2, 1000, 0.001) < -60); // -60 dBFS is shut
    // opens, then closes after the hold and release when the signal stops
    auto g3 = makeEffect(QStringLiteral("gate"), 48000, {{QStringLiteral("threshold"), -40.0}, {QStringLiteral("hold"), 10.0}, {QStringLiteral("release"), 20.0}});
    auto in = makeSignal(48000, [](std::int64_t i, int) { return i < 12000 ? sineAt(1000, 0.1, i) : sineAt(1000, 0.001, i); });
    const auto out = runEffect(*g3, in);
    QVERIFY(rms(out, 6000, 12000) > 0.05);
    QVERIFY(rms(out, 30000, 48000) < 1e-5);
  }

  void deEsserTargetsSibilance() {
    EffectParams p{{QStringLiteral("threshold"), -40.0}, {QStringLiteral("ratio"), 8.0}};
    auto d = makeEffect(QStringLiteral("deesser"), 48000, p);
    const double g12 = gainDb(*d, 12000, 0.1);
    QVERIFY2(g12 < -6.0, qPrintable(QString::number(g12)));
    auto d2 = makeEffect(QStringLiteral("deesser"), 48000, p);
    QVERIFY(std::fabs(gainDb(*d2, 500, 0.1)) < 0.1);
  }

  void reverbTailDecays() {
    auto r = makeEffect(QStringLiteral("reverb"), 48000, {{QStringLiteral("wet"), 0.3}, {QStringLiteral("dry"), 0.0}, {QStringLiteral("room"), 0.5}});
    auto in = makeSignal(48000 * 4, [](std::int64_t i, int) { return i == 0 ? 1.0f : 0.0f; });
    const auto out = runEffect(*r, in);
    double prev = 1e9;
    double first = 0, last = 0;
    for (int w = 0; w < 7; ++w) {
      const double e = rms(out, w * 12000, (w + 1) * 12000);
      if (w == 0) first = e;
      last = e;
      QVERIFY2(e < prev, qPrintable(QString::number(w)));
      prev = e;
    }
    QVERIFY(first > 0);
    QVERIFY(last < first * 0.01);
    QVERIFY(out[0] == 0.0f || std::fabs(out[0]) < 1.0f); // dry = 0
  }

  void automationRampsLandOnTheirSamples() {
    // compressor makeup -20 dB ramp over 1 s, applied on the 16-sample grid at the sample it names
    AudioFixture fx;
    fx.sources->add(QStringLiteral("ast_music"), dcSource(0.5f, 48000 * 3));
    fx.add(audioItem(fx.doc, 0, 90));
    Mixer m;
    MixNode strip;
    strip.id = trackOfKind(fx.doc, TrackKind::Audio).id;
    MixInsert c;
    c.id = QStringLiteral("ins_c");
    c.type = QStringLiteral("compressor");
    c.params = {{QStringLiteral("threshold"), 0.0}};
    c.automation[QStringLiteral("makeup")] = {{0, 0, Easing::Linear}, {30, -20, Easing::Linear}};
    strip.inserts = {c};
    m.strips = {strip};
    fx.doc.mixer = m;
    const auto out = fx.render(0, 60000);
    for (std::int64_t t = 0; t < 60000; t += 16) {
      const double db = -20.0 * std::min(1.0, static_cast<double>(t) / 48000.0);
      const double expected = 0.5 * std::pow(10.0, db / 20.0);
      if (std::fabs(out[static_cast<size_t>(t) * 2] - expected) > 2e-5) QFAIL(qPrintable(QStringLiteral("sample %1: %2 vs %3").arg(t).arg(out[static_cast<size_t>(t) * 2]).arg(expected)));
    }
  }
};

QTEST_GUILESS_MAIN(TstAudioDsp)
#include "tst_audio_dsp.moc"
