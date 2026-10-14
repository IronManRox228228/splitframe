// Measured audio on synthetic signals: loudness of a known tone, tempo and beats of a click track, bar phase, sections, clipping,
// the energy gate, ducking keyframes and the speech-over-music figure. Pure functions, no engine.

#include "audio/audio_analysis.h"
#include "audio_testutil.h"

#include <QTest>

#include <functional>

using namespace sf;
using namespace sf::audio;
using namespace sf::test;

namespace {

constexpr int kSr = 48000;

using Buf = std::vector<float>;

Buf silence(double seconds) { return Buf(static_cast<size_t>(seconds * kSr) * 2, 0.0f); }

// 1 kHz-ish stereo sine at `dbfs`
Buf tone(double dbfs, double seconds, double freq = 1000) {
  Buf v = silence(seconds);
  const double amp = std::pow(10.0, dbfs / 20.0);
  for (size_t i = 0; i < v.size() / 2; ++i) v[i * 2] = v[i * 2 + 1] = sineAt(freq, amp, static_cast<std::int64_t>(i));
  return v;
}

// A drum-like hit: 3 kHz burst, 8 ms decay constant, starting at `at` seconds
void hit(Buf& v, double at, double amp) {
  const std::int64_t s0 = static_cast<std::int64_t>(at * kSr);
  for (std::int64_t i = 0; i < 2400; ++i) {
    const std::int64_t k = s0 + i;
    if (k < 0 || static_cast<size_t>(k) * 2 + 1 >= v.size()) break;
    const double t = static_cast<double>(i) / kSr;
    const float x = static_cast<float>(amp * std::exp(-t / 0.008) * std::sin(kTwoPi * 3000.0 * t));
    v[static_cast<size_t>(k) * 2] += x;
    v[static_cast<size_t>(k) * 2 + 1] += x;
  }
}

// `n` hits every 60/bpm seconds from `first`; every 4th hit starting at index `accentPhase` is `accent` times louder
Buf clicks(double bpm, double seconds, double first = 0.13, double amp = 0.5, int accentPhase = -1, double accent = 3.0) {
  Buf v = silence(seconds);
  const double period = 60.0 / bpm;
  int k = 0;
  for (double t = first; t < seconds - 0.1; t += period, ++k) hit(v, t, accentPhase >= 0 && k % 4 == accentPhase ? std::min(0.95, amp * accent) : amp);
  return v;
}

// Drum voices: a 60 Hz kick (low band), a noise clap and a 6 kHz hat (both well above 150 Hz)
void addAt(Buf& v, double at, std::int64_t len, const std::function<double(std::int64_t, double)>& f) {
  const std::int64_t s0 = static_cast<std::int64_t>(at * kSr);
  for (std::int64_t i = 0; i < len; ++i) {
    const std::int64_t k = s0 + i;
    if (k < 0 || static_cast<size_t>(k) * 2 + 1 >= v.size()) break;
    const float x = static_cast<float>(f(k, static_cast<double>(i) / kSr));
    v[static_cast<size_t>(k) * 2] += x;
    v[static_cast<size_t>(k) * 2 + 1] += x;
  }
}
void kick(Buf& v, double at, double amp) { addAt(v, at, 9600, [&](std::int64_t, double t) { return amp * std::exp(-t / 0.06) * std::sin(kTwoPi * 60.0 * t); }); }
void clap(Buf& v, double at, double amp) { addAt(v, at, 4800, [&](std::int64_t k, double t) { return amp * std::exp(-t / 0.02) * noiseAt(k, 0, 1.0f); }); }
void hat(Buf& v, double at, double amp) { addAt(v, at, 1200, [&](std::int64_t, double t) { return amp * std::exp(-t / 0.004) * std::sin(kTwoPi * 6000.0 * t); }); }

// deterministic noise bed
void addNoise(Buf& v, double amp, std::int64_t from, std::int64_t to) {
  for (std::int64_t i = from; i < to && static_cast<size_t>(i) * 2 + 1 < v.size(); ++i) {
    v[static_cast<size_t>(i) * 2] += noiseAt(i, 0, static_cast<float>(amp));
    v[static_cast<size_t>(i) * 2 + 1] += noiseAt(i, 1, static_cast<float>(amp));
  }
}

} // namespace

