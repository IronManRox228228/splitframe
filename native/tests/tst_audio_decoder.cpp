#include "media/audio_decoder.h"
#include "media/waveform.h"
#include "media_testutil.h"

#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>

using namespace sf;

// Test signal: L = ramp, R = -ramp with ramp[n] = (n mod 32768) / 32768 at 48 kHz, stored as 16-bit
// PCM. The value at every sample is its own position, exactly representable, so "sample-accurate"
// can be checked with plain equality instead of correlation.
class TstAudioDecoder : public QObject {
  Q_OBJECT
  QTemporaryDir dir;
  QString ramp;     // stereo 48 kHz, 3 s
  QString rampMono; // mono 48 kHz, 1 s
  QString ramp44;   // stereo 44.1 kHz, 2 s
  QString aac;      // 1 kHz sine at amplitude 0.5, 3 s, AAC in mp4

  static float expectedRamp(qint64 n) { return static_cast<float>(((n % 32768) + 32768) % 32768) / 32768.0f; }

  void generate(const QString& out, const QString& source, const QStringList& codec) {
    QString log;
    QVERIFY2(sf::test::runFfmpeg(QStringList{"-f", "lavfi", "-i", source} + codec + QStringList{out}, &log), qPrintable(log));
  }

  // Compares a read against the formula; reports the first bad sample.
  void checkRamp(AudioDecoder& dec, qint64 start, qint64 count) {
    const std::vector<float> got = dec.read(start, count);
    for (qint64 i = 0; i < count; ++i) {
      const qint64 n = start + i;
      const bool inside = n >= 0 && n < dec.info().totalSamples;
      const float l = inside ? expectedRamp(n) : 0.0f;
      const float r = -l;
      const float gl = got[static_cast<size_t>(i) * 2];
      const float gr = got[static_cast<size_t>(i) * 2 + 1];
      if (std::abs(gl - l) > 1e-6f || std::abs(gr - r) > 1e-6f) {
        QFAIL(qPrintable(QStringLiteral("read(%1,%2): sample %3 (pos %4) is (%5,%6), expected (%7,%8)")
                             .arg(start).arg(count).arg(i).arg(n).arg(gl * 32768).arg(gr * 32768).arg(l * 32768).arg(r * 32768)));
      }
    }
  }

private slots:
  void initTestCase() {
    QVERIFY(dir.isValid());
    QVERIFY2(!sf::test::ffmpegExe().isEmpty(), "SF_FFMPEG_EXE not set");
    ramp = dir.filePath(QStringLiteral("ramp.wav"));
    generate(ramp, QStringLiteral("aevalsrc='mod(n,32768)/32768|-mod(n,32768)/32768':s=48000:d=3"), {"-c:a", "pcm_s16le"});
    rampMono = dir.filePath(QStringLiteral("ramp_mono.wav"));
    generate(rampMono, QStringLiteral("aevalsrc='mod(n,32768)/32768':s=48000:d=1:c=mono"), {"-c:a", "pcm_s16le"});
    ramp44 = dir.filePath(QStringLiteral("ramp44.wav"));
    generate(ramp44, QStringLiteral("aevalsrc='mod(n,32768)/32768|mod(n,32768)/32768':s=44100:d=2"), {"-c:a", "pcm_s16le"});
    aac = dir.filePath(QStringLiteral("tone.mp4"));
    generate(aac, QStringLiteral("aevalsrc='0.5*sin(2*PI*1000*t)|0.5*sin(2*PI*1000*t)':s=48000:d=3"), {"-c:a", "aac", "-b:a", "192k"});
  }

  void infoDescribesTheStream() {
    QString err;
    auto dec = AudioDecoder::open(ramp, &err);
    QVERIFY2(dec, qPrintable(err));
    QCOMPARE(dec->info().sourceRate, 48000);
    QCOMPARE(dec->info().sourceChannels, 2);
    QCOMPARE(dec->info().totalSamples, 144000);
    QVERIFY(qAbs(dec->info().durationSec - 3.0) < 1e-6);
    QVERIFY(qAbs(dec->info().startSec) < 1e-6);
  }

  void sequentialReadIsExact() {
    auto dec = AudioDecoder::open(ramp);
    QVERIFY(dec);
    for (qint64 pos = 0; pos < 144000; pos += 1000) checkRamp(*dec, pos, 1000);
    // reading uneven chunk sizes back to back must not drop or repeat a sample
    dec = AudioDecoder::open(ramp);
    qint64 pos = 0;
    for (const qint64 len : {1, 7, 1023, 1024, 1025, 4096, 3, 50000}) {
      checkRamp(*dec, pos, len);
      pos += len;
    }
  }

