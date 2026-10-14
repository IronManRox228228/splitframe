// Measured editing on synthetic timelines: shot statistics, cut rhythm, beat alignment, speech-safe cut points, snap planning with
// source handles and word protection, gaps and overlaps. Pure functions, no engine.

#include "engine/timeline_analysis.h"
#include "test_helpers.h"

#include <QTest>

using namespace sf;
using namespace sf::engine::tl;
using namespace sf::test;

namespace {

// Video items back to back on the main track: lengths in frames, every item reads from source frame 0
TimelineDoc storyline(const std::vector<Frame>& lengths, Frame firstStart = 0) {
  TimelineDoc d = makeDoc();
  std::vector<Item> items;
  Frame at = firstStart;
  for (Frame len : lengths) {
    items.push_back(videoItem(d, at, len));
    at += len;
  }
  return docWith(d, items);
}

QString mainTrack(const TimelineDoc& d) { return trackOfKind(d, TrackKind::Video).id; }

} // namespace

class TstTimelineAnalysis : public QObject {
  Q_OBJECT
private slots:
  void statsOfKnownShots() {
    const TimelineDoc d = storyline({30, 60, 30, 90}); // 1, 2, 1, 3 s at 30 fps
    const ShotStats s = computeStats(shotsOnTrack(d, mainTrack(d)), 30.0);
    QCOMPARE(s.shotCount, 4);
    QCOMPARE(s.cuts, (std::vector<Frame>{30, 90, 120}));
    QCOMPARE(s.aslSec, 1.75);
    QCOMPARE(s.medianSec, 1.5);
    QCOMPARE(s.minSec, 1.0);
    QCOMPARE(s.maxSec, 3.0);
    QVERIFY(std::fabs(s.stdevSec - std::sqrt((0.75 * 0.75 * 2 + 0.25 * 0.25 + 1.25 * 1.25) / 4.0)) < 1e-9);
    QCOMPARE(s.durationSec, 7.0);
    QCOMPARE(s.firstCutSec, 1.0);
    QCOMPARE(s.cutsPer10s, (std::vector<int>{3}));
    QVERIFY(std::fabs(s.cutsPerMinute - 3.0 / 7.0 * 60.0) < 1e-9);
    QCOMPARE(s.shortest.size(), size_t{4});
    QCOMPARE(s.shortest.front().length, Frame{30});
  }

  void statsRhythmAndRange() {
    // 25 shots of 1 s: cuts at 1..24 s; windows of 10 s hold 9 (1..9), 10 (10..19), 5 (20..24)
    const TimelineDoc d = storyline(std::vector<Frame>(25, 30));
    const std::vector<Shot> shots = shotsOnTrack(d, mainTrack(d));
    const ShotStats all = computeStats(shots, 30.0);
    QCOMPARE(all.cutsPer10s, (std::vector<int>{9, 10, 5}));
    // a range clips the shots at its edges and counts only the cuts inside
    const ShotStats part = computeStats(shots, 30.0, 45, 105);
    QCOMPARE(part.shotCount, 3);            // shots 1 (30..60), 2 (60..90), 3 (90..120)
    QCOMPARE(part.cuts, (std::vector<Frame>{60, 90}));
    QCOMPARE(part.durationSec, 2.0);
    QCOMPARE(part.minSec, 0.5);             // the first and the last shot are cut at the range edges
    QCOMPARE(part.maxSec, 1.0);
    QCOMPARE(computeStats({}, 30.0).shotCount, 0);
    const TimelineDoc one = storyline({30});
    QCOMPARE(computeStats(shotsOnTrack(one, mainTrack(one)), 30.0).firstCutSec, -1.0);
  }

  void primaryTrackIsTheBusiestVideoTrack() {
    TimelineDoc d = storyline({30, 30, 30});
    QCOMPARE(primaryVideoTrack(d), mainTrack(d));
    QCOMPARE(primaryVideoTrack(makeDoc()), QString());
  }

  void alignmentToAGrid() {
    std::vector<Frame> grid;
    for (Frame f = 0; f <= 300; f += 15) grid.push_back(f);
    const BeatAlignment a = alignment({30, 91, 118, 143}, grid, 30.0, 2);
    QVERIFY(a.valid);
    QCOMPARE(a.within, 3); // 143 is 7 frames from the beat at 150: out
    QCOMPARE(a.withinPct, 75.0);
    QCOMPARE(a.perCut[1].beat, Frame{90});
    QCOMPARE(a.perCut[2].beat, Frame{120});
    // signed mean of (cut - beat): (0 + 1 - 2 - 7) / 4 frames
    QVERIFY(std::fabs(a.meanOffsetMs + 2.0 / 30.0 * 1000.0) < 1e-6);
    QVERIFY(std::fabs(a.meanAbsOffsetMs - 2.5 / 30.0 * 1000.0) < 1e-6);
    QVERIFY(!alignment({30}, {}, 30.0).valid);
    QCOMPARE(*nearestOnGrid(grid, 22), Frame{15}); // 7 from 15, 8 from 30
    QCOMPARE(*nearestOnGrid(grid, 30 + 7), Frame{30});
    QCOMPARE(*nearestOnGrid({10, 20}, 15), Frame{10}); // the earlier beat on a tie
    QCOMPARE(*nearestOnGrid(grid, 9999), Frame{300});
  }