class TstAudioAnalysis : public QObject {
  Q_OBJECT
private slots:
  void sineHasItsLoudness() {
    for (const double db : {-30.0, -23.0, -14.0}) {
      const MixMeasurement m = measureMix(tone(db, 12).data(), kSr * 12);
      QVERIFY2(std::fabs(m.loudness.integrated - db) < 0.5, qPrintable(QStringLiteral("%1 -> %2").arg(db).arg(m.loudness.integrated)));
      QVERIFY2(std::fabs(m.loudness.samplePeakDb - db) < 0.1, qPrintable(QString::number(m.loudness.samplePeakDb)));
      QCOMPARE(m.clippedSamples, 0);
      QVERIFY2(std::fabs(m.noiseFloorDb - (db - 3.01)) < 0.3, qPrintable(QString::number(m.noiseFloorDb))); // RMS of a sine is 3 dB under its peak
    }
  }

  void shortTermCurveHasOneValuePerSecond() {
    const MixMeasurement m = measureMix(tone(-20, 8).data(), kSr * 8);
    QCOMPARE(m.shortTermLufs.size(), 8u);
    QVERIFY(!std::isfinite(m.shortTermLufs[0])); // 3 s are needed
    QVERIFY(!std::isfinite(m.shortTermLufs[1]));
    for (size_t i = 2; i < 8; ++i) QVERIFY2(std::fabs(m.shortTermLufs[i] + 20.0) < 0.5, qPrintable(QString::number(m.shortTermLufs[i])));
    QCOMPARE(m.seconds(), 8.0);
  }

  void chunkingDoesNotMatter() {
    Buf v = tone(-18, 6);
    for (size_t i = 100000; i < 110000; ++i) v[i] = 1.0f;
    MixAnalyzer a, b;
    a.process(v.data(), static_cast<std::int64_t>(v.size() / 2));
    for (std::int64_t at = 0; at < kSr * 6; at += 7777) b.process(v.data() + at * 2, std::min<std::int64_t>(7777, kSr * 6 - at));
    const MixMeasurement x = a.result(), y = b.result();
    QCOMPARE(x.loudness.integrated, y.loudness.integrated);
    QCOMPARE(x.clippedSamples, y.clippedSamples);
    QCOMPARE(x.clipRuns.size(), y.clipRuns.size());
    QCOMPARE(x.noiseFloorDb, y.noiseFloorDb);
    QCOMPARE(x.shortTermLufs.size(), y.shortTermLufs.size());
  }

  void clippingIsCountedInRuns() {
    Buf v = tone(-6, 4);
    QCOMPARE(measureMix(v.data(), kSr * 4).clippedSamples, 0);
    // a hot sine, hard-limited to full scale: clipped stretches around the peaks, 100 per second of 100 Hz... one run per cycle group
    Buf hot = tone(0, 4, 100);
    for (float& x : hot) x = std::clamp(x * 1.6f, -1.0f, 1.0f);
    const MixMeasurement m = measureMix(hot.data(), kSr * 4);
    QVERIFY2(m.clippedSamples > 1000, qPrintable(QString::number(m.clippedSamples)));
    QVERIFY(!m.clipRuns.empty());
    QVERIFY(m.clipRuns.size() <= kMaxClipRuns);
    QCOMPARE(m.clipRuns.front().startSample < 400, true); // the first peak is in the first cycle
    // an isolated click of full scale at 2 s
    Buf one = tone(-30, 4);
    one[static_cast<size_t>(2 * kSr) * 2] = 1.0f;
    const MixMeasurement c = measureMix(one.data(), kSr * 4);
    QCOMPARE(c.clippedSamples, 1);
    QCOMPARE(c.clipRuns.size(), 1u);
    QCOMPARE(c.clipRuns[0].startSample, static_cast<std::int64_t>(2 * kSr));
  }

