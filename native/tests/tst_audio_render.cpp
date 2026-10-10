#include "audio_testutil.h"
#include "media_testutil.h"

#include <QTemporaryDir>
#include <QTest>

#include <thread>

using namespace sf;
using namespace sf::audio;
using namespace sf::test;

namespace {

constexpr std::int64_t kHour = 48000LL * 3600;

AudioProps fades(Frame in, Frame out) {
  AudioProps p;
  p.fadeInFrames = in;
  p.fadeOutFrames = out;
  return p;
}

const QString kMusic = QStringLiteral("ast_music");

void setStripAutomation(TimelineDoc& doc, const QString& trackId, const QString& key, KeyframeList kfs) {
  if (!doc.mixer) doc.mixer = Mixer{};
  MixNode n;
  n.id = trackId;
  n.automation[key] = std::move(kfs);
  doc.mixer->strips.push_back(std::move(n));
}

} // namespace

class TstAudioRender : public QObject {
  Q_OBJECT

private slots:
  void timebaseIsExactAndDriftFree() {
    // 29 fps does not divide 48000: boundaries come from the absolute frame, never accumulate
    const std::int64_t fps = 29;
    QCOMPARE(frameToSample(0, fps), 0);
    std::int64_t prev = 0;
    for (Frame f = 1; f <= 29 * 7200; ++f) {
      const std::int64_t s = frameToSample(f, fps);
      const std::int64_t step = s - prev;
      if (step != 1655 && step != 1656) QFAIL(qPrintable(QStringLiteral("frame %1: step %2").arg(f).arg(step)));
      const auto ref = static_cast<std::int64_t>(std::floor(static_cast<long double>(f) * 48000.0L / 29.0L + 0.5L));
      if (s != ref) QFAIL(qPrintable(QStringLiteral("frame %1: %2 != %3").arg(f).arg(s).arg(ref)));
      prev = s;
    }
    QCOMPARE(frameToSample(29 * 7200, fps), kHour * 2);
    QCOMPARE(frameToSample(24 * 7200, 24), kHour * 2);
    QCOMPARE(frameToSample(30 * 3600 * 24, 30), 48000LL * 3600 * 24); // a day of 30 fps
    QCOMPARE(sampleToFrame(frameToSample(12345, 30), 30), 12345);
    QCOMPARE(frameToSample(-1, 30), -1600);
  }

  void placementSourceOffsetAndVolume() {
    AudioFixture fx;
    fx.sources->add(kMusic, sineSource(1000, 0.5, 48000LL * 10));
    // starts at 1 s, plays source from 0.5 s on, half volume
    fx.add(audioItem(fx.doc, 30, 60, [](ItemInit& i) {
      i.sourceInFrame = 15;
      i.volume = 0.5;
    }));
    const auto out = fx.render(0, 48000 * 4);
    for (std::int64_t t = 0; t < 48000 * 4; ++t) {
      float expected = 0;
      if (t >= 48000 && t < 48000 * 3) expected = sineAt(1000, 0.5, t - 48000 + 24000) * 0.5f;
      if (out[static_cast<size_t>(t) * 2] != expected || out[static_cast<size_t>(t) * 2 + 1] != expected)
        QFAIL(qPrintable(QStringLiteral("sample %1: %2 != %3").arg(t).arg(out[static_cast<size_t>(t) * 2]).arg(expected)));
    }
  }