  void seekIsSampleAccurate() {
    auto dec = AudioDecoder::open(ramp);
    QVERIFY(dec);
    // positions that are not packet or frame aligned, near both ends, and across the 32768 ramp wrap
    for (const qint64 start : {0, 1, 12345, 32767, 32768, 100000, 143000, 143999, 70001, 5, 90000}) {
      checkRamp(*dec, start, 1);
      checkRamp(*dec, start, 777);
    }
    QRandomGenerator rng(99);
    for (int i = 0; i < 60; ++i) checkRamp(*dec, rng.bounded(143000u), 1 + rng.bounded(5000u));
  }

  void backwardsAndRepeatedReads() {
    auto dec = AudioDecoder::open(ramp);
    QVERIFY(dec);
    checkRamp(*dec, 50000, 2000);
    checkRamp(*dec, 50000, 2000); // same range again
    checkRamp(*dec, 49000, 3000); // overlapping, earlier
    checkRamp(*dec, 20000, 100);  // far back
    checkRamp(*dec, 20100, 100);  // and on from there
  }

  void outsideTheStreamIsSilence() {
    auto dec = AudioDecoder::open(ramp);
    QVERIFY(dec);
    checkRamp(*dec, -500, 1000);          // starts before 0
    checkRamp(*dec, 143500, 1000);        // runs off the end
    checkRamp(*dec, 500000, 100);         // entirely past the end
    const std::vector<float> v = dec->read(0, 0);
    QVERIFY(v.empty());
    float one[2] = {9, 9};
    QCOMPARE(dec->read(143999, 1, one), 1);
    QCOMPARE(dec->read(144000 + 10, 1, one), 0);
    QCOMPARE(one[0], 0.0f);
  }

  void monoIsDuplicatedAtUnityGain() {
    auto dec = AudioDecoder::open(rampMono);
    QVERIFY(dec);
    QCOMPARE(dec->info().sourceChannels, 1);
    const std::vector<float> v = dec->read(20000, 5000);
    for (qint64 i = 0; i < 5000; ++i) {
      const float e = expectedRamp(20000 + i);
      QVERIFY2(std::abs(v[static_cast<size_t>(i) * 2] - e) < 1e-6f, "left");
      QVERIFY2(std::abs(v[static_cast<size_t>(i) * 2 + 1] - e) < 1e-6f, "right");
    }
  }

  // 44.1 kHz sources are resampled; the ramp slope lets us check the position to within a few samples.
  void resampledSourceKeepsTimeAlignment() {
    auto dec = AudioDecoder::open(ramp44);
    QVERIFY(dec);
    QCOMPARE(dec->info().sourceRate, 44100);
    QCOMPARE(dec->info().totalSamples, 96000);
    const double ratio = 44100.0 / 48000.0;
    auto check = [&](qint64 start, qint64 count) {
      const std::vector<float> v = dec->read(start, count);
      for (qint64 i = 0; i < count; ++i) {
        const double e = (double(start + i) * ratio) / 32768.0; // stays below the wrap for the ranges used
        if (std::abs(v[static_cast<size_t>(i) * 2] - e) > 1.5e-4) {
          QFAIL(qPrintable(QStringLiteral("pos %1: got %2 expected %3 (off by %4 output samples)")
                               .arg(start + i).arg(v[static_cast<size_t>(i) * 2]).arg(e).arg((v[static_cast<size_t>(i) * 2] - e) * 32768 / ratio)));
        }
      }
    };
    check(3000, 4000);   // after the resampler's start-up
    check(30000, 1000);  // seeked
    check(31000, 1000);  // continued
    check(8000, 500);    // back
  }

  void lossyAudioSeeksMatchSequentialDecode() {
    auto dec = AudioDecoder::open(aac);
    QVERIFY(dec);
    QCOMPARE(dec->info().codec, QStringLiteral("aac"));
    QVERIFY(qAbs(dec->info().totalSamples - 144000) <= 2048);
    const qint64 n = 140000;
    const std::vector<float> all = dec->read(0, n);

    // alignment: the decoded sine must sit on top of the formula (an off-by-one sample is ~0.065)
    double worst = 0;
    for (qint64 i = 1000; i < n - 1000; ++i) {
      const double e = 0.5 * std::sin(2 * 3.14159265358979323846 * 1000.0 * i / 48000.0);
      worst = std::max(worst, std::abs(all[static_cast<size_t>(i) * 2] - e));
    }
    QVERIFY2(worst < 0.03, qPrintable(QStringLiteral("max deviation from the reference sine %1").arg(worst)));

    auto dec2 = AudioDecoder::open(aac);
    QRandomGenerator rng(5);
    for (int i = 0; i < 30; ++i) {
      const qint64 start = rng.bounded(static_cast<quint32>(n - 6000));
      const qint64 len = 1 + rng.bounded(5000u);
      const std::vector<float> v = dec2->read(start, len);
      for (qint64 k = 0; k < len; ++k) {
        const float a = v[static_cast<size_t>(k) * 2];
        const float b = all[static_cast<size_t>(start + k) * 2];
        if (std::abs(a - b) > 1e-3f) QFAIL(qPrintable(QStringLiteral("seek to %1: sample %2 differs: %3 vs %4").arg(start).arg(k).arg(a).arg(b)));
      }
    }
  }