  void noiseFloorTracksQuietParts() {
    Buf v = silence(10);
    addNoise(v, 0.001, 0, kSr * 10); // about -64 dBFS RMS (uniform noise: amp / sqrt(3))
    for (std::int64_t i = kSr * 3; i < kSr * 8; ++i) v[static_cast<size_t>(i) * 2] = v[static_cast<size_t>(i) * 2 + 1] = 0.3f * sineAt(300, 1, i);
    const MixMeasurement m = measureMix(v.data(), kSr * 10);
    QVERIFY2(m.noiseFloorDb < -60 && m.noiseFloorDb > -70, qPrintable(QString::number(m.noiseFloorDb)));
  }

  void clickTrackTempoAndBeatTimes() {
    for (const double bpm : {120.0, 100.0, 140.0, 90.0}) {
      const double first = 0.13;
      const Buf v = clicks(bpm, 40, first);
      const MusicStructure ms = analyseMusic(v.data(), static_cast<std::int64_t>(v.size() / 2));
      QVERIFY2(ms.valid, qPrintable(QString::fromStdString(ms.note)));
      QVERIFY2(std::fabs(ms.bpm - bpm) <= 1.0, qPrintable(QStringLiteral("%1 -> %2").arg(bpm).arg(ms.bpm)));
      QVERIFY(ms.tempoConfidence > 0.3);
      // every beat lands on a hit
      const double period = 60.0 / bpm;
      QVERIFY(ms.beats.size() >= 30);
      double worst = 0;
      for (const double t : ms.beats) {
        const double k = std::round((t - first) / period);
        if (k < 0) continue;
        worst = std::max(worst, std::fabs(t - (first + k * period)));
      }
      qInfo().noquote() << QStringLiteral("click track %1 BPM -> %2 BPM, confidence %3, worst beat error %4 ms").arg(bpm).arg(ms.bpm, 0, 'f', 2).arg(ms.tempoConfidence, 0, 'f', 2).arg(worst * 1000, 0, 'f', 1);
      QVERIFY2(worst < 0.025, qPrintable(QStringLiteral("%1 BPM: beats are off by up to %2 ms").arg(bpm).arg(worst * 1000)));
    }
  }

  void downbeatIsTheAccentedBeat() {
    for (const int phase : {0, 1, 2, 3}) {
      const Buf v = clicks(120, 40, 0.13, 0.25, phase);
      const MusicStructure ms = analyseMusic(v.data(), static_cast<std::int64_t>(v.size() / 2));
      QVERIFY(ms.valid);
      QVERIFY(ms.beats.size() >= 4 && ms.downbeats.size() >= 8);
      // the first hit is the first beat (index 0), so the accented hit k = phase is beat index `phase`
      QVERIFY2(std::fabs(ms.downbeats.front() - (0.13 + phase * 0.5)) < 0.03, qPrintable(QStringLiteral("phase %1: first downbeat at %2").arg(phase).arg(ms.downbeats.front())));
      QCOMPARE(ms.downbeatPhase, phase);
      QVERIFY(std::fabs(ms.downbeats[1] - ms.downbeats[0] - 2.0) < 0.03);
    }
  }

  void sectionsFollowEnergy() {
    // 8 bars quiet, 16 bars loud (extra noise bed), 8 bars quiet at 120 BPM: 64 s
    Buf v = silence(64);
    const double period = 0.5;
    int k = 0;
    for (double t = 0.13; t < 63.9; t += period, ++k) {
      const bool loud = t >= 16 && t < 48;
      hit(v, t, loud ? 0.8 : 0.1);
    }
    addNoise(v, 0.3, static_cast<std::int64_t>(16.13 * kSr), static_cast<std::int64_t>(48.13 * kSr));
    const MusicStructure ms = analyseMusic(v.data(), static_cast<std::int64_t>(v.size() / 2));
    QVERIFY(ms.valid);
    QVERIFY2(std::fabs(ms.bpm - 120) <= 1.0, qPrintable(QString::number(ms.bpm)));
    QCOMPARE(ms.sections.size(), 3u);
    QCOMPARE(ms.sections[0].label, std::string("intro"));
    QCOMPARE(ms.sections[1].label, std::string("drop"));
    QCOMPARE(ms.sections[2].label, std::string("outro"));
    QVERIFY2(std::fabs(ms.sections[1].startSec - 16.1) < 0.7 && std::fabs(ms.sections[1].endSec - 48.1) < 0.7,
             qPrintable(QStringLiteral("drop %1 .. %2 s").arg(ms.sections[1].startSec).arg(ms.sections[1].endSec)));
    QCOMPARE(ms.sections[0].startSec, 0.0);
    QCOMPARE(ms.sections[2].endSec, 64.0);
    QVERIFY(ms.sections[1].relEnergy > 0.99 && ms.sections[0].relEnergy < 0.4);
    qInfo().noquote() << QStringLiteral("sections: intro ends %1 s, drop %2 .. %3 s").arg(ms.sections[0].endSec).arg(ms.sections[1].startSec).arg(ms.sections[1].endSec);
  }

