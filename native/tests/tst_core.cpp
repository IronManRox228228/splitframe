#include "test_helpers.h"

// Ports packages/editor-core/src/core.test.ts case by case.
using namespace sf;
using namespace sf::test;

namespace {
QString S(const char* s) { return QString::fromLatin1(s); }
} // namespace

class TstCore : public QObject {
  Q_OBJECT
private slots:
  // ---- describe('item.add / remove / update / move') ----
  void addsAnItemAndRemovesItAgain() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 0, 90);
    const auto r = applyOp(doc, addItem(item));
    QCOMPARE_EQ(r.doc.items.size(), 1u);
    const auto d2 = applyOp(r.doc, r.inverse[0]);
    QCOMPARE_EQ(d2.doc.items.size(), 0u);
  }

  void doesNotMutateTheInputDoc() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 0, 90);
    applyOp(doc, addItem(item));
    QCOMPARE_EQ(doc.items.size(), 0u);
  }

  void rejectsDuplicateItemIds() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 0, 90);
    const TimelineDoc d1 = applyOp(doc, addItem(item)).doc;
    SF_THROWS(applyOp(d1, addItem(item)), OpError);
  }

  void rejectsItemsOnIncompatibleTracks() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 0, 90, [&](ItemInit& i) { i.trackId = trackOfKind(doc, TrackKind::Audio).id; });
    SF_THROWS(applyOp(doc, addItem(item)), OpError);
  }

  void updatesAPatchAndProducesAnExactInverse() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 0, 90);
    const TimelineDoc d1 = applyOp(doc, addItem(item)).doc;
    ItemUpdate up;
    up.itemId = item.id;
    TransformPatch tp;
    tp.x = 100;
    tp.opacity = 0.5;
    up.patch.transform = tp;
    up.patch.volume = 0.25;
    const auto r = applyOp(d1, up);
    const Item& updated = r.doc.items[0];
    QCOMPARE_EQ(updated.transform.x, 100);
    QCOMPARE_EQ(updated.transform.y, 0); // merge, not replace
    QCOMPARE_EQ(updated.transform.opacity, 0.5);
    QCOMPARE_EQ(updated.volume, 0.25);
    const TimelineDoc d3 = applyOps(r.doc, r.inverse).doc;
    SF_COMPARE(d3.items[0], item);
  }

  void wholeReplacesPropsAndRestoresThem() {
    const TimelineDoc doc = makeDoc();
    const Item item = textItem(doc, 0, 60, S("Before"));
    const TimelineDoc d1 = applyOp(doc, addItem(item)).doc;
    ItemUpdate up;
    up.itemId = item.id;
    up.patch.props = QJsonValue(toJson(ItemProps{TextProps{S("After"), std::get<TextProps>(item.props).style}}));
    const auto r = applyOp(d1, up);
    QCOMPARE(std::get<TextProps>(r.doc.items[0].props).text, S("After"));
    const TimelineDoc d3 = applyOps(r.doc, r.inverse).doc;
    SF_COMPARE(d3.items[0], item);
  }

  void movesAnItemBetweenFramesAndBetweenTracks() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 0, 90);
    const TimelineDoc d1 = applyOp(doc, addItem(item)).doc;
    const auto m1 = applyOp(d1, ItemMove{item.id, std::nullopt, 30});
    QCOMPARE_EQ(m1.doc.items[0].startFrame, 30);
    const TimelineDoc d3 = applyOps(m1.doc, m1.inverse).doc;
    QCOMPARE_EQ(d3.items[0].startFrame, 0);

    // now add a second video track and move the item to it
    const QString newTrackId = newId(S("trk"));
    TrackAdd add;
    add.trackId = newTrackId;
    add.kind = TrackKind::Video;
    add.name = S("B-roll");
    const TimelineDoc d4 = applyOp(d3, add).doc;
    const auto m2 = applyOp(d4, ItemMove{item.id, newTrackId, std::nullopt});
    QCOMPARE(m2.doc.items[0].trackId, newTrackId);
    const TimelineDoc d6 = applyOps(m2.doc, m2.inverse).doc;
    QCOMPARE(d6.items[0].trackId, d1.tracks[1].id);
  }

  void itemRemoveThrowsOnUnknownIdsWithAHint() {
    const TimelineDoc doc = makeDoc();
    SF_THROWS_MATCH(applyOp(doc, ItemRemove{{S("itm_nope")}, false}), "not found");
  }

  // ---- describe('tracks') ----
  void addsATrackAtAnIndexAndRemovesItWithItsItems() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 0, 30);
    const TimelineDoc d1 = applyOp(doc, addItem(item)).doc;
    const QString trackId = newId(S("trk"));
    TrackAdd add;
    add.trackId = trackId;
    add.kind = TrackKind::Overlay;
    add.name = S("Overlay");
    add.index = 1;
    const auto addR = applyOp(d1, add);
    QCOMPARE(addR.doc.tracks[1].id, trackId);
    ItemUpdate mv;
    mv.itemId = item.id;
    mv.patch.trackId = trackId;
    const auto moveR = applyOp(addR.doc, mv);
    const auto removeR = applyOp(moveR.doc, TrackRemove{trackId});
    QCOMPARE_EQ(removeR.doc.tracks.size(), d1.tracks.size());
    for (std::size_t i = 0; i < d1.tracks.size(); ++i) QCOMPARE(removeR.doc.tracks[i].id, d1.tracks[i].id);
    QVERIFY(!getItem(removeR.doc, item.id)); // removed with the track
    // undo everything: track comes back with its item, then the item moves home
    const TimelineDoc d5 = applyOps(removeR.doc, removeR.inverse).doc;
    const TimelineDoc d6 = applyOps(d5, moveR.inverse).doc;
    const TimelineDoc d7 = applyOps(d6, addR.inverse).doc;
    QCOMPARE_EQ(d7.tracks.size(), d1.tracks.size());
    for (std::size_t i = 0; i < d1.tracks.size(); ++i) QCOMPARE(d7.tracks[i].id, d1.tracks[i].id);
    QCOMPARE(requireItem(d7, item.id).trackId, d1.tracks[1].id);
  }

  void reordersTracksAndRestoresTheOrder() {
    const TimelineDoc doc = makeDoc();
    std::vector<QString> ids;
    for (const Track& t : doc.tracks) ids.push_back(t.id);
    std::vector<QString> reversed(ids.rbegin(), ids.rend());
    const auto r = applyOp(doc, TrackReorder{reversed});
    for (std::size_t i = 0; i < ids.size(); ++i) QCOMPARE(r.doc.tracks[i].id, reversed[i]);
    const TimelineDoc d2 = applyOps(r.doc, r.inverse).doc;
    for (std::size_t i = 0; i < ids.size(); ++i) QCOMPARE(d2.tracks[i].id, ids[i]);
  }

  void rejectsReorderWithMissingIds() {
    const TimelineDoc doc = makeDoc();
    std::vector<QString> ids;
    for (std::size_t i = 1; i < doc.tracks.size(); ++i) ids.push_back(doc.tracks[i].id);
    SF_THROWS(applyOp(doc, TrackReorder{ids}), OpError);
  }

  // ---- describe('markers') ----
  void markerAddUpdateRemoveRoundtrip() {
    const TimelineDoc doc = makeDoc();
    const Marker marker{newId(S("mrk")), 45, S("Hit"), S("#f00")};
    const auto r = applyOp(doc, MarkerAdd{marker});
    QCOMPARE_EQ(r.doc.markers.size(), 1u);
    MarkerUpdate up;
    up.markerId = marker.id;
    up.patch.label = S("Drop");
    const auto u = applyOp(r.doc, up);
    QCOMPARE(u.doc.markers[0].label, S("Drop"));
    const TimelineDoc d3 = applyOps(u.doc, u.inverse).doc;
    QCOMPARE(d3.markers[0].label, S("Hit"));
    const TimelineDoc d4 = applyOps(d3, r.inverse).doc;
    QCOMPARE_EQ(d4.markers.size(), 0u);
  }

  // ---- describe('batch ops') ----
  void batchAppliesAtomicallyFailureLeavesTheDocUntouched() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 0, 30);
    const TimelineDoc d1 = applyOp(doc, addItem(item)).doc;
    BatchOp batch;
    batch.ops.push_back(MarkerAdd{Marker{newId(S("mrk")), 10, S("x"), std::nullopt}});
    ItemUpdate bad;
    bad.itemId = S("itm_missing");
    bad.patch.volume = 0.5;
    batch.ops.push_back(bad);
    SF_THROWS_ANY(applyOp(d1, batch));
    // d1 unchanged (apply never mutates input)
    QCOMPARE_EQ(d1.markers.size(), 0u);
  }

  void rejectsNestedBatches() {
    const TimelineDoc doc = makeDoc();
    BatchOp inner;
    inner.ops.push_back(ProjectRename{S("x")});
    BatchOp outer;
    outer.ops.push_back(inner);
    SF_THROWS_MATCH(applyOp(doc, outer), "batch");
  }

  // ---- describe('timeline queries') ----
  void computesDocDurationAndSortedTrackItems() {
    const TimelineDoc doc = makeDoc();
    const Item a = videoItem(doc, 30, 60);
    const Item b = videoItem(doc, 0, 30);
    const TimelineDoc d1 = applyOps(doc, {addItem(a), addItem(b), addItem(audioItem(doc, 0, 120))}).doc;
    QCOMPARE_EQ(docDurationFrames(d1), 120);
    const Track& main = trackOfKind(d1, TrackKind::Video);
    const auto sorted = itemsOnTrack(d1, main.id);
    QCOMPARE_EQ(sorted.size(), 2u);
    QCOMPARE_EQ(sorted[0]->startFrame, 0);
    QCOMPARE_EQ(sorted[1]->startFrame, 30);
    QCOMPARE_EQ(itemEnd(*sorted[0]), 30);
  }

  void mapsTimelineFramesToSourceFramesWithSpeed() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 10, 90, [](ItemInit& i) {
      i.speed = 2;
      i.sourceInFrame = 5;
    });
    QCOMPARE_EQ(sourceFrameAt(item, 10), 5);
    QCOMPARE_EQ(sourceFrameAt(item, 11), 7);
    QCOMPARE_EQ(sourceFrameAt(item, 12), 9);
  }

  void mapsFramesThroughATimeRemap() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 0, 60, [](ItemInit& i) {
      i.timeRemap = std::vector<TimeRemapPoint>{{0, 0}, {30, 30}, {60, 30}}; // freeze at the end
    });
    QCOMPARE_EQ(sourceFrameAt(item, 15), 15);
    QCOMPARE_EQ(sourceFrameAt(item, 45), 30);
    QCOMPARE_EQ(sourceFrameAt(item, 70), 30);
  }
};

QTEST_APPLESS_MAIN(TstCore)
#include "tst_core.moc"
