#include "audio/loudness.h"
#include "audio_testutil.h"

#include <QTest>

using namespace sf;
using namespace sf::audio;
using namespace sf::test;

namespace {

// 1 kHz stereo sine at `dbfs` for `seconds`, appended to v
void addTone(std::vector<float>& v, double dbfs, double seconds, double freq = 1000) {
  const std::int64_t n = static_cast<std::int64_t>(seconds * 48000), base = static_cast<std::int64_t>(v.size() / 2);
  const double amp = std::pow(10.0, dbfs / 20.0);
  v.resize(v.size() + static_cast<size_t>(n) * 2);
  for (std::int64_t i = 0; i < n; ++i) v[static_cast<size_t>(base + i) * 2] = v[static_cast<size_t>(base + i) * 2 + 1] = sineAt(freq, amp, i);
}

} // namespace

class TstLoudness : public QObject {
  Q_OBJECT
private slots:
  void sineAtMinus23IsMinus23Lufs() {
    std::vector<float> v;
    addTone(v, -23.0, 20);
    const auto r = measureLoudness(v.data(), 48000 * 20);
    QVERIFY2(std::fabs(r.integrated + 23.0) < 0.1, qPrintable(QString::number(r.integrated)));
    LoudnessMeter m;
    m.process(v.data(), 48000 * 20);
    QVERIFY(std::fabs(m.momentary() + 23.0) < 0.1);
    QVERIFY(std::fabs(m.shortTerm() + 23.0) < 0.1);
    QVERIFY(std::fabs(r.samplePeakDb + 23.0) < 0.1);
  }

  void levelsTrackAmplitude() {
    for (const double db : {-33.0, -18.0, -3.0}) {
      std::vector<float> v;
      addTone(v, db, 20);
      const auto r = measureLoudness(v.data(), 48000 * 20);
      QVERIFY2(std::fabs(r.integrated - db) < 0.1, qPrintable(QStringLiteral("%1 -> %2").arg(db).arg(r.integrated)));
    }
  }

  void chunkingDoesNotMatter() {
    std::vector<float> v;
    addTone(v, -20.0, 10);
    LoudnessMeter a, b;
    a.process(v.data(), 480000);
    for (std::int64_t at = 0; at < 480000; at += 777) b.process(v.data() + at * 2, std::min<std::int64_t>(777, 480000 - at));
    QCOMPARE(a.integrated(), b.integrated());
    QCOMPARE(a.truePeakDb(), b.truePeakDb());
  }

  void gatesExcludeQuietAndSilentParts() {
    // EBU 3341 style: a loud section, a section more than 10 LU below it, and digital silence
    std::vector<float> v;
    addTone(v, -20.0, 20);
    addTone(v, -45.0, 20); // below the relative gate
    v.resize(v.size() + 48000 * 2 * 10, 0.0f); // silence: below the absolute gate
    const auto r = measureLoudness(v.data(), static_cast<std::int64_t>(v.size() / 2));
    QVERIFY2(std::fabs(r.integrated + 20.0) < 0.15, qPrintable(QString::number(r.integrated)));

    // a section within 10 LU is kept: energy mean of -20 and -26
    std::vector<float> w;
    addTone(w, -20.0, 20);
    addTone(w, -26.0, 20);
    const double expected = 10.0 * std::log10((std::pow(10.0, -2.0) + std::pow(10.0, -2.6)) / 2.0);
    const auto rw = measureLoudness(w.data(), static_cast<std::int64_t>(w.size() / 2));
    QVERIFY2(std::fabs(rw.integrated - expected) < 0.15, qPrintable(QString::number(rw.integrated)));
  }

  void silenceIsSilence() {
    std::vector<float> v(48000 * 2 * 5, 0.0f);
    const auto r = measureLoudness(v.data(), 48000 * 5);
    QVERIFY(!std::isfinite(r.integrated));
    QVERIFY(!std::isfinite(r.truePeakDb));
    QCOMPARE(normalizeGainDb(r, -14.0), 0.0);
  }

  void loudnessRange() {
    std::vector<float> v;
    addTone(v, -20.0, 30);
    addTone(v, -30.0, 30);
    const auto r = measureLoudness(v.data(), static_cast<std::int64_t>(v.size() / 2));
    QVERIFY2(std::fabs(r.range - 10.0) < 1.0, qPrintable(QString::number(r.range)));
    std::vector<float> flat;
    addTone(flat, -23.0, 30);
    QVERIFY(measureLoudness(flat.data(), 48000 * 30).range < 0.1);
  }

  void truePeakSeesIntersamplePeaks() {
    // fs/4 at 45 degrees: every sample is +-0.3536 but the waveform reaches 0.5 (-6.02 dBTP)
    std::vector<float> v(48000 * 2 * 2);
    for (std::int64_t i = 0; i < 96000; ++i) v[static_cast<size_t>(i) * 2] = v[static_cast<size_t>(i) * 2 + 1] = sineAt(12000, 0.5, i, 3.14159265358979 / 4.0);
    const auto r = measureLoudness(v.data(), 96000);
    QVERIFY2(std::fabs(r.samplePeakDb + 9.03) < 0.1, qPrintable(QString::number(r.samplePeakDb)));
    QVERIFY2(std::fabs(r.truePeakDb + 6.02) < 0.2, qPrintable(QString::number(r.truePeakDb)));
    // full-scale 997 Hz: 0 dBTP
    std::vector<float> f;
    addTone(f, 0.0, 2, 997);
    QVERIFY(std::fabs(measureLoudness(f.data(), 96000).truePeakDb) < 0.1);
  }

  void normaliseGain() {
    std::vector<float> v;
    addTone(v, -23.0, 20);
    const auto r = measureLoudness(v.data(), 48000 * 20);
    QVERIFY(std::fabs(normalizeGainDb(r, -14.0) - 9.0) < 0.1);
    QVERIFY(std::fabs(normalizeGainDb(r, -16.0) - 7.0) < 0.1);
    QVERIFY(std::fabs(normalizeGainDb(r, -23.0)) < 0.1);
    // true peak ceiling limits the boost: peak is -23 dBFS, ceiling -20 -> at most +3 dB
    QVERIFY(std::fabs(normalizeGainDb(r, -14.0, -20.0) - 3.0) < 0.1);

    // applying the gain lands on the target
    const float g = static_cast<float>(std::pow(10.0, normalizeGainDb(r, -14.0) / 20.0));
    for (float& s : v) s *= g;
    QVERIFY(std::fabs(measureLoudness(v.data(), 48000 * 20).integrated + 14.0) < 0.1);
  }
};

QTEST_GUILESS_MAIN(TstLoudness)
#include "tst_loudness.moc"