  void kickOnOneBeatsLouderClapOnThree() {
    for (const int phase : {0, 2}) {
      Buf v = silence(40);
      int k = 0;
      for (double t = 0.13; t < 39.8; t += 0.5, ++k) {
        if (k % 4 == phase) kick(v, t, 0.5);
        if (k % 4 == (phase + 2) % 4) {
          kick(v, t, 0.25); // a lighter kick under the louder clap
          clap(v, t, 0.9);
        }
        hat(v, t, 0.15);
        hat(v, t + 0.25, 0.1);
      }
      const MusicStructure ms = analyseMusic(v.data(), static_cast<std::int64_t>(v.size() / 2));
      QVERIFY(ms.valid);
      QVERIFY2(std::fabs(ms.bpm - 120) <= 1.0, qPrintable(QString::number(ms.bpm)));
      QVERIFY2(ms.downbeatPhase == phase && std::fabs(ms.downbeats.front() - (0.13 + phase * 0.5)) < 0.03,
               qPrintable(QStringLiteral("kick on beat %1 -> phase %2, first downbeat %3 s; %4").arg(phase).arg(ms.downbeatPhase).arg(ms.downbeats.front()).arg(QString::fromStdString(ms.downbeatEvidence))));
      qInfo().noquote() << QStringLiteral("kick on beat %1, louder clap on beat %2 -> downbeatPhase %3 (%4)").arg(phase).arg((phase + 2) % 4).arg(ms.downbeatPhase).arg(QString::fromStdString(ms.downbeatEvidence));
    }
  }

