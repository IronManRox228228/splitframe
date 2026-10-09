#include "media/thumbnails.h"
#include "media/video_decoder.h"
#include "media_testutil.h"

#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>

using namespace sf;
using sf::test::readIndex;

class TstThumbnails : public QObject {
  Q_OBJECT
  QTemporaryDir dir;
  QString make(const QString& name, const QStringList& codec, int w = 320, int h = 240, double seconds = 4) {
    const QString path = dir.filePath(name);
    QString log;
    if (!sf::test::makeIndexClip(path, codec, w, h, 25, seconds, {}, &log)) {
      qWarning() << "cannot generate" << name << log;
      return {};
    }
    return path;
  }

private slots:
  void initTestCase() {
    QVERIFY(dir.isValid());
    QVERIFY2(!sf::test::ffmpegExe().isEmpty(), "SF_FFMPEG_EXE not set");
  }

  // Keyframes every 10 frames: each thumbnail must be the keyframe at or before its slot.
  void picksTheKeyframeAtOrBeforeEachSlot() {
    const QString clip = make(QStringLiteral("g10.mp4"), {"-c:v", "mpeg4", "-g", "10", "-bf", "2", "-q:v", "2"});
    QVERIFY(!clip.isEmpty());
    QString err;
    auto dec = VideoDecoder::open(clip, {HwMode::Off, 0});
    QVERIFY(dec);
    const auto strip = extractThumbnailStrip(clip, 8, QSize(160, 120), {}, &err);
    QVERIFY2(strip.size() == 8, qPrintable(err));
    for (size_t i = 0; i < strip.size(); ++i) {
      const Thumbnail& t = strip[i];
      QVERIFY(!t.image.isNull());
      QVERIFY(t.image.width() <= 160 && t.image.height() <= 120);
      QCOMPARE(t.image.size(), QSize(160, 120)); // exactly half size, so the frame-number blocks stay readable
      const int frame = readIndex(t.image);
      const int slotFrame = int(t.requestedSec * 25);
      // the encoder also inserts keyframes on its own (scene cuts), so ask the decoder's table
      QVERIFY2(dec->isKeyFrame(frame), qPrintable(QStringLiteral("slot %1 shows frame %2, not a keyframe").arg(i).arg(frame)));
      // mp4 indexes keyframes by decode time, which runs a few frames (the B-frame delay) ahead of display time
      QVERIFY2(frame <= slotFrame + 3, qPrintable(QStringLiteral("slot %1 (frame %2) shows later frame %3").arg(i).arg(slotFrame).arg(frame)));
      for (int n = frame + 1; n <= slotFrame - 3; ++n) {
        QVERIFY2(!dec->isKeyFrame(n), qPrintable(QStringLiteral("slot %1 (frame %2) shows %3 but %4 is a closer keyframe").arg(i).arg(slotFrame).arg(frame).arg(n)));
      }
      QVERIFY(qAbs(t.actualSec - frame / 25.0) < 1e-3);
      QVERIFY(t.actualSec <= t.requestedSec + 0.13);
    }
  }

  void intraCodecShowsTheExactFrame() {
    const QString clip = make(QStringLiteral("prores.mov"), {"-c:v", "prores_ks", "-profile:v", "3", "-pix_fmt", "yuv422p10le"});
    QVERIFY(!clip.isEmpty());
    const auto strip = extractThumbnailStrip(clip, 10, QSize(160, 120));
    QCOMPARE(strip.size(), size_t(10));
    for (const Thumbnail& t : strip) {
      const int slotFrame = int(t.requestedSec * 25 + 1e-6);
      QCOMPARE(readIndex(t.image), slotFrame);
    }
  }

  void sparseKeyframesShareImages() {
    const QString clip = make(QStringLiteral("g100.mp4"), {"-c:v", "mpeg4", "-g", "100", "-q:v", "2"});
    QVERIFY(!clip.isEmpty());
    const auto strip = extractThumbnailStrip(clip, 6);
    QCOMPARE(strip.size(), size_t(6));
    for (const Thumbnail& t : strip) {
      QCOMPARE(readIndex(t.image), 0);
      QCOMPARE(t.actualSec, 0.0);
    }
    QCOMPARE(strip[0].image.constBits(), strip[5].image.constBits()); // one decode, shared pixels
  }

  void rotatedClipsComeOutUpright() {
    const QString clip = make(QStringLiteral("g10b.mp4"), {"-c:v", "mpeg4", "-g", "10", "-q:v", "2"});
    QVERIFY(!clip.isEmpty());
    const QString rot = dir.filePath(QStringLiteral("rot.mp4"));
    QString log;
    QVERIFY2(sf::test::runFfmpeg({"-display_rotation", "90", "-i", clip, "-c", "copy", rot}, &log), qPrintable(log));
    const auto strip = extractThumbnailStrip(rot, 3, QSize(160, 160));
    QCOMPARE(strip.size(), size_t(3));
    QCOMPARE(strip[0].image.size(), QSize(120, 160)); // stored 4:3 landscape shown as 3:4 portrait
  }

  void isFastEvenOnLongGopFiles() {
    const QString clip = make(QStringLiteral("g250.mp4"), {"-c:v", "mpeg4", "-g", "250", "-q:v", "2"}, 640, 360, 10);
    QVERIFY(!clip.isEmpty());
    QElapsedTimer t;
    t.start();
    const auto strip = extractThumbnailStrip(clip, 20);
    QCOMPARE(strip.size(), size_t(20));
    QVERIFY2(t.elapsed() < 2000, qPrintable(QString::number(t.elapsed())));
  }

  void cancelAndFailure() {
    const QString clip = make(QStringLiteral("c.mp4"), {"-c:v", "mpeg4", "-g", "10"});
    QVERIFY(!clip.isEmpty());
    int polls = 0;
    QVERIFY(extractThumbnailStrip(clip, 5, {160, 90}, [&polls] { return ++polls > 2; }).empty());
    QString err;
    QVERIFY(extractThumbnailStrip(dir.filePath(QStringLiteral("none.mp4")), 4, {160, 90}, {}, &err).empty());
    QVERIFY(!err.isEmpty());
  }
};

QTEST_GUILESS_MAIN(TstThumbnails)
#include "tst_thumbnails.moc"
