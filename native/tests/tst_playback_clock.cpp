#include "render/playback_clock.h"

#include <QTest>

using namespace sf;
using namespace sf::render;

namespace {

// Time only moves when the test says so
class FakeClock final : public ClockSource {
public:
  std::int64_t now = 1'000'000'000;
  std::int64_t nowNs() const override { return now; }
  void advanceMs(double ms) { now += static_cast<std::int64_t>(ms * 1e6); }
};

} // namespace

class TstPlaybackClock : public QObject {
  Q_OBJECT

  std::shared_ptr<FakeClock> src = std::make_shared<FakeClock>();
  std::unique_ptr<PlaybackClock> clock;

private slots:
  void init() {
    src = std::make_shared<FakeClock>();
    clock = std::make_unique<PlaybackClock>(src);
    clock->setTimeline(30, 300);
  }

  void pausedClockStandsStill() {
    clock->seek(42);
    src->advanceMs(5000);
    QCOMPARE(clock->poll().frame, 42);
    QVERIFY(!clock->playing());
  }

  void playingAdvancesAtTheProjectRate() {
    clock->seek(10);
    clock->play();
    src->advanceMs(1000.0 / 30 * 0.5);
    QCOMPARE(clock->poll().frame, 10); // half a frame in
    src->advanceMs(1000.0 / 30 * 0.5);
    QCOMPARE(clock->poll().frame, 11);
    src->advanceMs(1000); // one second = 30 frames
    QCOMPARE(clock->poll().frame, 41);
  }

  // The point of a clock-derived position: a late present skips frames, it does not lag behind
  void lateFramesAreSkippedNotDelayed() {
    clock->play();
    PacingStats stats;
    Frame f = clock->poll().frame;
    stats.present(f, true, 1);
    for (int i = 0; i < 10; ++i) {
      src->advanceMs(1000.0 / 30);
      stats.present(clock->poll().frame, true, 1);
    }
    QCOMPARE(stats.dropped(), 0);
    src->advanceMs(5 * 1000.0 / 30); // a 5 frame hitch
    const Frame after = clock->poll().frame;
    QCOMPARE(after, 15);
    stats.present(after, true, 1);
    QCOMPARE(stats.dropped(), 4);
    // and it is back in step immediately: no accumulated drift
    src->advanceMs(1000.0 / 30);
    QCOMPARE(clock->poll().frame, 16);
    QCOMPARE(stats.presented(), 12);
  }

  void noDriftOverALongRun() {
    clock->setTimeline(30, 1'000'000);
    clock->play();
    // jittery 60 Hz presents for a simulated hour
    std::int64_t elapsed = 0;
    for (int i = 0; i < 60 * 3600; ++i) {
      const std::int64_t step = (i % 7 == 0) ? 16'900'000 : 16'600'000;
      src->now += step;
      elapsed += step;
    }
    const Frame expect = static_cast<Frame>(static_cast<double>(elapsed) * 1e-9 * 30);
    QCOMPARE(clock->poll().frame, expect);
  }

  void pauseKeepsTheFrameAndResumeContinues() {
    clock->play();
    src->advanceMs(500);
    clock->pause();
    const Frame at = clock->poll().frame;
    QCOMPARE(at, 15);
    src->advanceMs(3000);
    QCOMPARE(clock->poll().frame, at);
    clock->play();
    src->advanceMs(1000);
    QCOMPARE(clock->poll().frame, at + 30);
  }

  void seekWhilePlayingKeepsPlaying() {
    clock->play();
    src->advanceMs(2000);
    clock->seek(100);
    QVERIFY(clock->playing());
    QCOMPARE(clock->poll().frame, 100);
    src->advanceMs(1000);
    QCOMPARE(clock->poll().frame, 130);
  }

  void fasterAndBackwards() {
    clock->seek(100);
    clock->play(2.0);
    src->advanceMs(1000);
    QCOMPARE(clock->poll().frame, 160);
    clock->play(-1.0);
    src->advanceMs(1000);
    QCOMPARE(clock->poll().frame, 130);
    clock->setRate(-4.0);
    src->advanceMs(500);
    QCOMPARE(clock->poll().frame, 70);
    PacingStats stats;
    stats.present(100, true, -1);
    stats.present(97, true, -1); // going backwards, two skipped
    QCOMPARE(stats.dropped(), 2);
  }

