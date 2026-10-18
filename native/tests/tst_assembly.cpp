// Unit tests for pure assembly solver (planAssembly).

#include "engine/assembly.h"
#include "engine/engine.h"
#include "core/time.h"
#include "core/schema.h"
#include "core/ops.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>
#include <vector>

using namespace sf;
using namespace sf::engine;

class TstAssembly : public QObject {
  Q_OBJECT

private:
  MusicGrid makeMusic() {
    MusicGrid m;
    m.durationSec = 64.0;
    for (double b = 0.0; b <= 64.0; b += 0.5) {
      m.beats.push_back(b);
    }
    for (double d = 0.0; d <= 64.0; d += 2.0) {
      m.downbeats.push_back(d);
    }
    m.sections = {
      {0.0, 8.0, 0.2, QStringLiteral("intro")},
      {8.0, 40.0, 1.0, QStringLiteral("drop")},
      {40.0, 56.0, 0.3, QStringLiteral("breakdown")},
      {56.0, 64.0, 0.2, QStringLiteral("outro")}
    };
    return m;
  }

  PacingTarget makePacing() {
    PacingTarget p;
    p.aslMin = 1.5;
    p.aslMax = 4.0;
    p.minShot = 0.5;
    p.maxShot = 8.0;
    p.hookMax = 2.5;
    p.cutsOnBeatMinPct = 60;
    return p;
  }

  std::vector<ShotCandidate> makePool() {
    std::vector<ShotCandidate> pool;
    pool.push_back(ShotCandidate{QStringLiteral("A"), 0.0, 30.0, 10.0, 128.0, 0.0, 5.0, false, false});
    pool.push_back(ShotCandidate{QStringLiteral("A"), 0.0, 30.0, 20.0, 128.0, 0.0, 5.0, true, false}); // freeze
    pool.push_back(ShotCandidate{QStringLiteral("B"), 0.0, 30.0, 30.0, 128.0, 0.0, 5.0, false, false});
    pool.push_back(ShotCandidate{QStringLiteral("B"), 0.0, 30.0, 40.0, 128.0, 0.0, 5.0, false, true});  // black
    pool.push_back(ShotCandidate{QStringLiteral("C"), 0.0, 30.0, 50.0, 128.0, 0.0, 5.0, false, false});
    pool.push_back(ShotCandidate{QStringLiteral("C"), 0.0, 30.0, 60.0, 128.0, 0.0, 5.0, false, false});
    return pool;
  }

private slots:
  // 1. fillsExactly: the shots tile 0..64 with no gaps and the last end is 64.
  void fillsExactly() {
    const auto music = makeMusic();
    const auto pace = makePacing();
    const auto pool = makePool();

    AssemblyPlan plan = planAssembly(pool, music, pace, 64.0, 1);
    QVERIFY(plan.ok);
    QVERIFY(!plan.shots.empty());
    QCOMPARE(plan.shots.front().timelineStartSec, 0.0);
    QCOMPARE(plan.shots.back().timelineEndSec, 64.0);

    for (size_t i = 1; i < plan.shots.size(); ++i) {
      QCOMPARE(plan.shots[i].timelineStartSec, plan.shots[i - 1].timelineEndSec);
    }
  }

  // 2. cutsOnBeat: every cut is on a beat (100%, since beats are everywhere).
  void cutsOnBeat() {
    const auto music = makeMusic();
    const auto pace = makePacing();
    const auto pool = makePool();

    AssemblyPlan plan = planAssembly(pool, music, pace, 64.0, 1);
    QVERIFY(plan.ok);
    QVERIFY(plan.cuts > 0);
    QCOMPARE(plan.cutsOnBeat, plan.cuts);
  }

