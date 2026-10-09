#include "editor/timeline_controller.h"
#include "editor_testutil.h"

#include <QSignalSpy>

namespace tst {
using namespace sf;
using namespace sf::editor;
using namespace sf::test;
using sf::editor::Project; // not sf::Project

namespace {
const QString V1 = QStringLiteral("itm_v1"), V2 = QStringLiteral("itm_v2"), A1 = QStringLiteral("itm_a1"), T1 = QStringLiteral("itm_t1");
} // namespace

class TstTimelineController : public QObject {
  Q_OBJECT

  static void lock(Project& p, TrackKind kind, bool on = true) {
    TrackUpdate u;
    u.trackId = trackNamed(p.doc(), kind).id;
    u.patch.locked = on;
    QVERIFY(p.apply(u));
  }

private slots:
  // ---- move ----
  void dragMoveWithinTrackCommitsOneMoveAndUndoes() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    c.setSnapEnabled(false);
    const int video = trackIndex(p.doc(), TrackKind::Video);
    QVERIFY(c.beginMove(V2, 130, false)); // grab 10 frames into the clip
    QVERIFY(c.dragging());
    c.updateMove(190, video);
    QCOMPARE(c.ghosts().front().start, Frame(180));
    QCOMPARE(itemOf(p, V2).startFrame, Frame(120)); // document untouched while dragging
    QVERIFY(c.commitDrag());
    QCOMPARE(itemOf(p, V2).startFrame, Frame(180));
    QVERIFY(!c.dragging());
    QVERIFY(p.undo());
    QCOMPARE(itemOf(p, V2).startFrame, Frame(120));
    QVERIFY(!p.canUndo() || p.undoLabel() != QStringLiteral("Move clip"));
  }
  void clickWithoutMovingIsNotAnEdit() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    QVERIFY(c.beginMove(V1, 10, false));
    QVERIFY(!c.commitDrag());
    QVERIFY(!p.canUndo());
    QCOMPARE(p.selection(), QStringList{V1});
  }
  void moveCannotGoBeforeZero() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    c.setSnapEnabled(false);
    QVERIFY(c.beginMove(V2, 130, false));
    c.updateMove(-50, trackIndex(p.doc(), TrackKind::Video));
    QCOMPARE(c.ghosts().front().start, Frame(0));
  }
  void moveAcrossTracksHonoursTrackAllowsItem() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    c.setSnapEnabled(false);
    const int text = trackIndex(p.doc(), TrackKind::Text);
    const int audio = trackIndex(p.doc(), TrackKind::Audio);
    const int video = trackIndex(p.doc(), TrackKind::Video);
    // a video clip may not go to the audio track: ghost stays, the pointer is flagged
    QVERIFY(c.beginMove(V2, 130, false));
    c.updateMove(130, audio);
    QCOMPARE(c.ghosts().front().trackIndex, video);
    QVERIFY(c.hoverBlocked());
    c.cancelDrag();
    // a text clip may not go to video either (text kinds: text, caption, shape, motion)
    QVERIFY(c.beginMove(T1, 40, false));
    c.updateMove(40, video);
    QVERIFY(c.hoverBlocked());
    c.cancelDrag();
    QCOMPARE(itemOf(p, T1).trackId, trackNamed(p.doc(), TrackKind::Text).id);
    Q_UNUSED(text);
  }
  void moveBetweenCompatibleTracks() {
    Project p;
    fillProject(p);
    TrackAdd add;
    add.trackId = QStringLiteral("trk_v2");
    add.kind = TrackKind::Video;
    add.name = QStringLiteral("V2");
    add.index = 0;
    QVERIFY(p.apply(add));
    TimelineController c(p);
    c.setSnapEnabled(false);
    QVERIFY(c.beginMove(V1, 5, false));
    c.updateMove(5, 0); // the new video track is on top
    QVERIFY(!c.hoverBlocked());
    QVERIFY(c.commitDrag());
    QCOMPARE(itemOf(p, V1).trackId, QStringLiteral("trk_v2"));
    QCOMPARE(itemOf(p, V1).startFrame, Frame(0));
  }
  void lockedTrackRefusesDrags() {
    Project p;
    fillProject(p);
    lock(p, TrackKind::Video);
    TimelineController c(p);
    QSignalSpy msg(&c, &TimelineController::message);
    QVERIFY(!c.beginMove(V1, 5, false));
    QVERIFY(!c.beginTrim(V1, true, false));
    QCOMPARE(msg.count(), 2);
    QVERIFY(msg.first().at(0).toString().contains(QStringLiteral("locked")));
    QCOMPARE(msg.first().at(1).toString(), QStringLiteral("error"));
    QVERIFY(!c.dragging());
    QCOMPARE(p.selection(), QStringList{V1}); // still selectable
  }
  void dropOnLockedTrackIsBlocked() {
    Project p;
    fillProject(p);
    TrackAdd add;
    add.trackId = QStringLiteral("trk_v2");
    add.kind = TrackKind::Video;
    add.name = QStringLiteral("V2");
    add.locked = true;
    add.index = 0;
    QVERIFY(p.apply(add));
    TimelineController c(p);
    c.setSnapEnabled(false);
    QVERIFY(c.beginMove(V1, 5, false));
    c.updateMove(5, 0);
    QVERIFY(c.hoverBlocked());
    QCOMPARE(c.ghosts().front().trackIndex, 2); // stays on its own (video) track
    c.cancelDrag();
  }
  void groupMoveShiftsEveryMember() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    c.setSnapEnabled(false);
    p.setSelection({V1, V2});
    QVERIFY(c.beginMove(V1, 10, false));
    QCOMPARE(c.ghosts().size(), size_t(2));
    c.updateMove(40, trackIndex(p.doc(), TrackKind::Video)); // +30
    QVERIFY(c.commitDrag());
    QCOMPARE(itemOf(p, V1).startFrame, Frame(30));
    QCOMPARE(itemOf(p, V2).startFrame, Frame(150));
    QVERIFY(p.undo()); // one step for the whole group
    QCOMPARE(itemOf(p, V1).startFrame, Frame(0));
    QCOMPARE(itemOf(p, V2).startFrame, Frame(120));
  }
  void groupMoveStopsAtZeroForEveryone() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    c.setSnapEnabled(false);
    p.setSelection({V1, V2});
    QVERIFY(c.beginMove(V2, 130, false));
    c.updateMove(-100, trackIndex(p.doc(), TrackKind::Video));
    QVERIFY(!c.commitDrag()); // v1 is already at 0, so the group cannot move left at all
    QCOMPARE(itemOf(p, V1).startFrame, Frame(0));
    QCOMPARE(itemOf(p, V2).startFrame, Frame(120)); // the group did not move: v1 is already at 0
  }
  void plainClickInsideSelectionNarrowsIt() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    p.setSelection({V1, V2});
    QVERIFY(c.beginMove(V1, 10, false));
    QVERIFY(!c.commitDrag());
    QCOMPARE(p.selection(), QStringList{V1});
  }
  void shiftClickTogglesSelection() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    QVERIFY(c.beginMove(V1, 10, false));
    c.cancelDrag();
    QVERIFY(c.beginMove(V2, 130, true));
    c.cancelDrag();
    QCOMPARE(p.selection().size(), 2);
    QVERIFY(!c.beginMove(V2, 130, true)); // shift-click on a selected clip only deselects
    QCOMPARE(p.selection(), QStringList{V1});
  }

  // ---- snapping ----
  void movingSnapsToNeighbourEdgeAndShowsGuide() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    c.setSnapThresholdFrames(5);
    const int video = trackIndex(p.doc(), TrackKind::Video);
    QVERIFY(c.beginMove(V2, 120, false)); // grab at the clip start
    c.updateMove(93, video);              // 3 frames past v1's end (90)
    QCOMPARE(c.ghosts().front().start, Frame(90));
    QCOMPARE(c.guideFrame(), qint64(90));
    c.updateMove(110, video); // outside the threshold of 90 and of the audio-excluded edges
    QCOMPARE(c.ghosts().front().start, Frame(110));
    QCOMPARE(c.guideFrame(), qint64(-1));
    c.cancelDrag();
  }
  void movingSnapsItsOutEdgeToo() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    c.setSnapThresholdFrames(5);
    // t1 lasts 60: its end (87 here) should snap to v1's end (90), pulling the start back to 30
    QVERIFY(c.beginMove(T1, 30, false));
    c.updateMove(27, trackIndex(p.doc(), TrackKind::Text));
    QCOMPARE(c.ghosts().front().start, Frame(30));
    QCOMPARE(c.guideFrame(), qint64(90));
    c.cancelDrag();
  }
  void snapsToThePlayhead() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    c.setSnapThresholdFrames(4);
    c.setPlayhead(300);
    const int video = trackIndex(p.doc(), TrackKind::Video);
    QVERIFY(c.beginMove(V2, 120, false));
    c.updateMove(302, video);
    QCOMPARE(c.ghosts().front().start, Frame(300));
    c.cancelDrag();
  }
  void snapOffIsExact() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    c.setSnapEnabled(false);
    QVERIFY(c.beginMove(V2, 120, false));
    c.updateMove(93, trackIndex(p.doc(), TrackKind::Video));
    QCOMPARE(c.ghosts().front().start, Frame(93));
    QCOMPARE(c.guideFrame(), qint64(-1));
    c.cancelDrag();
  }

  // ---- trim ----
  void trimOutEdge() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    c.setSnapEnabled(false);
    QVERIFY(c.beginTrim(V1, false, false));
    c.updateTrim(60);
    QCOMPARE(c.ghosts().front().duration, Frame(60));
    QVERIFY(c.commitDrag());
    QCOMPARE(itemOf(p, V1).durationFrames, Frame(60));
    QCOMPARE(itemOf(p, V2).startFrame, Frame(120)); // no ripple: the neighbour stays
    QVERIFY(p.undo());
    QCOMPARE(itemOf(p, V1).durationFrames, Frame(90));
  }
  void trimInEdgeAdvancesSourceIn() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    c.setSnapEnabled(false);
    QVERIFY(c.beginTrim(V2, true, false));
    c.updateTrim(150);
    QVERIFY(c.commitDrag());
    QCOMPARE(itemOf(p, V2).startFrame, Frame(150));
    QCOMPARE(itemOf(p, V2).durationFrames, Frame(60));
    QCOMPARE(itemOf(p, V2).sourceInFrame.value_or(-1), Frame(30));
  }
  void rippleTrimShiftsDownstream() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    c.setSnapEnabled(false);
    c.setRippleEnabled(true);
    QVERIFY(c.beginTrim(V1, false, false));
    c.updateTrim(60);
    QVERIFY(c.commitDrag());
    QCOMPARE(itemOf(p, V1).durationFrames, Frame(60));
    QCOMPARE(itemOf(p, V2).startFrame, Frame(90)); // closed the 30-frame gap
    QVERIFY(p.undo());
    QCOMPARE(itemOf(p, V2).startFrame, Frame(120));
  }
  void trimWithoutMovementOrToSameEdgeIsNoOp() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    c.setSnapEnabled(false);
    QVERIFY(c.beginTrim(V1, false, false));
    QVERIFY(!c.commitDrag());
    QVERIFY(c.beginTrim(V1, false, false));
    c.updateTrim(90);
    QVERIFY(!c.commitDrag());
    QVERIFY(!p.canUndo());
  }
  void trimSnapsToEdges() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    c.setSnapThresholdFrames(5);
    QVERIFY(c.beginTrim(V1, false, false));
    c.updateTrim(117);
    QCOMPARE(c.guideFrame(), qint64(120));
    QVERIFY(c.commitDrag());
    QCOMPARE(itemOf(p, V1).durationFrames, Frame(120));
  }

  // ---- commands ----
  void splitAtPlayheadSplitsClipsUnderIt() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    c.setPlayhead(45);
    QVERIFY(c.splitAtPlayhead());
    // v1, a1 and t1 all span frame 45
    QCOMPARE(p.doc().items.size(), size_t(7));
    QCOMPARE(itemOf(p, V1).durationFrames, Frame(45));
    QCOMPARE(p.selection().size(), 3); // the new right halves
    QVERIFY(p.undo()); // one step
    QCOMPARE(p.doc().items.size(), size_t(4));
  }
  void splitRespectsSelection() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    c.setPlayhead(45);
    p.setSelection({V1});
    QVERIFY(c.splitAtPlayhead());
    QCOMPARE(p.doc().items.size(), size_t(5));
    QCOMPARE(itemOf(p, A1).durationFrames, Frame(200));
  }
  void splitAtClipEdgeOrEmptySpaceDoesNothing() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    QSignalSpy msg(&c, &TimelineController::message);
    c.setPlayhead(90); // v1's end: no clip is split (a1 and t1... t1 ends at 90 too, a1 spans it)
    QVERIFY(c.splitAtPlayhead());
    QCOMPARE(p.doc().items.size(), size_t(5)); // only the audio clip
    c.setPlayhead(1000);
    QVERIFY(!c.splitAtPlayhead());
    QCOMPARE(msg.count(), 1);
  }
  void splitSkipsLockedTracks() {
    Project p;
    fillProject(p);
    lock(p, TrackKind::Audio);
    TimelineController c(p);
    c.setPlayhead(45);
    QVERIFY(c.splitAtPlayhead());
    QCOMPARE(itemOf(p, A1).durationFrames, Frame(200));
    QCOMPARE(itemOf(p, V1).durationFrames, Frame(45));
  }
  void deleteAndRippleDelete() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    QSignalSpy msg(&c, &TimelineController::message);
    p.setSelection({V1});
    QVERIFY(c.deleteSelection());
    QVERIFY(!getItem(p.doc(), V1));
    QCOMPARE(itemOf(p, V2).startFrame, Frame(120));
    QCOMPARE(msg.last().at(1).toString(), QStringLiteral("undo"));
    QVERIFY(p.undo());
    QVERIFY(getItem(p.doc(), V1));

    c.setRippleEnabled(true);
    p.setSelection({V1});
    QVERIFY(c.deleteSelection());
    QCOMPARE(itemOf(p, V2).startFrame, Frame(30)); // v1 (90 long) closed up: 120 - 90
  }
  void deleteSkipsLockedTrackClips() {
    Project p;
    fillProject(p);
    lock(p, TrackKind::Video);
    TimelineController c(p);
    QSignalSpy msg(&c, &TimelineController::message);
    p.setSelection({V1});
    QVERIFY(!c.deleteSelection());
    QVERIFY(getItem(p.doc(), V1));
    QCOMPARE(msg.last().at(1).toString(), QStringLiteral("error"));
    p.setSelection({V1, A1});
    QVERIFY(c.deleteSelection());
    QVERIFY(getItem(p.doc(), V1));
    QVERIFY(!getItem(p.doc(), A1));
  }
  void cloneNudgeSelectAll() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    p.setSelection({V1});
    QVERIFY(c.cloneSelection());
    QCOMPARE(p.doc().items.size(), size_t(5));
    c.selectAll();
    QCOMPARE(p.selection().size(), 5);
    c.deselect();
    QVERIFY(p.selection().isEmpty());

    p.setSelection({V2});
    QVERIFY(c.nudgeSelection(7));
    QCOMPARE(itemOf(p, V2).startFrame, Frame(127));
    p.setSelection({A1});
    QVERIFY(!c.nudgeSelection(-5)); // already at 0
    QVERIFY(c.nudgeSelection(3));
    QVERIFY(c.nudgeSelection(-10)); // clamps to 0
    QCOMPARE(itemOf(p, A1).startFrame, Frame(0));
  }
  void marqueeSelectsIntersectingClips() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    // lanes: text 0-44, video 44-108, audio 108-156
    c.beginMarquee(false);
    c.updateMarquee(10, 50, 100, 120); // video row and the top of audio, frames 10..100
    QVERIFY(p.isSelected(V1));
    QVERIFY(p.isSelected(A1));
    QVERIFY(!p.isSelected(V2));
    QVERIFY(!p.isSelected(T1));
    c.beginMarquee(true);
    c.updateMarquee(125, 50, 130, 60);
    QVERIFY(p.isSelected(V2));
    QVERIFY(p.isSelected(V1)); // additive keeps the base
  }

  // ---- adding media ----
  void addAssetsLandOnTheRightTrackWithDuration() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    c.setSnapEnabled(false);
    QVERIFY(c.addAsset(QStringLiteral("ast_video"), 300));
    QCOMPARE(p.selection().size(), 1);
    const Item& v = itemOf(p, p.selection().first());
    QCOMPARE(v.trackId, trackNamed(p.doc(), TrackKind::Video).id);
    QCOMPARE(v.startFrame, Frame(300));
    QCOMPARE(v.durationFrames, Frame(90)); // 3 s at 30 fps
    QCOMPARE(v.sourceInFrame.value_or(-1), Frame(0));
    QCOMPARE(v.labels.name.value_or(QString()), QStringLiteral("clip.mp4"));
    QVERIFY(c.addAsset(QStringLiteral("ast_music"), 0));
    const Item& a = itemOf(p, p.selection().first());
    QCOMPARE(a.durationFrames, Frame(300));
    QCOMPARE(a.type(), ItemType::Audio);
    QVERIFY(c.addAsset(QStringLiteral("ast_still"), 500));
    QCOMPARE(itemOf(p, p.selection().first()).durationFrames, Frame(150)); // 5 s still
    QVERIFY(p.undo());
    QVERIFY(p.selection().isEmpty());
  }
  void addAssetOntoSpecificTrackAndSnap() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    c.setSnapThresholdFrames(6);
    // dropping on the audio track a video clip falls back to the video track; start snaps to v2's end (210)
    QVERIFY(c.addAsset(QStringLiteral("ast_video"), 213, trackIndex(p.doc(), TrackKind::Audio)));
    const Item& v = itemOf(p, p.selection().first());
    QCOMPARE(v.trackId, trackNamed(p.doc(), TrackKind::Video).id);
    QCOMPARE(v.startFrame, Frame(210));
    QCOMPARE(c.snappedDropStart(QStringLiteral("ast_video"), 213), qint64(210));
  }
  void addAssetToLockedTrackIsRefused() {
    Project p;
    fillProject(p);
    lock(p, TrackKind::Audio);
    TimelineController c(p);
    QSignalSpy msg(&c, &TimelineController::message);
    QVERIFY(!c.addAsset(QStringLiteral("ast_music"), 0, trackIndex(p.doc(), TrackKind::Audio)));
    QVERIFY(msg.count() >= 1);
    QCOMPARE(p.doc().items.size(), size_t(4));
    QVERIFY(!c.addAsset(QStringLiteral("ast_nope"), 0));
  }
  void addTextAndShape() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    c.setPlayhead(10);
    QVERIFY(c.addText(QStringLiteral("Hi there")));
    const Item& t = itemOf(p, p.selection().first());
    QCOMPARE(t.type(), ItemType::Text);
    QCOMPARE(t.startFrame, Frame(10));
    QCOMPARE(t.durationFrames, Frame(90));
    QCOMPARE(std::get<TextProps>(t.props).text, QStringLiteral("Hi there"));
    QVERIFY(c.addShape());
    QCOMPARE(itemOf(p, p.selection().first()).type(), ItemType::Shape);
  }

  // ---- tracks ----
  void trackOperations() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    QVERIFY(c.addTrack(QStringLiteral("video")));
    QCOMPARE(p.doc().tracks.size(), size_t(4));
    QCOMPARE(p.doc().tracks.front().kind, TrackKind::Video);
    QCOMPARE(p.doc().tracks.front().name, QStringLiteral("Video 2"));
    QVERIFY(c.addTrack(QStringLiteral("audio")));
    QCOMPARE(p.doc().tracks.back().kind, TrackKind::Audio);
    QVERIFY(!c.addTrack(QStringLiteral("hologram")));

    const QString first = p.doc().tracks[0].id;
    QVERIFY(c.moveTrack(0, 2));
    QCOMPARE(p.doc().tracks[2].id, first);
    QVERIFY(p.undo());
    QCOMPARE(p.doc().tracks[0].id, first);

    QVERIFY(c.setTrackFlag(0, QStringLiteral("hidden"), true));
    QVERIFY(p.doc().tracks[0].hidden);
    QVERIFY(c.setTrackFlag(0, QStringLiteral("muted"), true));
    QVERIFY(c.setTrackFlag(0, QStringLiteral("locked"), true));
    QVERIFY(!c.removeTrack(0)); // locked
    QVERIFY(c.renameTrack(1, QStringLiteral("Renamed")));
    QCOMPARE(p.doc().tracks[1].name, QStringLiteral("Renamed"));
    QVERIFY(c.setTrackFlag(0, QStringLiteral("locked"), false));
    QVERIFY(c.removeTrack(0));
    QCOMPARE(p.doc().tracks.size(), size_t(4));
  }
  void removingATrackRemovesItsClipsAndUndoRestores() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    QVERIFY(c.removeTrack(trackIndex(p.doc(), TrackKind::Video)));
    QVERIFY(!getItem(p.doc(), V1));
    QVERIFY(p.undo());
    QVERIFY(getItem(p.doc(), V1));
    QVERIFY(getItem(p.doc(), V2));
  }

  // ---- navigation ----
  void edgesForUpDown() {
    Project p;
    fillProject(p);
    TimelineController c(p);
    QCOMPARE(c.edgeFrom(0, 1), qint64(30));
    QCOMPARE(c.edgeFrom(90, 1), qint64(120));
    QCOMPARE(c.edgeFrom(95, -1), qint64(90));
    QCOMPARE(c.edgeFrom(0, -1), qint64(-1));
    QCOMPARE(c.edgeFrom(210, 1), qint64(-1));
    QCOMPARE(c.durationFrames(), qint64(210));
  }
};

} // namespace tst
using tst::TstTimelineController;
QTEST_GUILESS_MAIN(TstTimelineController)
#include "tst_timeline_controller.moc"