  void linearFadesAndKeyframes() {
    AudioFixture fx;
    fx.sources->add(kMusic, dcSource(1.0f, 48000LL * 10));
    fx.add(audioItem(fx.doc, 0, 120, [](ItemInit& i) { i.props = fades(30, 30); })); // 4 s, 1 s fades
    const auto out = fx.render(0, 48000 * 4 + 10);
    for (std::int64_t t : {0LL, 1LL, 12000LL, 47999LL}) QVERIFY(std::fabs(out[static_cast<size_t>(t) * 2] - static_cast<float>(t) / 48000.0f) < 1e-6);
    QCOMPARE(out[static_cast<size_t>(60000) * 2], 1.0f);
    // fade out: gain at local sample L is (dur - L) / fadeSamples
    for (std::int64_t t : {48000LL * 3 + 1, 48000LL * 3 + 24000, 48000LL * 4 - 1}) {
      const float expected = static_cast<float>(48000 * 4 - t) / 48000.0f;
      QVERIFY(std::fabs(out[static_cast<size_t>(t) * 2] - expected) < 1e-6);
    }
    QCOMPARE(out[static_cast<size_t>(48000 * 4) * 2], 0.0f);

    // item-local volume keyframes: ramp 0 -> 1 over the first second of an item that starts at 0.5 s
    AudioFixture k;
    k.sources->add(kMusic, dcSource(1.0f, 48000LL * 10));
    k.add(audioItem(k.doc, 15, 90, [](ItemInit& i) { i.keyframes = KeyframeMap{{QStringLiteral("volume"), {{0, 0, Easing::Linear}, {30, 1, Easing::Linear}}}}; }));
    const auto o2 = k.render(0, 48000 * 3);
    QCOMPARE(o2[static_cast<size_t>(24000 - 1) * 2], 0.0f);
    QVERIFY(std::fabs(o2[static_cast<size_t>(24000 + 24000) * 2] - 0.5f) < 1e-6);
    QCOMPARE(o2[static_cast<size_t>(24000 + 48000 + 100) * 2], 1.0f);
  }

  void gainCeilingMatchesElectron() {
    AudioFixture fx;
    fx.sources->add(kMusic, dcSource(0.5f, 48000));
    fx.add(audioItem(fx.doc, 0, 30, [](ItemInit& i) { i.volume = 2.0; }));
    QCOMPARE(fx.render(100, 1)[0], 0.5f); // the export clamps the clip's volume to 1
    RenderOptions boost;
    boost.volumeCeiling = 4.0;
    QCOMPARE(fx.render(100, 1, boost)[0], 1.0f);
  }

  void speedResamples() {
    for (const double speed : {2.0, 0.5, 1.5}) {
      AudioFixture fx;
      fx.sources->add(kMusic, sineSource(440, 0.5, 48000LL * 20));
      fx.add(audioItem(fx.doc, 0, 30, [=](ItemInit& i) {
        i.speed = speed;
        i.sourceInFrame = 30;
      }));
      const auto out = fx.render(0, 48000);
      double worst = 0;
      for (std::int64_t t = 200; t < 48000 - 200; ++t) {
        const double expected = 0.5 * std::sin(kTwoPi * 440.0 * (48000.0 + static_cast<double>(t) * speed) / 48000.0);
        worst = std::max(worst, std::fabs(out[static_cast<size_t>(t) * 2] - expected));
      }
      QVERIFY2(worst < 2e-3, qPrintable(QStringLiteral("speed %1: worst error %2").arg(speed).arg(worst)));
    }
  }

  void speedFilterBandLimitsWhenReadingFast() {
    // 4x speed: a 10 kHz tone would fold down to 40 kHz -> aliases; the resampler must remove it first
    AudioFixture fx;
    fx.sources->add(kMusic, sineSource(10000, 0.5, 48000LL * 20));
    fx.add(audioItem(fx.doc, 0, 30, [](ItemInit& i) { i.speed = 4.0; }));
    const auto out = fx.render(0, 48000);
    QVERIFY(rms(out, 1000, 47000) < 0.01);
  }