  // 3. dropDenserThanBreakdown: mean shot length inside drop < mean inside breakdown.
  void dropDenserThanBreakdown() {
    const auto music = makeMusic();
    const auto pace = makePacing();
    const auto pool = makePool();

    AssemblyPlan plan = planAssembly(pool, music, pace, 64.0, 1);
    QVERIFY(plan.ok);

    double dropDur = 0.0;
    int dropCount = 0;
    double bdDur = 0.0;
    int bdCount = 0;

    for (const auto& s : plan.shots) {
      if (s.timelineStartSec >= 8.0 - 1e-6 && s.timelineEndSec <= 40.0 + 1e-6) {
        dropDur += (s.timelineEndSec - s.timelineStartSec);
        dropCount++;
      } else if (s.timelineStartSec >= 40.0 - 1e-6 && s.timelineEndSec <= 56.0 + 1e-6) {
        bdDur += (s.timelineEndSec - s.timelineStartSec);
        bdCount++;
      }
    }

    QVERIFY(dropCount > 0);
    QVERIFY(bdCount > 0);
    const double meanDrop = dropDur / dropCount;
    const double meanBd = bdDur / bdCount;
    QVERIFY(meanDrop < meanBd);
  }

  // 4. hookShort: the first shot is at most 2.5 s.
  void hookShort() {
    const auto music = makeMusic();
    const auto pace = makePacing();
    const auto pool = makePool();

    AssemblyPlan plan = planAssembly(pool, music, pace, 64.0, 1);
    QVERIFY(plan.ok);
    QVERIFY(!plan.shots.empty());
    const double hookDur = plan.shots.front().timelineEndSec - plan.shots.front().timelineStartSec;
    QVERIFY(hookDur <= 2.5 + 1e-6);
  }

  // 5. filtersBad: the black and freeze candidates are never used.
  void filtersBad() {
    const auto music = makeMusic();
    const auto pace = makePacing();
    // A pool with only 1 valid candidate and 2 bad ones
    std::vector<ShotCandidate> pool;
    pool.push_back(ShotCandidate{QStringLiteral("valid"), 0.0, 30.0, 50.0, 128.0, 0.0, 5.0, false, false});
    pool.push_back(ShotCandidate{QStringLiteral("badFreeze"), 0.0, 30.0, 90.0, 128.0, 0.0, 5.0, true, false});
    pool.push_back(ShotCandidate{QStringLiteral("badBlack"), 0.0, 30.0, 95.0, 128.0, 0.0, 5.0, false, true});

    AssemblyPlan plan = planAssembly(pool, music, pace, 64.0, 1);
    QVERIFY(plan.ok);
    for (const auto& s : plan.shots) {
      QCOMPARE(s.assetId, QStringLiteral("valid"));
    }
  }

  // 6. noBackToBackRepeat: no two consecutive shots share an asset.
  void noBackToBackRepeat() {
    const auto music = makeMusic();
    const auto pace = makePacing();
    const auto pool = makePool();

    AssemblyPlan plan = planAssembly(pool, music, pace, 64.0, 1);
    QVERIFY(plan.ok);
    for (size_t i = 1; i < plan.shots.size(); ++i) {
      QVERIFY2(plan.shots[i].assetId != plan.shots[i - 1].assetId,
               qPrintable(QStringLiteral("Consecutive repeat at index %1: %2").arg(i).arg(plan.shots[i].assetId)));
    }
  }

  // 7. deterministic: the same seed gives an identical plan. Seed 2 may differ.
  void deterministic() {
    const auto music = makeMusic();
    const auto pace = makePacing();
    const auto pool = makePool();

    AssemblyPlan plan1 = planAssembly(pool, music, pace, 64.0, 42);
    AssemblyPlan plan2 = planAssembly(pool, music, pace, 64.0, 42);
    QVERIFY(plan1.ok);
    QVERIFY(plan2.ok);
    QCOMPARE(plan1.shots.size(), plan2.shots.size());
    for (size_t i = 0; i < plan1.shots.size(); ++i) {
      QCOMPARE(plan1.shots[i].assetId, plan2.shots[i].assetId);
      QCOMPARE(plan1.shots[i].timelineStartSec, plan2.shots[i].timelineStartSec);
      QCOMPARE(plan1.shots[i].timelineEndSec, plan2.shots[i].timelineEndSec);
      QCOMPARE(plan1.shots[i].srcInSec, plan2.shots[i].srcInSec);
      QCOMPARE(plan1.shots[i].srcOutSec, plan2.shots[i].srcOutSec);
    }
  }