  void breakdownWithSameLevelButNoLowEnd() {
    // 120 BPM: 8 bars drop (kick, claps, hats), 8 bars breakdown (claps, hats, a pad at the same overall level, no kick or sub), 8 bars drop
    Buf v = silence(48.5);
    const auto section = [&](double from, double to, bool kicks) {
      int k = 0;
      for (double t = 0.13 + from; t < 0.13 + to - 0.01; t += 0.5, ++k) {
        if (kicks) kick(v, t, 0.6);
        if (k % 2 == 1) clap(v, t, 0.3);
        hat(v, t + 0.25, 0.15);
      }
    };
    section(0, 16, true);
    section(16, 32, false);
    section(32, 48, true);
    const auto energy = [&](double a, double b) {
      double s = 0;
      const std::int64_t i0 = static_cast<std::int64_t>(a * kSr), i1 = static_cast<std::int64_t>(b * kSr);
      for (std::int64_t i = i0; i < i1; ++i) s += static_cast<double>(v[static_cast<size_t>(i) * 2]) * v[static_cast<size_t>(i) * 2];
      return s / static_cast<double>(i1 - i0);
    };
    const double padRms = std::sqrt(std::max(0.0, energy(2, 14) - energy(18, 30)));
    for (std::int64_t i = 16 * kSr; i < 32 * kSr; ++i) v[static_cast<size_t>(i) * 2] = v[static_cast<size_t>(i) * 2 + 1] += static_cast<float>(padRms * 1.414 * std::sin(kTwoPi * 500.0 * static_cast<double>(i) / kSr));
    const double dDb = 10 * std::log10(energy(2, 14)), bDb = 10 * std::log10(energy(18, 30));
    QVERIFY2(std::fabs(dDb - bDb) < 1.0, qPrintable(QStringLiteral("premise: drop %1 dB, breakdown %2 dB").arg(dDb).arg(bDb)));
    const MusicStructure ms = analyseMusic(v.data(), static_cast<std::int64_t>(48.5 * kSr));
    QVERIFY(ms.valid);
    QVERIFY2(std::fabs(ms.bpm - 120) <= 1.0, qPrintable(QString::number(ms.bpm)));
    QString got;
    for (const MusicSection& x : ms.sections) got += QStringLiteral("%1[%2-%3] ").arg(QString::fromStdString(x.label)).arg(x.startSec).arg(x.endSec);
    QVERIFY2(ms.sections.size() == 3u, qPrintable(got));
    QCOMPARE(ms.sections[0].label, std::string("drop"));
    QCOMPARE(ms.sections[1].label, std::string("breakdown"));
    QCOMPARE(ms.sections[2].label, std::string("drop"));
    QVERIFY2(std::fabs(ms.sections[1].startSec - 16.0) <= 0.5 && std::fabs(ms.sections[1].endSec - 32.0) <= 0.5,
             qPrintable(QStringLiteral("breakdown %1 .. %2 s").arg(ms.sections[1].startSec).arg(ms.sections[1].endSec)));
    qInfo().noquote() << QStringLiteral("drop/breakdown/drop: full-band %1 vs %2 dB; sections %3 [%4-%5] %6 [%7-%8] %9; downbeatPhase %10")
                             .arg(dDb, 0, 'f', 1).arg(bDb, 0, 'f', 1).arg(QString::fromStdString(ms.sections[0].label)).arg(ms.sections[0].startSec).arg(ms.sections[0].endSec)
                             .arg(QString::fromStdString(ms.sections[1].label)).arg(ms.sections[1].startSec).arg(ms.sections[1].endSec).arg(QString::fromStdString(ms.sections[2].label)).arg(ms.downbeatPhase);
  }

  void breakdownThatKeepsItsBassIsStillABreakdown() {
    // 120 BPM: 8 bars drop (kick, loud claps, hats), 8 bars where the kick stays but claps and hats go (low band unchanged, full and high band well down), 8 bars drop
    Buf v = silence(48.5);
    int k = 0;
    for (double t = 0.13; t < 47.9; t += 0.5, ++k) {
      const bool keepsBass = t >= 16 && t < 32;
      kick(v, t, 0.3);
      if (!keepsBass) {
        clap(v, t, 0.9);
        hat(v, t + 0.25, 0.5);
      }
    }
    const MusicStructure ms = analyseMusic(v.data(), static_cast<std::int64_t>(48.5 * kSr));
    QVERIFY(ms.valid);
    QString got;
    for (const MusicSection& x : ms.sections) got += QStringLiteral("%1[%2-%3] ").arg(QString::fromStdString(x.label)).arg(x.startSec).arg(x.endSec);
    QVERIFY2(ms.sections.size() == 3u, qPrintable(got));
    QCOMPARE(ms.sections[0].label, std::string("drop"));
    QCOMPARE(ms.sections[1].label, std::string("breakdown")); // the low band is within 6 dB of the drops, so only the dip between peaks can say so
    QCOMPARE(ms.sections[2].label, std::string("drop"));
  }