  void stopsOnTheLastFrame() {
    clock->seek(290);
    clock->play();
    src->advanceMs(2000);
    const auto p = clock->poll();
    QCOMPARE(p.frame, 299);
    QVERIFY(p.reachedEnd);
    QVERIFY(!p.playing);
    QVERIFY(!clock->playing());
    src->advanceMs(2000);
    QVERIFY(!clock->poll().reachedEnd);
    QCOMPARE(clock->poll().frame, 299);
  }

  void playingFromTheEndRestarts() {
    clock->seek(299);
    clock->play();
    QCOMPARE(clock->poll().frame, 0);
    clock->pause();
    clock->seek(0);
    clock->play(-1);
    QCOMPARE(clock->poll().frame, 299);
  }

  void reverseStopsAtTheStart() {
    clock->seek(10);
    clock->play(-1);
    src->advanceMs(2000);
    const auto p = clock->poll();
    QCOMPARE(p.frame, 0);
    QVERIFY(p.reachedEnd);
  }

  void loopWraps() {
    clock->setLoop(true);
    clock->seek(290);
    clock->play();
    src->advanceMs(1000.0 / 30 * 15);
    const auto p = clock->poll();
    QCOMPARE(p.frame, 5);
    QVERIFY(p.playing);
    QVERIFY(!p.reachedEnd);
  }

  void seekIsClamped() {
    clock->seek(-5);
    QCOMPARE(clock->poll().frame, 0);
    clock->seek(100000);
    QCOMPARE(clock->poll().frame, 299);
  }

  // Audio will become the master clock by replacing the source; nothing else changes
  void anyMonotonicSourceCanBeTheMaster() {
    struct AudioClock final : ClockSource {
      std::int64_t samplesPlayed = 0;
      std::int64_t nowNs() const override { return samplesPlayed * 1'000'000'000 / 48000; }
    };
    auto audio = std::make_shared<AudioClock>();
    PlaybackClock c(audio);
    c.setTimeline(30, 300);
    c.play();
    audio->samplesPlayed = 48000 * 2; // two seconds of audio played
    QCOMPARE(c.poll().frame, 60);
    audio->samplesPlayed += 48000 / 30 / 2;
    QCOMPARE(c.poll().frame, 60); // half a frame later: still the same
    audio->samplesPlayed += 48000 / 30 / 2;
    QCOMPARE(c.poll().frame, 61);
  }

  void shuttleDoublesInTheSameDirectionAndRestartsOnReversal() {
    double rate = PlaybackClock::shuttleRate(1, false, +1);
    QCOMPARE(rate, 1.0); // not playing: L starts at 1x
    rate = PlaybackClock::shuttleRate(rate, true, +1);
    QCOMPARE(rate, 2.0);
    rate = PlaybackClock::shuttleRate(rate, true, +1);
    rate = PlaybackClock::shuttleRate(rate, true, +1);
    QCOMPARE(rate, 8.0);
    QCOMPARE(PlaybackClock::shuttleRate(rate, true, +1), 8.0); // capped
    QCOMPARE(PlaybackClock::shuttleRate(rate, true, -1), -1.0); // J against L: reverse at 1x
    QCOMPARE(PlaybackClock::shuttleRate(-1, true, -1), -2.0);
    QCOMPARE(PlaybackClock::shuttleRate(-4, true, -1), -8.0);
    QCOMPARE(PlaybackClock::shuttleRate(2, false, -1), -1.0); // paused: J starts backwards at 1x whatever the stored rate
  }

  void realClockMoves() {
    PlaybackClock c;
    c.setTimeline(1000, 100000);
    c.play();
    QTest::qWait(60);
    const Frame f = c.frame();
    QVERIFY2(f >= 40 && f < 400, qPrintable(QString::number(f)));
  }
};

QTEST_GUILESS_MAIN(TstPlaybackClock)
#include "tst_playback_clock.moc"
