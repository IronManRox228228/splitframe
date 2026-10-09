#include "core/time.h"

#include <QTest>

// Mirrors packages/schema/src/time.test.ts: the native app must produce the same frames and timecodes.
class TstTime : public QObject {
  Q_OBJECT
private slots:
  void conversions() {
    QCOMPARE(sf::secondsToFrames(2.5, 30), 75);
    QCOMPARE(sf::framesToMs(45, 30), 1500);
    QCOMPARE(sf::msToFrames(1000, 29.97), 30);
    QCOMPARE(sf::framesToSeconds(60, 24), 2.5);
  }

  void roundsHalfUpLikeJavaScript() {
    QCOMPARE(sf::secondsToFrames(0.5 / 30, 30), 1); // exactly half a frame
    QCOMPARE(sf::secondsToFrames(-0.5 / 30, 30), 0); // Math.round(-0.5) is -0, not -1
  }

  void timecode_data() {
    QTest::addColumn<qint64>("frames");
    QTest::addColumn<double>("fps");
    QTest::addColumn<QString>("expected");
    QTest::newRow("zero") << qint64(0) << 30.0 << "00:00.0";
    QTest::newRow("7.2s") << qint64(216) << 30.0 << "00:07.2";
    QTest::newRow("carry into minute") << qint64(1799) << 30.0 << "01:00.0";
    QTest::newRow("hours") << qint64(3723 * 30) << 30.0 << "01:02:03.0";
    QTest::newRow("negative clamps") << qint64(-5) << 30.0 << "00:00.0";
  }
  void timecode() {
    QFETCH(qint64, frames);
    QFETCH(double, fps);
    QFETCH(QString, expected);
    QCOMPARE(sf::formatTimecode(frames, fps), expected);
  }
};

QTEST_APPLESS_MAIN(TstTime)
#include "tst_time.moc"
