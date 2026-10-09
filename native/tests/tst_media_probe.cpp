#include "media/probe.h"
#include "media_testutil.h"

#include <QTemporaryDir>
#include <QTest>

using namespace sf;

// Probe details beyond tst_probe's basics: orientation, colour tags, frame timing, stream offsets.
class TstMediaProbe : public QObject {
  Q_OBJECT
  QTemporaryDir dir;

  QString make(const QString& name, const QStringList& codec, double fps = 25, double seconds = 4, const QStringList& before = {},
               const QString& pix = QStringLiteral("yuv420p")) {
    const QString path = dir.filePath(name);
    QString log;
    if (!sf::test::makeIndexClip(path, codec, 320, 240, fps, seconds, before, &log, pix)) {
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

  void rotationFromDisplayMatrix_data() {
    QTest::addColumn<int>("degrees");
    QTest::addColumn<int>("expectedRotation");
    QTest::addColumn<int>("displayWidth");
    QTest::addColumn<int>("displayHeight");
    // -display_rotation is counter-clockwise (ffmpeg's convention); `rotation` is the clockwise turn to display upright
    QTest::newRow("none") << 0 << 0 << 320 << 240;
    QTest::newRow("ccw90") << 90 << 270 << 240 << 320;
    QTest::newRow("cw90") << -90 << 90 << 240 << 320;
    QTest::newRow("180") << 180 << 180 << 320 << 240;
  }
  void rotationFromDisplayMatrix() {
    QFETCH(int, degrees);
    QFETCH(int, expectedRotation);
    QFETCH(int, displayWidth);
    QFETCH(int, displayHeight);
    const QString base = make(QStringLiteral("base.mp4"), {"-c:v", "mpeg4"}, 25, 1);
    QVERIFY(!base.isEmpty());
    const QString out = dir.filePath(QStringLiteral("rot_%1.mp4").arg(degrees));
    QString log;
    QVERIFY2(sf::test::runFfmpeg({"-display_rotation", QString::number(degrees), "-i", base, "-c", "copy", out}, &log), qPrintable(log));
    const auto info = probeMedia(out);
    QVERIFY(info);
    QCOMPARE(info->rotation, expectedRotation);
    QCOMPARE(info->width, 320);
    QCOMPARE(info->height, 240);
    QCOMPARE(info->displayWidth, displayWidth);
    QCOMPARE(info->displayHeight, displayHeight);
  }

  void colourTags() {
    const QString hdr = make(QStringLiteral("hdr.mp4"), {"-c:v", "mpeg4"}, 25, 1, {"-vf", "setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc:range=tv"});
    QVERIFY(!hdr.isEmpty());
    const auto h = probeMedia(hdr);
    QVERIFY(h);
    QCOMPARE(h->color.primaries, QStringLiteral("bt2020"));
    QCOMPARE(h->color.transfer, QStringLiteral("smpte2084"));
    QCOMPARE(h->color.matrix, QStringLiteral("bt2020nc"));
    QCOMPARE(h->color.range, QStringLiteral("tv"));
    QVERIFY(h->color.isHdr());

    const QString sdr = make(QStringLiteral("sdr.mp4"), {"-c:v", "mpeg4"}, 25, 1, {"-vf", "setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709"});
    const auto s = probeMedia(sdr);
    QVERIFY(s);
    QCOMPARE(s->color.primaries, QStringLiteral("bt709"));
    QVERIFY(!s->color.isHdr());

    const QString plain = make(QStringLiteral("untagged.avi"), {"-c:v", "mjpeg"});
    const auto p = probeMedia(plain);
    QVERIFY(p);
    QCOMPARE(p->color.primaries, QStringLiteral("unspecified"));
    QCOMPARE(p->color.transfer, QStringLiteral("unspecified"));
  }

  void pixelFormatAndBitDepth() {
    const QString p8 = make(QStringLiteral("p8.mp4"), {"-c:v", "mpeg4"});
    const auto a = probeMedia(p8);
    QVERIFY(a);
    QCOMPARE(a->pixelFormat, QStringLiteral("yuv420p"));
    QCOMPARE(a->bitDepth, 8);
    const QString pr = make(QStringLiteral("p10.mov"), {"-c:v", "prores_ks", "-profile:v", "3", "-pix_fmt", "yuv422p10le"});
    const auto b = probeMedia(pr);
    QVERIFY(b);
    QCOMPARE(b->pixelFormat, QStringLiteral("yuv422p10le"));
    QCOMPARE(b->bitDepth, 10);
    QCOMPARE(b->videoCodec, QStringLiteral("prores"));
  }

  void constantFrameRateIsNotVfr_data() {
    QTest::addColumn<QString>("name");
    QTest::addColumn<double>("fps");
    QTest::newRow("25 mp4") << "c25.mp4" << 25.0;
    QTest::newRow("29.97 mp4") << "c2997.mp4" << 29.97;
    QTest::newRow("30 mkv (ms time base)") << "c30.mkv" << 30.0;
    QTest::newRow("23.976 mkv") << "c23976.mkv" << 23.976;
    QTest::newRow("60 mkv") << "c60.mkv" << 60.0;
  }
  // Matroska rounds every timestamp to a millisecond, so a perfectly steady 30 fps file has 33/34 ms
  // gaps. That is jitter, not variable frame rate.
  void constantFrameRateIsNotVfr() {
    QFETCH(QString, name);
    QFETCH(double, fps);
    const QString clip = make(name, {"-c:v", "mpeg4", "-g", "30", "-bf", "2"}, fps, 3);
    QVERIFY(!clip.isEmpty());
    const auto info = probeMedia(clip);
    QVERIFY(info);
    QVERIFY2(!info->vfr, qPrintable(QStringLiteral("min %1 max %2 fps").arg(info->minFps).arg(info->maxFps)));
    QVERIFY(info->timingComplete);
    QCOMPARE(info->frameCount, qRound64(fps * 3));
    QVERIFY(qAbs(info->avgFps - fps) < 0.05);
  }

  void variableFrameRateIsDetected() {
    const QString clip = make(QStringLiteral("vfr.mkv"), {"-c:v", "libvpx-vp9", "-deadline", "realtime", "-g", "20", "-fps_mode", "passthrough", "-enc_time_base", "1:1000"}, 25, 3,
                              {"-vf", "setpts='if(lt(N,50),N,50+(N-50)*3)'"});
    QVERIFY(!clip.isEmpty());
    const auto info = probeMedia(clip);
    QVERIFY(info);
    QVERIFY(info->vfr);
    QCOMPARE(info->frameCount, 75);
    QVERIFY2(qAbs(info->maxFps - 25.0) < 0.5 && qAbs(info->minFps - 25.0 / 3) < 0.5,
             qPrintable(QStringLiteral("%1..%2").arg(info->minFps).arg(info->maxFps)));
    QVERIFY(info->avgFps > 10 && info->avgFps < 25);
  }

  void scanLimitIsHonoured() {
    const QString clip = make(QStringLiteral("long.mp4"), {"-c:v", "mpeg4", "-g", "30"}, 25, 6);
    QVERIFY(!clip.isEmpty());
    ProbeOptions quick;
    quick.vfrScanLimit = 40;
    const auto cut = probeMedia(clip, nullptr, quick);
    QVERIFY(cut);
    QVERIFY(!cut->timingComplete);
    QVERIFY(!cut->vfr);
    QCOMPARE(cut->frameCount, 150); // falls back to the container's own count
    ProbeOptions full;
    full.vfrScanLimit = 0;
    const auto all = probeMedia(clip, nullptr, full);
    QVERIFY(all);
    QVERIFY(all->timingComplete);
    QCOMPARE(all->frameCount, 150);
  }

  void streamStartOffsets() {
    const QString path = dir.filePath(QStringLiteral("offsets.mkv"));
    QString log;
    QVERIFY2(sf::test::runFfmpeg({"-f", "lavfi", "-i", sf::test::indexSource(320, 240, 25, 2), "-f", "lavfi", "-i", "sine=duration=2", "-vf",
                                  "setpts=PTS+0.6/TB", "-c:v", "mpeg4", "-c:a", "pcm_s16le", path},
                                 &log),
             qPrintable(log));
    const auto info = probeMedia(path);
    QVERIFY(info);
    QVERIFY2(qAbs(info->videoStartSec - 0.6) < 0.002, qPrintable(QString::number(info->videoStartSec)));
    QVERIFY2(qAbs(info->audioStartSec) < 0.002, qPrintable(QString::number(info->audioStartSec)));
  }

  void audioStreamDetails() {
    const QString path = dir.filePath(QStringLiteral("a.mp4"));
    QString log;
    QVERIFY2(sf::test::runFfmpeg({"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=1", "-ac", "1", "-c:a", "aac", path}, &log),
             qPrintable(log));
    const auto info = probeMedia(path);
    QVERIFY(info);
    QVERIFY(info->hasAudio);
    QVERIFY(!info->hasVideo);
    QCOMPARE(info->audioSampleRate, 44100);
    QCOMPARE(info->audioChannels, 1);
    QCOMPARE(info->audioCodec, QStringLiteral("aac"));
    QCOMPARE(info->frameCount, 0);
  }
};

QTEST_GUILESS_MAIN(TstMediaProbe)
#include "tst_media_probe.moc"