  void speechSafePoints() {
    const std::vector<FrameSpan> spans{{30, 60}, {90, 120}};
    QVERIFY(!insideWord(spans, 30)); // on an edge is fine
    QVERIFY(!insideWord(spans, 60));
    QVERIFY(!insideWord(spans, 75));
    QVERIFY(insideWord(spans, 31));
    QCOMPARE(*nearestGapFrame(spans, 75, 12), Frame{75});  // already in a gap
    QCOMPARE(*nearestGapFrame(spans, 35, 12), Frame{30});  // closer to the head
    QCOMPARE(*nearestGapFrame(spans, 55, 12), Frame{60});  // closer to the tail
    QCOMPARE(*nearestGapFrame(spans, 45, 15), Frame{30});  // a tie goes to the earlier edge
    QVERIFY(!nearestGapFrame(spans, 105, 5));              // 15 from either edge
    QCOMPARE(*nearestGapFrame({}, 5, 5), Frame{5});
    QCOMPARE(*nearestGapFrame(std::vector<FrameSpan>{{0, 20}}, 4, 12), Frame{0});
  }

  void snapPlanMovesCutsOntoTheGrid() {
    const TimelineDoc d = storyline({30, 40, 30, 30}); // cuts at 30, 70, 100
    SnapInput in;
    in.doc = &d;
    in.trackId = mainTrack(d);
    for (Frame f = 0; f <= 150; f += 15) in.grid.push_back(f);
    const SnapPlan p = planSnap(in);
    QCOMPARE(p.considered, 3);
    QCOMPARE(p.alreadyOnGrid, (std::vector<Frame>{30}));
    QCOMPARE(p.skipped.size(), size_t{0});
    QCOMPARE(p.moves.size(), size_t{2});
    QCOMPARE(p.moves[0].from, Frame{70});
    QCOMPARE(p.moves[0].to, Frame{75});
    QCOMPARE(p.moves[0].shift, Frame{5});
    QCOMPARE(p.moves[1].from, Frame{100});
    QCOMPARE(p.moves[1].to, Frame{105});
    QCOMPARE(p.moves[0].leftId, shotsOnTrack(d, in.trackId)[1].itemId);
    QCOMPARE(p.moves[0].rightId, shotsOnTrack(d, in.trackId)[2].itemId);

    // only the cut at the head of the chosen item
    in.itemIds = {shotsOnTrack(d, in.trackId)[3].itemId};
    const SnapPlan one = planSnap(in);
    QCOMPARE(one.considered, 1);
    QCOMPARE(one.moves.size(), size_t{1});
    QCOMPARE(one.moves[0].from, Frame{100});
  }