  void weakBoundaryInsideAPhraseIsMerged() {
    // 120 BPM: 8 quiet bars, then 3 bars at medium level, then 6 dB louder: the 14 dB step is a real boundary, the 6 dB one 3 bars later is
    // weak and would leave a section under a phrase
    Buf v = silence(64.5);
    for (double t = 0.13; t < 63.9; t += 0.5) {
      const double amp = t < 16 ? 0.05 : t < 22 ? 0.25 : 0.5;
      kick(v, t, amp);
      hat(v, t + 0.25, amp * 0.5);
    }
    const MusicStructure ms = analyseMusic(v.data(), static_cast<std::int64_t>(64.5 * kSr));
    QVERIFY(ms.valid);
    QString got;
    for (const MusicSection& x : ms.sections) got += QStringLiteral("%1[%2-%3] ").arg(QString::fromStdString(x.label)).arg(x.startSec).arg(x.endSec);
    QVERIFY2(ms.sections.size() == 3u, qPrintable(got)); // intro, the body, and the partial last bar's outro
    for (size_t i = 1; i < ms.sections.size(); ++i) QVERIFY2(ms.sections[i].startSec <= 16.8 || ms.sections[i].startSec >= 60, qPrintable(got)); // nothing cut at 22 s
    QVERIFY2(std::fabs(ms.sections[1].startSec - 16.13) <= 0.7, qPrintable(got));
  }

  void finalHitIsTheLastStrongOnsetNotTheNextBar() {
    // 120 BPM: 16 bars of kick, a big hit on the bar line at 32.13 s that rings out as a decaying noise tail for 8 s
    Buf v = silence(40.5);
    for (double t = 0.13; t < 32.0; t += 0.5) {
      kick(v, t, std::fmod(t - 0.13, 2.0) < 0.01 ? 0.7 : 0.35); // accented bar lines, so the bar phase is not a guess
    }
    hit(v, 32.13, 0.95);
    kick(v, 32.13, 0.8);
    addAt(v, 32.13, static_cast<std::int64_t>(8 * kSr), [&](std::int64_t k, double t) { return 0.25 * std::exp(-t / 1.2) * noiseAt(k, 0, 1.0f); });
    const MusicStructure ms = analyseMusic(v.data(), static_cast<std::int64_t>(40.5 * kSr));
    QVERIFY(ms.valid);
    QVERIFY2(ms.finalHitSec > 0 && std::fabs(ms.finalHitSec - 32.13) <= 0.03, qPrintable(QString::number(ms.finalHitSec)));
    for (const MusicSection& x : ms.sections) QVERIFY2(x.startSec < 32.13 + 0.5, qPrintable(QString::number(x.startSec))); // no section starts a bar late
  }

  void pickupBarShiftsThePhraseGrid() {
    // 120 BPM, bar-accented kick (beat 0 loud). Bar 0 is a pickup (hats only), then drop bars 1-8, breakdown (no kick) 9-16, drop 17-24
    Buf v = silence(50.5);
    int k = 0;
    for (double t = 0.13; t < 50.1; t += 0.5, ++k) {
      const int bar = k / 4;
      const bool drop = bar >= 1 && !(bar >= 9 && bar < 17);
      if (drop) {
        kick(v, t, k % 4 == 0 ? 0.6 : 0.3);
        if (k % 2 == 1) clap(v, t, 0.3);
      }
      hat(v, t + 0.25, 0.15);
    }
    const MusicStructure ms = analyseMusic(v.data(), static_cast<std::int64_t>(v.size() / 2));
    QVERIFY(ms.valid);
    QString got;
    for (const MusicSection& x : ms.sections) got += QStringLiteral("%1[%2-%3] ").arg(QString::fromStdString(x.label)).arg(x.startSec).arg(x.endSec);
    QVERIFY2(ms.downbeatPhase == 0, qPrintable(QString::fromStdString(ms.downbeatEvidence)));
    QVERIFY2(ms.sections.size() == 4u, qPrintable(got));
    const double want[3] = {2.13, 18.13, 34.13};
    for (int i = 0; i < 3; ++i) QVERIFY2(std::fabs(ms.sections[static_cast<size_t>(i) + 1].startSec - want[i]) <= 0.5, qPrintable(got));
    QCOMPARE(ms.sections[1].label, std::string("drop"));
    QCOMPARE(ms.sections[2].label, std::string("breakdown"));
    QCOMPARE(ms.sections[3].label, std::string("drop"));
    qInfo().noquote() << "pickup: " << got << QString::fromStdString(ms.downbeatEvidence);
  }