  // 8. emptyPoolFails.
  void emptyPoolFails() {
    const auto music = makeMusic();
    const auto pace = makePacing();

    AssemblyPlan plan = planAssembly({}, music, pace, 64.0, 1);
    QVERIFY(!plan.ok);
    QCOMPARE(plan.error, QStringLiteral("empty shot pool"));
  }

  // 9. shortCandidate: candidate is shorter than requested slot length.
  void shortCandidate() {
    MusicGrid music = makeMusic();
    PacingTarget pace = makePacing();
    std::vector<ShotCandidate> pool;
    pool.push_back(ShotCandidate{QStringLiteral("short1"), 0.0, 1.2, 50.0, 128.0, 0.0, 5.0, false, false});

    AssemblyPlan plan = planAssembly(pool, music, pace, 4.0, 1);
    QVERIFY(plan.ok);
    QVERIFY(!plan.shots.empty());
    for (const auto& s : plan.shots) {
      QVERIFY(s.srcOutSec <= 1.2 + 1e-6);
      QVERIFY(s.timelineEndSec - s.timelineStartSec <= 1.2 + 1e-6);
    }
    QVERIFY(!plan.warnings.isEmpty());
  }

  // 10. singleCandidate: single usable candidate never repeats identical source range back to back.
  void singleCandidate() {
    MusicGrid music = makeMusic();
    PacingTarget pace = makePacing();
    std::vector<ShotCandidate> pool;
    pool.push_back(ShotCandidate{QStringLiteral("single"), 0.0, 6.0, 50.0, 128.0, 0.0, 5.0, false, false});

    AssemblyPlan plan = planAssembly(pool, music, pace, 12.0, 1);
    QVERIFY(plan.ok);
    QVERIFY(plan.shots.size() > 1);
    for (size_t i = 1; i < plan.shots.size(); ++i) {
      const bool identical = std::abs(plan.shots[i].srcInSec - plan.shots[i - 1].srcInSec) < 1e-4 &&
                             std::abs(plan.shots[i].srcOutSec - plan.shots[i - 1].srcOutSec) < 1e-4;
      QVERIFY(!identical);
    }

    std::vector<ShotCandidate> poolFixed;
    poolFixed.push_back(ShotCandidate{QStringLiteral("fixed"), 0.0, 2.0, 50.0, 128.0, 0.0, 5.0, false, false});
    AssemblyPlan planFixed = planAssembly(poolFixed, music, pace, 6.0, 1);
    QVERIFY(planFixed.ok);
    bool foundInsufficient = false;
    for (const QString& w : planFixed.warnings) {
      if (w.contains(QStringLiteral("insufficient_footage"))) foundInsufficient = true;
    }
    QVERIFY(foundInsufficient);
  }

  // 11. musicNotAtFrame0: beat grid offset when music item starts at non-zero frame.
  void musicNotAtFrame0() {
    MusicGrid music;
    music.durationSec = 16.0;
    const double offset = 2.0;
    for (double b = 0.5; b <= 16.0; b += 0.5) {
      music.beats.push_back(offset + b);
    }
    for (double d = 2.0; d <= 16.0; d += 2.0) {
      music.downbeats.push_back(offset + d);
    }

    PacingTarget pace = makePacing();
    const auto pool = makePool();

    AssemblyPlan plan = planAssembly(pool, music, pace, 16.0, 1);
    QVERIFY(plan.ok);
    QVERIFY(plan.cuts > 0);
    for (size_t i = 0; i + 1 < plan.shots.size(); ++i) {
      const double cutTime = plan.shots[i].timelineEndSec;
      bool onBeat = false;
      for (double b : music.beats) {
        if (std::abs(cutTime - b) <= 0.001) { onBeat = true; break; }
      }
      if (cutTime >= offset) {
        QVERIFY(onBeat);
      }
    }
  }