  void snapPlanReasons() {
    const TimelineDoc d = storyline({30, 40, 30, 30});
    SnapInput in;
    in.doc = &d;
    in.trackId = mainTrack(d);
    for (Frame f = 0; f <= 150; f += 15) in.grid.push_back(f);
    in.maxShift = 4; // 70 -> 75 and 100 -> 105 are 5 away
    SnapPlan p = planSnap(in);
    QCOMPARE(p.moves.size(), size_t{0});
    QCOMPARE(p.skipped.size(), size_t{2});
    QCOMPARE(p.skipped[0].reason, QStringLiteral("too_far"));

    // a word at the target: refused, with the nearest gap suggested; allowWordClip overrides
    in.maxShift = 6;
    in.speech = {{72, 80}};
    p = planSnap(in);
    QCOMPARE(p.moves.size(), size_t{1}); // the cut at 100 still moves
    QCOMPARE(p.skipped.size(), size_t{1});
    QCOMPARE(p.skipped[0].reason, QStringLiteral("clips_word"));
    QCOMPARE(p.skipped[0].frame, Frame{70});
    QVERIFY(p.skipped[0].suggestedFrame.has_value());
    QCOMPARE(*p.skipped[0].suggestedFrame, Frame{72});
    in.allowWordClip = true;
    QCOMPARE(planSnap(in).moves.size(), size_t{2});
    in.allowWordClip = false;
    in.speech.clear();

    // source handles: the left shot of the cut at 70 (40 frames from source 0) has only 2 frames of media left after its out point
    in.sourceFrames[QStringLiteral("ast_video")] = 42;
    p = planSnap(in);
    QCOMPARE(p.moves.size(), size_t{1}); // the cut at 100 (left shot: 30 frames, 12 left) still moves
    QCOMPARE(p.skipped.size(), size_t{1});
    QCOMPARE(p.skipped[0].reason, QStringLiteral("exceeds_handles"));
    QCOMPARE(p.skipped[0].frame, Frame{70});
    in.sourceFrames[QStringLiteral("ast_video")] = 100;
    QCOMPARE(planSnap(in).moves.size(), size_t{2});

    // a head move earlier needs source before the in point: the items read from frame 0, so there is none
    const TimelineDoc d2 = storyline({30, 40});
    SnapInput back;
    back.doc = &d2;
    back.trackId = mainTrack(d2);
    back.grid = {0, 25, 50, 75};
    p = planSnap(back); // cut 30 -> 25 (shift -5): the right shot starts at source 0
    QCOMPARE(p.skipped.size(), size_t{1});
    QCOMPARE(p.skipped[0].reason, QStringLiteral("exceeds_handles"));
    // with handles it goes
    TimelineDoc d3 = makeDoc();
    d3 = docWith(d3, {videoItem(d3, 0, 30), videoItem(d3, 30, 40, [](ItemInit& i) { i.sourceInFrame = 20; })});
    back.doc = &d3;
    back.trackId = mainTrack(d3);
    p = planSnap(back);
    QCOMPARE(p.moves.size(), size_t{1});
    QCOMPARE(p.moves[0].shift, Frame{-5});

    // a gap between the shots is not a cut to snap
    const TimelineDoc gap = docWith(makeDoc(), {});
    TimelineDoc g2 = gap;
    g2 = docWith(g2, {videoItem(g2, 0, 30), videoItem(g2, 40, 30)});
    SnapInput gi;
    gi.doc = &g2;
    gi.trackId = mainTrack(g2);
    gi.grid = {45};
    p = planSnap(gi);
    QCOMPARE(p.skipped.size(), size_t{1});
    QCOMPARE(p.skipped[0].reason, QStringLiteral("not_abutting"));
    gi.grid.clear();
    QCOMPARE(planSnap(gi).skipped[0].reason, QStringLiteral("not_abutting")); // checked before the grid

    // never leave a sliver
    const TimelineDoc tiny = storyline({30, 4, 30});
    SnapInput ti;
    ti.doc = &tiny;
    ti.trackId = mainTrack(tiny);
    ti.grid = {33, 36, 60};
    p = planSnap(ti); // cut 30 -> 33 leaves the middle shot at 1 frame: refused
    QVERIFY(!p.skipped.empty());
    QCOMPARE(p.skipped[0].reason, QStringLiteral("too_short"));
  }

  void sequentialMovesRespectEachOther() {
    // the middle shot is 12 frames; the cuts on both sides move towards each other (6 + 6 would leave nothing)
    const TimelineDoc d = storyline({30, 12, 30});
    SnapInput in;
    in.doc = &d;
    in.trackId = mainTrack(d);
    in.grid = {36, 36 + 1}; // cut 30 -> 36, cut 42 -> 37: the middle shot would be 1 frame
    const SnapPlan p = planSnap(in);
    QCOMPARE(p.moves.size(), size_t{1});
    QCOMPARE(p.skipped.size(), size_t{1});
    QCOMPARE(p.skipped[0].reason, QStringLiteral("too_short"));
  }

  void gapsAndOverlaps() {
    TimelineDoc d = makeDoc();
    d = docWith(d, {videoItem(d, 5, 30), videoItem(d, 35, 30), videoItem(d, 70, 30), videoItem(d, 95, 30)});
    const std::vector<Shot> shots = shotsOnTrack(d, mainTrack(d));
    const std::vector<Hole> gaps = findGaps(shots);
    QCOMPARE(gaps.size(), size_t{2});
    QCOMPARE(gaps[0].frame, Frame{0}); // before the first shot
    QCOMPARE(gaps[0].frames, Frame{5});
    QCOMPARE(gaps[1].frame, Frame{65});
    QCOMPARE(gaps[1].frames, Frame{5});
    const std::vector<Hole> ov = findOverlaps(shots);
    QCOMPARE(ov.size(), size_t{1});
    QCOMPARE(ov[0].frame, Frame{95});
    QCOMPARE(ov[0].frames, Frame{5});
    QCOMPARE(findGaps({}).size(), size_t{0});
  }
};

QTEST_APPLESS_MAIN(TstTimelineAnalysis)
#include "tst_timeline_analysis.moc"
