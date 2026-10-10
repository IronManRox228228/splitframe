#include "index/video_analyzer.h"

#include <QPainter>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

class TstIndexVideo : public QObject {
  Q_OBJECT

  QTemporaryDir dir_;
  QString clipPath_;

private slots:
  void initTestCase() {
    QVERIFY(dir_.isValid());
    const QString ffmpeg = qEnvironmentVariable("SF_FFMPEG_EXE");
    QVERIFY2(!ffmpeg.isEmpty(), "SF_FFMPEG_EXE not set");

    // Generate a 2-second clip with two distinct scenes: 1s red, 1s blue
    clipPath_ = dir_.filePath(QStringLiteral("scene_test.mp4"));
    QProcess p;
    p.start(ffmpeg, {
      QStringLiteral("-v"), QStringLiteral("error"),
      QStringLiteral("-f"), QStringLiteral("lavfi"),
      QStringLiteral("-i"), QStringLiteral("color=c=red:s=320x240:r=25:d=1"),
      QStringLiteral("-f"), QStringLiteral("lavfi"),
      QStringLiteral("-i"), QStringLiteral("color=c=blue:s=320x240:r=25:d=1"),
      QStringLiteral("-filter_complex"), QStringLiteral("[0:v][1:v]concat=n=2:v=1:a=0[v]"),
      QStringLiteral("-map"), QStringLiteral("[v]"),
      QStringLiteral("-c:v"), QStringLiteral("mpeg4"),
      QStringLiteral("-q:v"), QStringLiteral("2"),
      QStringLiteral("-y"), clipPath_
    });
    QVERIFY(p.waitForFinished(30000));
    QCOMPARE(p.exitCode(), 0);
  }

  void sharpnessMetrics() {
    // 1. Uniform flat image -> variance must be 0
    QImage flat(320, 240, QImage::Format_Grayscale8);
    flat.fill(128);
    const double flatSharp = sf::index::VideoAnalyzer::calculateSharpness(flat);
    QCOMPARE(flatSharp, 0.0);

    // 2. High-frequency checkerboard pattern -> high variance
    QImage checker(320, 240, QImage::Format_Grayscale8);
    for (int y = 0; y < 240; ++y) {
      uchar* row = checker.scanLine(y);
      for (int x = 0; x < 320; ++x) {
        row[x] = ((x / 4 + y / 4) % 2 == 0) ? 255 : 0;
      }
    }
    const double checkSharp = sf::index::VideoAnalyzer::calculateSharpness(checker);
    QVERIFY(checkSharp > 500.0);
  }

  void lumaAndClippedExposure() {
    // 1. Blown out highlights
    QImage white(100, 100, QImage::Format_Grayscale8);
    white.fill(255);
    double lumaW = 0.0;
    int clippedW = 0;
    sf::index::VideoAnalyzer::calculateLuma(white, &lumaW, &clippedW);
    QCOMPARE(lumaW, 255.0);
    QCOMPARE(clippedW, 100);

    // 2. Normal midtone image
    QImage mid(100, 100, QImage::Format_Grayscale8);
    mid.fill(128);
    double lumaM = 0.0;
    int clippedM = 0;
    sf::index::VideoAnalyzer::calculateLuma(mid, &lumaM, &clippedM);
    QCOMPARE(lumaM, 128.0);
    QCOMPARE(clippedM, 0);
  }

  void motionAndFreeze() {
    QImage img1(100, 100, QImage::Format_Grayscale8);
    img1.fill(50);
    QImage img2 = img1;

    // Identical frames -> 0 motion
    const double motionZero = sf::index::VideoAnalyzer::calculateMotion(img1, img2);
    QCOMPARE(motionZero, 0.0);
    QVERIFY(sf::index::VideoAnalyzer::isFreezeFrame(motionZero));

    // Shifted frame -> significant motion
    img2.fill(150);
    const double motionHigh = sf::index::VideoAnalyzer::calculateMotion(img1, img2);
    QCOMPARE(motionHigh, 100.0);
    QVERIFY(!sf::index::VideoAnalyzer::isFreezeFrame(motionHigh));
  }

  void sceneDetectionOnSyntheticClip() {
    sf::index::VideoAnalyzer analyzer;
    sf::index::AnalysisOptions opts;
    opts.cacheDir = dir_.filePath(QStringLiteral("thumbs"));
    opts.sceneThreshold = 0.35;

    const auto scenes = analyzer.analyzeVideo(clipPath_, QStringLiteral("clip_test"), opts);

    // Should detect 2 distinct scenes: red (0..1s) and blue (1..2s)
    QCOMPARE(static_cast<int>(scenes.size()), 2);

    QCOMPARE(scenes[0].startFrame, 0);
    QVERIFY(scenes[0].endSec >= 0.8 && scenes[0].endSec <= 1.2);

    QVERIFY(scenes[1].startSec >= 0.8 && scenes[1].startSec <= 1.2);
    QVERIFY(scenes[1].endSec >= 1.9 && scenes[1].endSec <= 2.1);

    // Verify thumbnails exist on disk
    QVERIFY(!scenes[0].thumbnailPath.isEmpty());
    QVERIFY(QFile::exists(scenes[0].thumbnailPath));
    QVERIFY(!scenes[1].thumbnailPath.isEmpty());
    QVERIFY(QFile::exists(scenes[1].thumbnailPath));
  }
};

QTEST_MAIN(TstIndexVideo)
#include "tst_index_video.moc"