  void timeRemapMatchesSpeed() {
    AudioFixture a, b;
    a.sources->add(kMusic, sineSource(440, 0.5, 48000LL * 20));
    b.sources = a.sources;
    a.add(audioItem(a.doc, 0, 30, [](ItemInit& i) { i.speed = 2.0; }));
    b.add(audioItem(b.doc, 0, 30, [](ItemInit& i) { i.timeRemap = std::vector<TimeRemapPoint>{{0, 0}, {30, 60}}; }));
    const auto x = a.render(0, 48000), y = b.render(0, 48000);
    double worst = 0;
    for (size_t i = 0; i < x.size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(x[i] - y[i])));
    QVERIFY2(worst < 1e-4, qPrintable(QString::number(worst)));
  }

  void clipsSum() {
    AudioFixture fx;
    fx.sources->add(kMusic, dcSource(0.25f, 48000 * 5));
    fx.sources->add(QStringLiteral("ast_video"), dcSource(0.5f, 48000 * 5));
    fx.add(audioItem(fx.doc, 0, 60));
    fx.add(videoItem(fx.doc, 30, 60)); // a video clip's own audio is part of the mix
    const auto out = fx.render(0, 48000 * 3);
    QCOMPARE(out[100 * 2], 0.25f);
    QCOMPARE(out[60000 * 2], 0.75f);
    QCOMPARE(out[100000 * 2], 0.5f);
  }

  void mutesAndMissingMedia() {
    AudioFixture fx;
    fx.sources->add(kMusic, dcSource(0.25f, 48000 * 5));
    const Item muted = audioItem(fx.doc, 0, 30, [](ItemInit& i) { i.muted = true; });
    const Item live = audioItem(fx.doc, 30, 30);
    const Item orphan = audioItem(fx.doc, 60, 30, [](ItemInit& i) { i.assetId = QStringLiteral("ast_gone"); });
    fx.add(muted);
    fx.add(live);
    fx.add(orphan);
    auto out = fx.render(0, 48000 * 3);
    QCOMPARE(out[1000 * 2], 0.0f);       // muted item
    QCOMPARE(out[48000 * 2 + 2], 0.25f); // live
    QCOMPARE(out[(48000 * 2 + 5) * 2], 0.0f); // asset without audio: silent, no failure

    const QString trackId = live.trackId;
    TimelineDoc muteTrack = applyOp(fx.doc, TrackUpdate{trackId, {.muted = true}}).doc;
    fx.doc = muteTrack;
    QCOMPARE(fx.render(48000 * 2 / 2 + 100, 1)[0], 0.0f);

    fx.doc = applyOp(fx.doc, TrackUpdate{trackId, {.muted = false, .hidden = true}}).doc;
    QCOMPARE(fx.render(48000 + 100, 1)[0], 0.25f); // the export keeps hidden tracks audible
    RenderOptions strict;
    strict.hiddenTracksSilent = true;
    QCOMPARE(fx.render(48000 + 100, 1, strict)[0], 0.0f);
  }

  void soloAndTrackFaders() {
    AudioFixture fx;
    fx.sources->add(kMusic, dcSource(0.25f, 48000 * 5));
    fx.sources->add(QStringLiteral("ast_two"), dcSource(0.5f, 48000 * 5));
    fx.doc = applyOp(fx.doc, TrackAdd{QStringLiteral("trk_a2"), TrackKind::Audio, QStringLiteral("Audio 2"), false, false, false, std::nullopt}).doc;
    fx.add(audioItem(fx.doc, 0, 60));
    fx.add(audioItem(fx.doc, 0, 60, [](ItemInit& i) {
      i.trackId = QStringLiteral("trk_a2");
      i.assetId = QStringLiteral("ast_two");
    }));
    QCOMPARE(fx.render(100, 1)[0], 0.75f);

    Mixer m;
    MixNode a2;
    a2.id = QStringLiteral("trk_a2");
    a2.solo = true;
    m.strips.push_back(a2);
    fx.doc.mixer = m;
    QCOMPARE(fx.render(100, 1)[0], 0.5f); // only the soloed strip

    fx.doc.mixer->strips[0].solo = false;
    fx.doc.mixer->strips[0].volume = 0.5;
    QCOMPARE(fx.render(100, 1)[0], 0.75f - 0.25f); // track volume (a2 at 0.5 of 0.5)
  }

  void longTimelineSampleAccuracy() {
    // 2 hours of 29 fps video: two clips butting at the last minute, one more at the very end
    AudioFixture fx;
    fx.doc.project.fps = 29;
    const Frame F = 29 * 7200;
    fx.sources->add(kMusic, dcSource(0.25f, kHour * 2 + 48000 * 10));
    fx.sources->add(QStringLiteral("ast_two"), dcSource(0.5f, kHour * 2 + 48000 * 10));
    fx.add(audioItem(fx.doc, 0, F - 1));
    fx.add(audioItem(fx.doc, F - 1, 100, [](ItemInit& i) { i.assetId = QStringLiteral("ast_two"); }));
    const std::int64_t boundary = frameToSample(F - 1, 29);
    QCOMPARE(boundary, static_cast<std::int64_t>(std::floor(static_cast<long double>(F - 1) * 48000.0L / 29.0L + 0.5L)));
    const auto out = fx.render(boundary - 8, 16);
    for (int i = 0; i < 8; ++i) QCOMPARE(out[static_cast<size_t>(i) * 2], 0.25f);
    for (int i = 8; i < 16; ++i) QCOMPARE(out[static_cast<size_t>(i) * 2], 0.5f);
    QCOMPARE(docDurationSamples(fx.doc), frameToSample(F - 1 + 100, 29));
    // the very first and last sample of the end clip
    const std::int64_t end = frameToSample(F + 99, 29);
    const auto tail = fx.render(end - 2, 4);
    QCOMPARE(tail[0], 0.5f);
    QCOMPARE(tail[2], 0.5f);
    QCOMPARE(tail[4], 0.0f);

    // a tone carried through two hours is still the same tone: absolute sample numbers, not accumulated steps
    AudioFixture tone;
    tone.doc.project.fps = 29;
    tone.sources->add(kMusic, sineSource(1000, 0.5, kHour * 2 + 48000 * 10));
    tone.add(audioItem(tone.doc, F - 100, 99, [](ItemInit& i) { i.sourceInFrame = F - 100; }));
    const std::int64_t s0 = frameToSample(F - 100, 29);
    const auto t = tone.render(s0 + 1000, 64);
    for (int i = 0; i < 64; ++i) QCOMPARE(t[static_cast<size_t>(i) * 2], sineAt(1000, 0.5, s0 + 1000 + i));
  }

  void independentOfBlockSizeAndChunking() {
    AudioFixture fx;
    fx.sources->add(kMusic, noiseSource(0.4f, 48000 * 20));
    fx.sources->add(QStringLiteral("ast_two"), sineSource(300, 0.5, 48000 * 20));
    fx.doc = applyOp(fx.doc, TrackAdd{QStringLiteral("trk_a2"), TrackKind::Audio, QStringLiteral("Audio 2"), false, false, false, std::nullopt}).doc;
    fx.add(audioItem(fx.doc, 3, 150, [](ItemInit& i) {
      i.speed = 1.25;
      i.props = fades(10, 20);
    }));
    fx.add(audioItem(fx.doc, 20, 100, [](ItemInit& i) {
      i.trackId = QStringLiteral("trk_a2");
      i.assetId = QStringLiteral("ast_two");
    }));
    Mixer m;
    MixNode strip;
    strip.id = QStringLiteral("trk_a2");
    strip.pan = 0.3;
    MixInsert eq;
    eq.id = QStringLiteral("ins_eq");
    eq.type = QStringLiteral("eq");
    eq.params = {{QStringLiteral("b2.on"), true}, {QStringLiteral("b2.gain"), 5.0}};
    MixInsert comp;
    comp.id = QStringLiteral("ins_comp");
    comp.type = QStringLiteral("compressor");
    comp.params = {{QStringLiteral("threshold"), -20.0}};
    comp.automation[QStringLiteral("makeup")] = {{0, 0, Easing::Linear}, {60, 6, Easing::Linear}};
    strip.inserts = {eq, comp};
    strip.automation[QStringLiteral("volume")] = {{0, 1, Easing::Linear}, {100, 0.5, Easing::Linear}};
    m.strips.push_back(strip);
    MixInsert rev;
    rev.id = QStringLiteral("ins_rev");
    rev.type = QStringLiteral("reverb");
    m.master.inserts = {rev};
    fx.doc.mixer = m;

    const std::int64_t N = 48000 * 5;
    RenderOptions o512, o64, o1000;
    o64.blockSize = 64;
    o1000.blockSize = 1000;
    const auto a = fx.render(0, N, o512);
    QVERIFY(fx.render(0, N, o512) == a);   // twice: same bits
    QVERIFY(fx.render(0, N, o64) == a);    // block size does not matter
    QVERIFY(fx.render(0, N, o1000) == a);

    // contiguous render() calls of odd sizes continue the state
    AudioEngine eng(std::make_shared<const TimelineDoc>(fx.doc), fx.sources);
    std::vector<float> chunked(a.size());
    std::int64_t at = 0;
    int k = 0;
    while (at < N) {
      const std::int64_t n = std::min<std::int64_t>(N - at, 1 + (k++ * 7919) % 3001);
      eng.render(at, n, chunked.data() + at * 2);
      at += n;
    }
    QVERIFY(chunked == a);
  }

  void threadedMatchesSingle() {
    AudioFixture fx;
    fx.sources->add(kMusic, noiseSource(0.3f, 48000 * 20));
    fx.add(audioItem(fx.doc, 0, 300));
    Mixer m;
    MixNode master;
    master.id = QStringLiteral("master");
    MixInsert lim;
    lim.id = QStringLiteral("ins_lim");
    lim.type = QStringLiteral("limiter");
    lim.params = {{QStringLiteral("ceiling"), -12.0}};
    master.inserts = {lim};
    m.master = master;
    fx.doc.mixer = m;
    const auto doc = std::make_shared<const TimelineDoc>(fx.doc);

    const std::int64_t starts[4] = {0, 48000, 123457, 480000};
    std::vector<float> single[4], multi[4];
    for (int i = 0; i < 4; ++i) single[i] = renderRange(doc, fx.sources, starts[i], 30000);
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
      threads.emplace_back([&, i] {
        for (int rep = 0; rep < 3; ++rep) multi[i] = renderRange(doc, fx.sources, starts[i], 30000);
      });
    for (auto& t : threads) t.join();
    for (int i = 0; i < 4; ++i) QVERIFY2(single[i] == multi[i], qPrintable(QString::number(i)));
  }

  void masterLatencyIsCompensated() {
    // a lookahead limiter on the master delays by its lookahead; the engine must hide that
    AudioFixture fx;
    fx.sources->add(kMusic, dcSource(0.1f, 48000 * 5));
    fx.add(audioItem(fx.doc, 3, 30)); // starts at sample 4800
    Mixer m;
    MixInsert lim;
    lim.id = QStringLiteral("ins_lim");
    lim.type = QStringLiteral("limiter");
    m.master.inserts = {lim};
    fx.doc.mixer = m;
    for (const std::int64_t start : {0LL, 1000LL, 4790LL}) {
      const auto out = fx.render(start, 40);
      for (std::int64_t t = start; t < start + 40; ++t) {
        const float expected = t >= 4800 ? 0.1f : 0.0f;
        if (std::fabs(out[static_cast<size_t>(t - start) * 2] - expected) > 1e-6f)
          QFAIL(qPrintable(QStringLiteral("start %1, sample %2: %3").arg(start).arg(t).arg(out[static_cast<size_t>(t - start) * 2])));
      }
    }
  }

  void volumeAutomationIsPerSample() {
    AudioFixture fx;
    fx.sources->add(kMusic, dcSource(1.0f, 48000 * 5));
    fx.add(audioItem(fx.doc, 0, 90));
    setStripAutomation(fx.doc, trackOfKind(fx.doc, TrackKind::Audio).id, QStringLiteral("volume"), {{0, 0, Easing::Linear}, {30, 1, Easing::Linear}});
    const auto out = fx.render(0, 60000);
    for (std::int64_t t = 0; t < 60000; ++t) {
      const double expected = std::min(1.0, static_cast<double>(t) / 48000.0);
      if (std::fabs(out[static_cast<size_t>(t) * 2] - expected) > 1e-6) QFAIL(qPrintable(QStringLiteral("sample %1").arg(t)));
    }
  }

  void panIsConstantPower() {
    for (const double p : {-1.0, -0.5, 0.0, 0.25, 1.0}) {
      const PanGains g = panGains(p);
      QVERIFY(std::fabs(g.l * g.l + g.r * g.r - 2.0) < 1e-6);
    }
    QCOMPARE(panGains(0).l, 1.0f);
    QCOMPARE(panGains(0).r, 1.0f);
    QCOMPARE(panGains(1).l, 0.0f + panGains(1).l); // hard right: left is silent
    QVERIFY(std::fabs(panGains(1).l) < 1e-6);
    QVERIFY(std::fabs(panGains(-1).r) < 1e-6);

    AudioFixture fx;
    fx.sources->add(kMusic, dcSource(0.5f, 48000));
    fx.add(audioItem(fx.doc, 0, 30));
    Mixer m;
    MixNode n;
    n.id = trackOfKind(fx.doc, TrackKind::Audio).id;
    n.pan = -1;
    m.strips.push_back(n);
    fx.doc.mixer = m;
    const auto out = fx.render(10, 1);
    QVERIFY(std::fabs(out[0] - 0.5f * std::sqrt(2.0f)) < 1e-6f);
    QVERIFY(std::fabs(out[1]) < 1e-6f);
  }

  void busesSendsAndSidechain() {
    const QString t1 = QStringLiteral("trk_a1"), t2 = QStringLiteral("trk_a2");
    AudioFixture fx;
    fx.doc = applyOp(fx.doc, TrackAdd{t1, TrackKind::Audio, QStringLiteral("A1"), false, false, false, std::nullopt}).doc;
    fx.doc = applyOp(fx.doc, TrackAdd{t2, TrackKind::Audio, QStringLiteral("A2"), false, false, false, std::nullopt}).doc;
    fx.sources->add(kMusic, dcSource(0.4f, 48000 * 5));
    fx.add(audioItem(fx.doc, 0, 60, [&](ItemInit& i) { i.trackId = t1; }));

    // strip -> bus (0.5) -> master (0.5)
    {
      Mixer m;
      MixNode strip;
      strip.id = t1;
      strip.output = QStringLiteral("bus1");
      MixNode bus;
      bus.id = QStringLiteral("bus1");
      bus.volume = 0.5;
      m.strips = {strip};
      m.buses = {bus};
      m.master.volume = 0.5;
      fx.doc.mixer = m;
      QVERIFY(std::fabs(fx.render(100, 1)[0] - 0.1f) < 1e-6f);
    }
    // sends: strip volume 0.5, an fx bus fed pre- or post-fader at 0.5
    for (const bool pre : {true, false}) {
      Mixer m;
      MixNode strip;
      strip.id = t1;
      strip.volume = 0.5;
      strip.sends = {{QStringLiteral("fx"), 0.5, pre}};
      MixNode bus;
      bus.id = QStringLiteral("fx");
      m.strips = {strip};
      m.buses = {bus};
      fx.doc.mixer = m;
      const float expected = 0.4f * (0.5f + (pre ? 0.5f : 0.25f));
      QVERIFY(std::fabs(fx.render(100, 1)[0] - expected) < 1e-6f);
    }
    // sidechain: A2 (-40 dB) compressed by A1 (-8 dB)
    {
      fx.sources->add(QStringLiteral("ast_quiet"), dcSource(0.01f, 48000 * 5));
      fx.add(audioItem(fx.doc, 0, 60, [&](ItemInit& i) {
        i.trackId = t2;
        i.assetId = QStringLiteral("ast_quiet");
      }));
      Mixer m;
      MixNode a2;
      a2.id = t2;
      MixInsert comp;
      comp.id = QStringLiteral("ins_c");
      comp.type = QStringLiteral("compressor");
      comp.sidechain = t1;
      comp.params = {{QStringLiteral("threshold"), -20.0}, {QStringLiteral("ratio"), 10.0}, {QStringLiteral("knee"), 0.0},
                     {QStringLiteral("attack"), 0.0},     {QStringLiteral("release"), 5.0}, {QStringLiteral("mode"), QStringLiteral("peak")}};
      a2.inserts = {comp};
      m.strips = {a2};
      fx.doc.mixer = m;
      AudioEngine eng(std::make_shared<const TimelineDoc>(fx.doc), fx.sources);
      std::vector<float> out(48000 * 2);
      eng.render(0, 48000, out.data());
      const auto meters = eng.meters();
      const double keyDb = toDb(0.4);
      const double reduction = (keyDb + 20.0) * (1.0 - 1.0 / 10.0);
      const double expected = 0.01 * std::pow(10.0, -reduction / 20.0);
      QVERIFY2(std::fabs(meters.at(t2).peak[0] - expected) < expected * 0.02, qPrintable(QString::number(meters.at(t2).peak[0])));
      QVERIFY(meters.at(t2).gainReductionDb > 10.0);

      // without the sidechain the quiet track alone is below the threshold: untouched
      fx.doc.mixer->strips[0].inserts[0].sidechain.reset();
      AudioEngine eng2(std::make_shared<const TimelineDoc>(fx.doc), fx.sources);
      eng2.render(0, 48000, out.data());
      QVERIFY(std::fabs(eng2.meters().at(t2).peak[0] - 0.01f) < 1e-6f);
    }
  }

  void metersAndLoudness() {
    AudioFixture fx;
    fx.sources->add(kMusic, sineSource(1000, 0.5, 48000 * 30));
    fx.add(audioItem(fx.doc, 0, 600));
    AudioEngine eng(std::make_shared<const TimelineDoc>(fx.doc), fx.sources);
    eng.enableLoudness(true);
    std::vector<float> out(48000 * 8 * 2);
    eng.render(0, 48000 * 8, out.data());
    const auto m = eng.meters().at(QStringLiteral("master"));
    QVERIFY(std::fabs(m.peak[0] - 0.5f) < 0.01f);
    QVERIFY(std::fabs(m.rms[0] - 0.5f / std::sqrt(2.0f)) < 0.01f);
    const auto l = eng.loudness();
    // 0.5 amplitude sine on both channels = -6.02 dBFS: about -6.0 LUFS
    QVERIFY2(std::fabs(l.momentary - (-6.02)) < 0.3, qPrintable(QString::number(l.momentary)));
    QVERIFY(std::fabs(l.integrated - l.momentary) < 0.1);
  }

  void decodesRealFiles() {
    if (sf::test::ffmpegExe().isEmpty()) QSKIP("SF_FFMPEG_EXE not set");
    QTemporaryDir dir;
    const QString wav = dir.filePath(QStringLiteral("tone.wav"));
    QString log;
    QVERIFY2(runFfmpeg({"-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=48000:duration=6", "-ac", "2", "-c:a", "pcm_s16le", wav}, &log), qPrintable(log));
    auto files = std::make_shared<FileProvider>();
    files->setFiles({{QStringLiteral("ast_wav"), {wav, true}}, {QStringLiteral("ast_mute"), {wav, false}}});
    AudioFixture fx;
    const auto doc = std::make_shared<TimelineDoc>(fx.doc);
    ItemInit init;
    init.id = newId(QStringLiteral("itm"));
    init.trackId = trackOfKind(*doc, TrackKind::Audio).id;
    init.startFrame = 15; // 0.5 s
    init.durationFrames = 60;
    init.assetId = QStringLiteral("ast_wav");
    init.sourceInFrame = 30; // 1 s into the file
    *doc = docWith(*doc, {createItem(ItemType::Audio, init)});
    const auto out = renderRange(doc, files, 0, 48000 * 3);
    QVERIFY(peakOf(out, 0, 20000) == 0.0);
    // the engine delivers exactly the decoder's samples, 1 s into the file, 0.5 s into the timeline
    auto src = files->source(QStringLiteral("ast_wav"));
    QVERIFY(src);
    std::vector<float> ref(static_cast<size_t>(40000) * 2);
    src->read(48000 + 1000, 40000, ref.data());
    double worst = 0;
    for (std::int64_t i = 0; i < 40000; ++i) worst = std::max(worst, static_cast<double>(std::fabs(out[static_cast<size_t>(24000 + 1000 + i) * 2] - ref[static_cast<size_t>(i) * 2])));
    QVERIFY2(worst < 1e-7, qPrintable(QString::number(worst)));
    QVERIFY(rms(ref, 0, 40000) > 0.05); // and it is a real tone
    // assets flagged without audio never open
    init.assetId = QStringLiteral("ast_mute");
    QVERIFY(files->source(QStringLiteral("ast_mute")) == nullptr);
  }
};

QTEST_GUILESS_MAIN(TstAudioRender)
#include "tst_audio_render.moc"
