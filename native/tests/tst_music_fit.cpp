#include "audio/music_fit.h"

#include <QTest>

#include <cmath>
#include <string>
#include <vector>

using namespace sf::audio;

class TstMusicFit : public QObject {
  Q_OBJECT

  MusicStructure fixture() {
    MusicStructure ms;
    ms.valid = true;
    ms.bpm = 120.0;
    ms.beatPeriodSec = 0.5;
    ms.durationSec = 112.0;
    ms.finalHitSec = 110.0;
    for (double t = 0.0; t <= 112.0001; t += 0.5) {
      ms.beats.push_back(t);
    }
    for (double t = 0.0; t <= 112.0001; t += 2.0) {
      ms.downbeats.push_back(t);
    }
    // Bar is 2 s (4 beats * 0.5 s)
    ms.sections = {
      {0.0, 16.0, 0, 8, "intro", -16.0, 0.2},
      {16.0, 48.0, 8, 24, "drop", 0.0, 1.0},
      {48.0, 64.0, 24, 32, "breakdown", -10.0, 0.4},
      {64.0, 96.0, 32, 48, "drop", 0.0, 1.0},
      {96.0, 112.0, 48, 56, "outro", -14.0, 0.3}
    };
    return ms;
  }

  static bool isDownbeat(double t, const std::vector<double>& downbeats) {
    for (double d : downbeats) {
      if (std::fabs(d - t) <= 0.01) return true;
    }
    return false;
  }

private slots:
  void editShorter() {
    const MusicStructure ms = fixture();
    const FitRequest req{.targetSec = 80.0, .mode = FitMode::Auto};
    const FitPlan plan = planMusicFit(ms, 112.0, req);

    QVERIFY2(plan.ok, qPrintable(QString::fromStdString(plan.error)));
    QCOMPARE(plan.modeUsed, FitMode::Edit);
    QVERIFY(std::fabs(plan.totalSec - 80.0) <= 0.25);
    QCOMPARE(plan.segments.size(), 2u);

    // Both cut points on downbeats
    QVERIFY2(isDownbeat(plan.segments[0].srcEndSec, ms.downbeats),
             qPrintable(QStringLiteral("seg0 end %1 not on downbeat").arg(plan.segments[0].srcEndSec)));
    QVERIFY2(isDownbeat(plan.segments[1].srcStartSec, ms.downbeats),
             qPrintable(QStringLiteral("seg1 start %1 not on downbeat").arg(plan.segments[1].srcStartSec)));

    QVERIFY(plan.keepsFinalHit);

    // Join is between sections of equal or near relEnergy
    double e0 = -1, e1 = -1;
    for (const auto& sec : ms.sections) {
      if (plan.segments[0].srcEndSec - 0.01 >= sec.startSec && plan.segments[0].srcEndSec - 0.01 < sec.endSec) e0 = sec.relEnergy;
      if (plan.segments[1].srcStartSec + 0.01 >= sec.startSec && plan.segments[1].srcStartSec + 0.01 < sec.endSec) e1 = sec.relEnergy;
    }
    QVERIFY(e0 >= 0 && e1 >= 0);
    QVERIFY(std::fabs(e0 - e1) <= 0.25);
  }

  void trimRequested() {
    const MusicStructure ms = fixture();
    const FitRequest req{.targetSec = 60.0, .mode = FitMode::Trim};
    const FitPlan plan = planMusicFit(ms, 112.0, req);

    QVERIFY2(plan.ok, qPrintable(QString::fromStdString(plan.error)));
    QCOMPARE(plan.modeUsed, FitMode::Trim);
    QCOMPARE(plan.segments.size(), 1u);
    QCOMPARE(plan.segments[0].srcStartSec, 0.0);
    QCOMPARE(plan.segments[0].srcEndSec, 60.0);
    QVERIFY(plan.fadeOutSec >= 0.5 && plan.fadeOutSec <= 3.0);
  }

  void loopLonger() {
    const MusicStructure ms = fixture();
    const FitRequest req{.targetSec = 150.0, .mode = FitMode::Auto};
    const FitPlan plan = planMusicFit(ms, 112.0, req);

    QVERIFY2(plan.ok, qPrintable(QString::fromStdString(plan.error)));
    QCOMPARE(plan.modeUsed, FitMode::Loop);
    QVERIFY(std::fabs(plan.totalSec - 150.0) <= 0.01);

    // Every interior boundary on a downbeat
    for (size_t i = 0; i < plan.segments.size(); ++i) {
      if (i > 0) {
        QVERIFY2(isDownbeat(plan.segments[i].srcStartSec, ms.downbeats),
                 qPrintable(QStringLiteral("interior start %1 not downbeat").arg(plan.segments[i].srcStartSec)));
      }
      if (i + 1 < plan.segments.size()) {
        QVERIFY2(isDownbeat(plan.segments[i].srcEndSec, ms.downbeats),
                 qPrintable(QStringLiteral("interior end %1 not downbeat").arg(plan.segments[i].srcEndSec)));
      }
    }

    QVERIFY(plan.keepsFinalHit);
  }

  void alreadyFits() {
    const MusicStructure ms = fixture();
    const FitRequest req{.targetSec = 112.2, .mode = FitMode::Auto};
    const FitPlan plan = planMusicFit(ms, 112.0, req);

    QVERIFY2(plan.ok, qPrintable(QString::fromStdString(plan.error)));
    QCOMPARE(plan.segments.size(), 1u);
    QCOMPARE(plan.fadeOutSec, 0.0);
  }

  void tooShort() {
    const MusicStructure ms = fixture();
    const FitRequest req{.targetSec = 6.0, .mode = FitMode::Auto};
    const FitPlan plan = planMusicFit(ms, 112.0, req);

    QVERIFY(!plan.ok);
    QVERIFY(plan.error.find("4 bars") != std::string::npos);
  }

  void noGrid() {
    MusicStructure ms = fixture();
    ms.valid = false;

    // T = 50: ok, Trim, note contains "no beat grid"
    const FitRequest req1{.targetSec = 50.0, .mode = FitMode::Auto};
    const FitPlan plan1 = planMusicFit(ms, 112.0, req1);
    QVERIFY2(plan1.ok, qPrintable(QString::fromStdString(plan1.error)));
    QCOMPARE(plan1.modeUsed, FitMode::Trim);
    bool hasNoBeatGridNote = false;
    for (const auto& n : plan1.notes) {
      if (n.find("no beat grid") != std::string::npos) hasNoBeatGridNote = true;
    }
    QVERIFY(hasNoBeatGridNote);

    // T = 200: !ok
    const FitRequest req2{.targetSec = 200.0, .mode = FitMode::Auto};
    const FitPlan plan2 = planMusicFit(ms, 112.0, req2);
    QVERIFY(!plan2.ok);
  }

  void deterministic() {
    const MusicStructure ms = fixture();
    const FitRequest req{.targetSec = 80.0, .mode = FitMode::Auto};
    const FitPlan planA = planMusicFit(ms, 112.0, req);
    const FitPlan planB = planMusicFit(ms, 112.0, req);

    QVERIFY(planA.ok && planB.ok);
    QCOMPARE(planA.segments.size(), planB.segments.size());
    for (size_t i = 0; i < planA.segments.size(); ++i) {
      QCOMPARE(planA.segments[i].srcStartSec, planB.segments[i].srcStartSec);
      QCOMPARE(planA.segments[i].srcEndSec, planB.segments[i].srcEndSec);
    }
  }
};

QTEST_GUILESS_MAIN(TstMusicFit)
#include "tst_music_fit.moc"