  void streamStartOffsetIsReported() {
    const QString path = dir.filePath(QStringLiteral("late_audio.mkv"));
    QString log;
    QVERIFY2(sf::test::runFfmpeg({"-f", "lavfi", "-i", "color=c=black:s=64x64:r=25:d=2", "-itsoffset", "0.5", "-i", ramp, "-c:v", "mpeg4",
                                  "-c:a", "pcm_s16le", "-shortest", path},
                                 &log),
             qPrintable(log));
    auto dec = AudioDecoder::open(path);
    QVERIFY(dec);
    QVERIFY2(qAbs(dec->info().startSec - 0.5) < 0.002, qPrintable(QString::number(dec->info().startSec)));
    // sample 0 is still the first sample of the stream
    const std::vector<float> v = dec->read(0, 100);
    for (qint64 i = 0; i < 100; ++i) QVERIFY(std::abs(v[static_cast<size_t>(i) * 2] - expectedRamp(i)) < 1e-6f);
  }

  void waveformPeaksOfARamp() {
    QString err;
    const WaveformPeaks p = extractWaveformPeaks(ramp, 480, {}, &err);
    QVERIFY2(p.bucketCount() > 0, qPrintable(err));
    QCOMPARE(p.bucketCount(), 300);
    QCOMPARE(p.samplesPerPeak, 480);
    QCOMPARE(p.totalSamples, 144000);
    // before the 32768 wrap bucket b spans ramp values 480b .. 480b+479; L rises, R mirrors it
    for (int b = 0; b < 68; ++b) {
      const float top = (480.0f * b + 479.0f) / 32768.0f;
      QVERIFY2(std::abs(p.maxAt(b) - top) < 1e-5f, qPrintable(QStringLiteral("bucket %1 max %2 want %3").arg(b).arg(p.maxAt(b)).arg(top)));
      QVERIFY2(std::abs(p.minAt(b) + top) < 1e-5f, qPrintable(QStringLiteral("bucket %1 min %2 want %3").arg(b).arg(p.minAt(b)).arg(-top)));
    }
    const WaveformPeaks coarse = p.reduced(10);
    QCOMPARE(coarse.bucketCount(), 30);
    QCOMPARE(coarse.samplesPerPeak, 4800);
    for (int c = 0; c < 6; ++c) {
      float hi = -1, lo = 1;
      for (int b = c * 10; b < c * 10 + 10; ++b) {
        hi = std::max(hi, p.maxAt(b));
        lo = std::min(lo, p.minAt(b));
      }
      QCOMPARE(coarse.maxAt(c), hi);
      QCOMPARE(coarse.minAt(c), lo);
    }
  }

  void waveformPeaksOfASine() {
    const WaveformPeaks p = extractWaveformPeaks(aac, 1024);
    QVERIFY(p.bucketCount() >= 130);
    int loud = 0;
    for (qint64 b = 2; b < p.bucketCount() - 3; ++b) {
      if (p.maxAt(b) > 0.45f && p.minAt(b) < -0.45f && p.maxAt(b) < 0.56f) ++loud;
    }
    QVERIFY2(loud > p.bucketCount() - 8, qPrintable(QString::number(loud)));
  }

  void waveformCancel() {
    int polls = 0;
    const WaveformPeaks p = extractWaveformPeaks(ramp, 480, [&polls] { return ++polls > 1; });
    QCOMPARE(p.bucketCount(), 0);
  }

  void failuresAreReported() {
    QString err;
    QVERIFY(!AudioDecoder::open(dir.filePath(QStringLiteral("none.wav")), &err));
    QVERIFY(!err.isEmpty());
    // a file with only video has no audio stream
    const QString v = dir.filePath(QStringLiteral("video_only.mp4"));
    QString log;
    QVERIFY2(sf::test::makeIndexClip(v, {"-c:v", "mpeg4"}, 320, 240, 25, 1, {}, &log), qPrintable(log));
    err.clear();
    QVERIFY(!AudioDecoder::open(v, &err));
    QVERIFY(err.contains(QLatin1String("no decodable audio")));
  }
};

QTEST_GUILESS_MAIN(TstAudioDecoder)
#include "tst_audio_decoder.moc"
