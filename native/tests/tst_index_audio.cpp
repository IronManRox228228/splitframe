#include "index/audio_analyzer.h"

#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <vector>

class TstIndexAudio : public QObject {
  Q_OBJECT

  QTemporaryDir dir_;
  QString audioClipPath_;

private slots:
  void initTestCase() {
    QVERIFY(dir_.isValid());
    const QString ffmpeg = qEnvironmentVariable("SF_FFMPEG_EXE");
    QVERIFY2(!ffmpeg.isEmpty(), "SF_FFMPEG_EXE not set");

    // Generate 3-second audio: 1.5s 440Hz tone followed by 1.5s silence
    audioClipPath_ = dir_.filePath(QStringLiteral("audio_silence_test.wav"));
    QProcess p;
    p.start(ffmpeg, {
      QStringLiteral("-v"), QStringLiteral("error"),
      QStringLiteral("-f"), QStringLiteral("lavfi"),
      QStringLiteral("-i"), QStringLiteral("sine=frequency=440:duration=1.5"),
      QStringLiteral("-f"), QStringLiteral("lavfi"),
      QStringLiteral("-i"), QStringLiteral("anullsrc=r=48000:cl=stereo:d=1.5"),
      QStringLiteral("-filter_complex"), QStringLiteral("[0:a][1:a]concat=n=2:v=0:a=1[a]"),
      QStringLiteral("-map"), QStringLiteral("[a]"),
      QStringLiteral("-c:a"), QStringLiteral("pcm_s16le"),
      QStringLiteral("-y"), audioClipPath_
    });
    QVERIFY(p.waitForFinished(30000));
    QCOMPARE(p.exitCode(), 0);
  }

  void rmsDbCalculation() {
    // 1. Pure silence (zeros) -> -100 dB
    std::vector<float> silence(4800, 0.0f);
    const double rmsSil = sf::index::AudioAnalyzer::calculateRmsDb(silence.data(), 2400, 2);
    QCOMPARE(rmsSil, -100.0);

    // 2. Full scale constant 1.0 -> 0 dBFS
    std::vector<float> full(4800, 1.0f);
    const double rmsFull = sf::index::AudioAnalyzer::calculateRmsDb(full.data(), 2400, 2);
    QVERIFY(std::abs(rmsFull - 0.0) < 0.01);
  }

  void speechAndSilenceSegmentation() {
    sf::index::AudioAnalyzer analyzer;
    sf::index::AnalysisOptions opts;
    opts.silenceThresholdDb = -40.0;
    opts.minSilenceDurationSec = 0.3;

    const auto res = analyzer.analyzeAudio(audioClipPath_, QStringLiteral("audio_asset"), 25.0, opts);

    // Loudness metrics must be finite
    QVERIFY(!std::isinf(res.integratedLufs));
    QVERIFY(!std::isnan(res.integratedLufs));
    QVERIFY(res.integratedLufs < 0.0); // True audio is below 0 LUFS

    // Spans: tone (speech/active) -> silence
    QVERIFY(res.spans.size() >= 2);

    const auto& span1 = res.spans.front();
    const auto& spanLast = res.spans.back();

    QVERIFY(span1.isSpeech);
    QVERIFY(span1.meanVolumeDb > -30.0); // Tone is reasonably loud

    QVERIFY(!spanLast.isSpeech);
    QVERIFY(spanLast.meanVolumeDb < -70.0); // Silence is extremely quiet
  }
};

QTEST_MAIN(TstIndexAudio)
#include "tst_index_audio.moc"
