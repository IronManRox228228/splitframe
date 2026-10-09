#include "media/probe.h"

#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

// Generates a known clip with the ffmpeg CLI (2 s, 320x240, 25 fps, red then blue, with a tone)
// and checks the library reads it back.
class TstProbe : public QObject {
  Q_OBJECT
  QTemporaryDir dir;
  QString clip;

private slots:
  void initTestCase() {
    QVERIFY(dir.isValid());
    const QString ffmpeg = qEnvironmentVariable("SF_FFMPEG_EXE");
    QVERIFY2(!ffmpeg.isEmpty(), "SF_FFMPEG_EXE not set");
    clip = dir.filePath(QStringLiteral("clip.mp4"));
    QProcess p;
    p.start(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("lavfi"),
                     QStringLiteral("-i"), QStringLiteral("color=c=red:s=320x240:r=25:d=1"), QStringLiteral("-f"),
                     QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("color=c=blue:s=320x240:r=25:d=1"),
                     QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                     QStringLiteral("sine=frequency=440:duration=2"), QStringLiteral("-filter_complex"),
                     QStringLiteral("[0:v][1:v]concat=n=2:v=1:a=0[v]"), QStringLiteral("-map"), QStringLiteral("[v]"),
                     QStringLiteral("-map"), QStringLiteral("2:a"), QStringLiteral("-c:v"), QStringLiteral("mpeg4"),
                     QStringLiteral("-q:v"), QStringLiteral("2"), QStringLiteral("-c:a"), QStringLiteral("aac"),
                     QStringLiteral("-y"), clip});
    QVERIFY(p.waitForFinished(30000));
    QCOMPARE(p.exitCode(), 0);
  }

  void probe() {
    QString error;
    const auto info = sf::probeMedia(clip, &error);
    QVERIFY2(info.has_value(), qPrintable(error));
    QVERIFY(info->hasVideo);
    QVERIFY(info->hasAudio);
    QCOMPARE(info->width, 320);
    QCOMPARE(info->height, 240);
    QCOMPARE(info->fps, 25.0);
    QVERIFY2(qAbs(info->durationMs - 2000) <= 50, qPrintable(QString::number(info->durationMs)));
  }

  void decodesTheRightFrame() {
    QString error;
    const QImage first = sf::decodeFrame(clip, 0, &error);
    QVERIFY2(!first.isNull(), qPrintable(error));
    QCOMPARE(first.size(), QSize(320, 240));
    const QColor a = first.pixelColor(160, 120);
    QVERIFY2(a.red() > 200 && a.blue() < 60, qPrintable(a.name()));

    const QImage later = sf::decodeFrame(clip, 1500, &error);
    QVERIFY2(!later.isNull(), qPrintable(error));
    const QColor b = later.pixelColor(160, 120);
    QVERIFY2(b.blue() > 200 && b.red() < 60, qPrintable(b.name()));
  }

  void missingFileFailsCleanly() {
    QString error;
    QVERIFY(!sf::probeMedia(dir.filePath(QStringLiteral("nope.mp4")), &error).has_value());
    QVERIFY(!error.isEmpty());
    QVERIFY(sf::decodeFrame(dir.filePath(QStringLiteral("nope.mp4")), 0, &error).isNull());
  }
};

QTEST_APPLESS_MAIN(TstProbe)
#include "tst_probe.moc"