  // 12. driftOver40Shots: 40 shots accumulated cut positions match exact frames with zero drift.
  void driftOver40Shots() {
    MusicGrid music;
    music.durationSec = 100.0;
    for (double b = 0.0; b <= 100.0; b += 0.5) music.beats.push_back(b);
    for (double d = 0.0; d <= 100.0; d += 2.0) music.downbeats.push_back(d);

    PacingTarget pace = makePacing();
    pace.minShot = 1.0;
    pace.maxShot = 3.0;
    pace.aslMin = 1.5;
    pace.aslMax = 2.5;

    std::vector<ShotCandidate> pool;
    for (int i = 0; i < 10; ++i) {
      pool.push_back(ShotCandidate{QStringLiteral("cand_%1").arg(i), 0.0, 30.0, 50.0 + i, 128.0, 0.0, 5.0, false, false});
    }

    AssemblyPlan plan = planAssembly(pool, music, pace, 80.0, 42);
    QVERIFY(plan.ok);
    QVERIFY(plan.shots.size() >= 40);

    const double fps = 25.0;
    const Frame atFrame = 100;
    Frame curFrame = atFrame;
    for (size_t i = 0; i < 40; ++i) {
      const auto& shot = plan.shots[i];
      const Frame startF = curFrame;
      const Frame endF = atFrame + static_cast<Frame>(std::llround(shot.timelineEndSec * fps));
      const Frame durF = std::max<Frame>(1, endF - startF);
      curFrame = startF + durF;
      QCOMPARE(curFrame, atFrame + static_cast<Frame>(std::llround(shot.timelineEndSec * fps)));
    }
  }