  void nearTieKickLetsSectionChangesDecide() {
    // kicks on beats 0 and 2 only; the beat-2 kick is a touch stronger (inside the 1.15x tie band), but every section change is on a
    // beat-0 bar line: kick share alone would pick phase 2, the boundary votes must pick phase 0
    Buf v = silence(48.5);
    int k = 0;
    for (double t = 0.13; t < 48.1; t += 0.5, ++k) {
      const bool kicks = t < 16.1 || t >= 32.1;
      if (kicks && k % 4 == 0) kick(v, t, 0.46);
      if (kicks && k % 4 == 2) kick(v, t, 0.5);
      if (kicks && k % 2 == 1) clap(v, t, 0.3);
      hat(v, t + 0.25, 0.15);
    }
    const MusicStructure ms = analyseMusic(v.data(), static_cast<std::int64_t>(v.size() / 2));
    QVERIFY(ms.valid);
    const QString ev = QString::fromStdString(ms.downbeatEvidence);
    QVERIFY2(ev.contains("near-tied"), qPrintable(ev));
    QVERIFY2(ms.downbeatPhase == 0, qPrintable(ev));
    qInfo().noquote() << ev;
  }

  void steadyOrShortAudioIsNotInvented() {
    const Buf pure = tone(-20, 30, 440);
    const MusicStructure a = analyseMusic(pure.data(), kSr * 30);
    QVERIFY(!a.valid);
    const Buf shortClip = clicks(120, 3);
    QVERIFY(!analyseMusic(shortClip.data(), kSr * 3).valid);
    const Buf silent = silence(20);
    QVERIFY(!analyseMusic(silent.data(), kSr * 20).valid);
  }

  void mergeSpansJoinsSmallGaps() {
    const std::vector<Span> in{{100, 200}, {250, 300}, {1000, 1100}, {150, 220}, {5, 5}};
    QCOMPARE(mergeSpans(in, 60), (std::vector<Span>{{100, 300}, {1000, 1100}}));
    QCOMPARE(mergeSpans(in, 10), (std::vector<Span>{{100, 220}, {250, 300}, {1000, 1100}}));
    QVERIFY(mergeSpans({}, 10).empty());
  }

  void energyGateFindsSpeechLikeBursts() {
    // 0.6 s bursts at -20 dB every 1.5 s over a -60 dB noise floor
    Buf v = silence(8);
    addNoise(v, 0.002, 0, kSr * 8);
    std::vector<Span> truth;
    for (double t : {0.5, 2.0, 3.5, 5.0}) {
      const std::int64_t a = static_cast<std::int64_t>(t * kSr), b = a + static_cast<std::int64_t>(0.6 * kSr);
      for (std::int64_t i = a; i < b; ++i) v[static_cast<size_t>(i) * 2] = v[static_cast<size_t>(i) * 2 + 1] += 0.1f * sineAt(220, 1, i);
      truth.push_back({a, b});
    }
    EnergyGate g;
    g.process(v.data(), kSr * 8);
    const std::vector<Span> r = g.ranges();
    QCOMPARE(r.size(), truth.size());
    for (size_t i = 0; i < r.size(); ++i) {
      QVERIFY2(std::llabs(r[i].start - truth[i].start) <= kGateBlock && std::llabs(r[i].end - truth[i].end) <= kGateBlock, qPrintable(QStringLiteral("range %1: %2..%3").arg(i).arg(r[i].start).arg(r[i].end)));
    }
    // a steady tone is all speech-like above the floor, silence is none
    EnergyGate s, z;
    s.process(tone(-20, 2).data(), kSr * 2);
    QCOMPARE(s.ranges().size(), 1u);
    z.process(silence(2).data(), kSr * 2);
    QVERIFY(z.ranges().empty());
  }

  void speechOverMusicMargin() {
    EnergyMeter s, m;
    s.process(tone(-14, 2).data(), kSr * 2);
    m.process(tone(-23, 2, 220).data(), kSr * 2);
    QVERIFY(std::fabs(marginDb(s.db(), m.db()) - 9.0) < 0.05);
    EnergyMeter none;
    QCOMPARE(marginDb(s.db(), none.db()), std::numeric_limits<double>::infinity());
    QCOMPARE(marginDb(none.db(), m.db()), -std::numeric_limits<double>::infinity());
  }

  void duckKeyframesForRanges() {
    DuckParams p;
    p.depthDb = 12;
    p.attackMs = 100; // 3 frames at 30 fps
    p.releaseMs = 500; // 15 frames
    const double low = std::pow(10.0, -12.0 / 20.0);
    const auto k = duckKeyframes({{30, 60}, {90, 120}}, p, 30);
    QCOMPARE(k.size(), 8u);
    const std::int64_t frames[] = {27, 30, 60, 75, 87, 90, 120, 135};
    const double values[] = {1, low, low, 1, 1, low, low, 1};
    for (size_t i = 0; i < 8; ++i) {
      QCOMPARE(k[i].frame, frames[i]);
      QVERIFY(std::fabs(k[i].value - values[i]) < 1e-9);
    }
    // close ranges (the ramp back up would be cut short by the next attack): ducked straight through
    const auto b = duckKeyframes({{30, 60}, {66, 90}}, p, 30);
    QCOMPARE(b.size(), 4u);
    QCOMPARE(b[0].frame, 27);
    QCOMPARE(b[1].frame, 30);
    QCOMPARE(b[2].frame, 90);
    QCOMPARE(b[3].frame, 105);
    // speech from frame 0: no point before it; base gain is the fader the music already has
    p.baseGain = 0.5;
    const auto z = duckKeyframes({{0, 30}}, p, 30);
    QCOMPARE(z.size(), 3u);
    QCOMPARE(z[0].frame, 0);
    QVERIFY(std::fabs(z[0].value - 0.5 * low) < 1e-9);
    QVERIFY(std::fabs(z[2].value - 0.5) < 1e-9);
    // frames strictly increase; nothing to do for depth 0 or no ranges
    for (size_t i = 1; i < k.size(); ++i) QVERIFY(k[i].frame > k[i - 1].frame);
    p.depthDb = 0;
    QVERIFY(duckKeyframes({{30, 60}}, p, 30).empty());
    QVERIFY(duckKeyframes({}, DuckParams{}, 30).empty());
  }

  void duckingRaisesTheMeasuredMargin() {
    // speech tone and music tone at the same level; the keyframes applied to the music by hand give the wanted margin
    const Buf speech = tone(-20, 6, 300), music = tone(-20, 6, 200);
    const std::vector<Span> ranges{{30, 150}}; // frames at 30 fps: 1 s .. 5 s
    DuckParams p;
    p.depthDb = 9;
    p.attackMs = 100;
    p.releaseMs = 300;
    const auto keys = duckKeyframes(ranges, p, 30);
    Buf ducked = music;
    for (std::int64_t i = 0; i < kSr * 6; ++i) {
      const double pos = static_cast<double>(i) / kSr * 30.0;
      double g = keys.front().value;
      for (size_t j = 0; j + 1 < keys.size(); ++j)
        if (pos >= static_cast<double>(keys[j].frame) && pos <= static_cast<double>(keys[j + 1].frame)) {
          const double t = (pos - static_cast<double>(keys[j].frame)) / static_cast<double>(keys[j + 1].frame - keys[j].frame);
          g = keys[j].value + (keys[j + 1].value - keys[j].value) * t;
        }
      if (pos > static_cast<double>(keys.back().frame)) g = keys.back().value;
      ducked[static_cast<size_t>(i) * 2] *= static_cast<float>(g);
      ducked[static_cast<size_t>(i) * 2 + 1] *= static_cast<float>(g);
    }
    const auto level = [&](const Buf& b) {
      EnergyMeter e;
      e.process(b.data() + static_cast<size_t>(kSr) * 2, kSr * 4); // 1 s .. 5 s
      return e.db();
    };
    QVERIFY(std::fabs(marginDb(level(speech), level(music))) < 0.05);
    QVERIFY(std::fabs(marginDb(level(speech), level(ducked)) - 9.0) < 0.05);
  }
};

QTEST_GUILESS_MAIN(TstAudioAnalysis)
#include "tst_audio_analysis.moc"