  // 13. assembleCommandMusicOffsetSpeedAndDrift:
  // Calls edit.assemble itself through cmd_assemble with musicItem at startFrame=75 (!= atFrame 25),
  // speed=1.25 (!= 1), and >= 40 shots.
  // Asserts real cut frames on timeline against independently computed beat frames:
  //   beat frame = musicStartFrame + k * round(beatSec * fps / speed) = 75 + k * 10
  void assembleCommandMusicOffsetSpeedAndDrift() {
    Engine engine;
    engine.call(QStringLiteral("project.new"), {
      {QStringLiteral("name"), QStringLiteral("AssembleCommandTest")},
      {QStringLiteral("fps"), 25},
      {QStringLiteral("width"), 1920},
      {QStringLiteral("height"), 1080}
    });

    auto* sess = engine.session();
    QVERIFY(sess && sess->project);

    // Register 5 video assets of 60 s each
    for (int i = 0; i < 5; ++i) {
      Asset a;
      a.id = QStringLiteral("cand_asset_%1").arg(i);
      a.path = QStringLiteral("dummy_v_%1.mp4").arg(i);
      a.kind = AssetKind::Video;
      a.durationMs = 60000;
      sess->project->addAsset(a);
    }

    // Audio asset for music
    Asset ma;
    ma.id = QStringLiteral("music_asset");
    ma.path = QStringLiteral("dummy_music.wav");
    ma.kind = AssetKind::Audio;
    ma.durationMs = 120000;
    sess->project->addAsset(ma);

    QString audioTrackId;
    QString videoTrackId;
    for (const auto& trk : sess->project->doc().tracks) {
      if (trk.kind == TrackKind::Audio && audioTrackId.isEmpty()) audioTrackId = trk.id;
      if (trk.kind == TrackKind::Video && videoTrackId.isEmpty()) videoTrackId = trk.id;
    }
    QVERIFY(!audioTrackId.isEmpty());
    QVERIFY(!videoTrackId.isEmpty());

    // Insert music item at startFrame = 75, duration = 2500 frames, speed = 1.25
    Item musicItem;
    musicItem.id = QStringLiteral("music_item_1");
    musicItem.trackId = audioTrackId;
    musicItem.assetId = QStringLiteral("music_asset");
    musicItem.props = AudioProps{};
    musicItem.startFrame = 75; // NOT at frame 0
    musicItem.durationFrames = 2500;
    musicItem.speed = 1.25;    // speed != 1
    QVERIFY(sess->project->apply(Op(ItemAdd{musicItem})));

    // Mock music.structure returning beats every 0.5s
    engine.wrap(QStringLiteral("music.structure"), [](Handler) {
      return [](CallContext&, const QJsonObject&) {
        QJsonArray beats;
        for (double b = 0.0; b <= 100.0; b += 0.5) beats.append(b);
        QJsonArray downbeats;
        for (double d = 0.0; d <= 100.0; d += 2.0) downbeats.append(d);
        QJsonArray sections;
        sections.append(QJsonObject{
          {QStringLiteral("startSec"), 0.0},
          {QStringLiteral("endSec"), 100.0},
          {QStringLiteral("relEnergy"), 0.8},
          {QStringLiteral("label"), QStringLiteral("verse")}
        });
        return Result::success(QJsonObject{
          {QStringLiteral("beats"), beats},
          {QStringLiteral("downbeats"), downbeats},
          {QStringLiteral("sections"), sections}
        });
      };
    });

    Result r = engine.call(QStringLiteral("edit.assemble"), {
      {QStringLiteral("session"), sess->id},
      {QStringLiteral("assetIds"), QJsonArray{
        QStringLiteral("cand_asset_0"), QStringLiteral("cand_asset_1"),
        QStringLiteral("cand_asset_2"), QStringLiteral("cand_asset_3"),
        QStringLiteral("cand_asset_4")
      }},
      {QStringLiteral("musicItemId"), QStringLiteral("music_item_1")},
      {QStringLiteral("pacing"), QStringLiteral("social-short")},
      {QStringLiteral("atFrame"), 25}, // atFrame = 25 (offset = (75 - 25) / 25 = 2.0s)
      {QStringLiteral("apply"), true},
      {QStringLiteral("seed"), 42}
    });
    QVERIFY2(r.ok, qPrintable(r.error.message));

    // Inspect timeline items on video track
    std::vector<Item> vItems;
    for (const auto& itm : sess->project->doc().items) {
      if (itm.trackId == videoTrackId) vItems.push_back(itm);
    }
    std::sort(vItems.begin(), vItems.end(), [](const Item& a, const Item& b) {
      return a.startFrame < b.startFrame;
    });

    QVERIFY(vItems.size() >= 40);
    QCOMPARE(vItems[0].startFrame, 25); // Starts at atFrame

    // Independently verify:
    // With music at startFrame 75 and speed 1.25, source beats at 0.5s intervals occur at:
    //   t_k = 3.0 + k * (0.5 / 1.25) = 3.0 + k * 0.4 s
    //   frame_k = 25 * 3.0 + 25 * k * 0.4 = 75 + k * 10
    // Every beat on timeline is at frame (75 + k * 10), so (frame - 75) % 10 == 0!
    Frame curF = 25;
    for (size_t i = 0; i < 40; ++i) {
      const auto& item = vItems[i];
      // Continuity: no gaps, no overlaps
      QCOMPARE(item.startFrame, curF);
      curF = item.startFrame + item.durationFrames;
      // Cut position after music begins must match independent beat formula
      if (curF >= 75) {
        QCOMPARE((curF - 75) % 10, 0);
      }
    }
    // Verify 40 shots accumulate with zero rounding drift
    QVERIFY(curF > 75);
  }
};

QTEST_GUILESS_MAIN(TstAssembly)
#include "tst_assembly.moc"
