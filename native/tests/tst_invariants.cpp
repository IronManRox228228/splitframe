#include "test_helpers.h"

// Ports packages/editor-core/src/invariants.test.ts case by case.
using namespace sf;
using namespace sf::test;

namespace {
QString S(const char* s) { return QString::fromLatin1(s); }
Op trim(const QString& id, TrimEdge edge, Frame frame, bool ripple) { return ItemTrim{id, edge, frame, ripple}; }
} // namespace

class TstInvariants : public QObject {
  Q_OBJECT
private slots:
  // ---- describe('trim boundaries') ----
  void neverMovesTheStartBelowFrame0WhenExtendingTheHead() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 10, 100, [](ItemInit& i) { i.sourceInFrame = 100; });
    const TimelineDoc d1 = docWith(doc, {item});
    const TimelineDoc d2 = applyOp(d1, trim(item.id, TrimEdge::In, -50, false)).doc;
    const Item& t = d2.items[0];
    QCOMPARE_EQ(t.startFrame, 0);
    QCOMPARE_EQ(t.startFrame + t.durationFrames, 110); // the tail stays put
    QCOMPARE_EQ(*t.sourceInFrame, 90);                  // head extended by 10 frames only
  }

  void keepsTheStartInsideTheItemWhenTheInEdgeIsDraggedPastTheEnd() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 10, 100);
    const TimelineDoc d1 = docWith(doc, {item});
    const TimelineDoc d2 = applyOp(d1, trim(item.id, TrimEdge::In, 500, false)).doc;
    const Item& t = d2.items[0];
    QCOMPARE_EQ(t.startFrame, 109);
    QCOMPARE_EQ(t.durationFrames, 1);
    QCOMPARE_EQ(*t.sourceInFrame, 99);
  }

  void neverSlidesDownstreamItemsByMoreThanTheItemLengthOnRippleTrimIn() {
    const TimelineDoc doc = makeDoc();
    const Item a = videoItem(doc, 0, 100);
    const Item b = videoItem(doc, 100, 50);
    const TimelineDoc d1 = docWith(doc, {a, b});
    const TimelineDoc d2 = applyOp(d1, trim(a.id, TrimEdge::In, 500, true)).doc;
    QCOMPARE_EQ(requireItem(d2, a.id).durationFrames, 1);
    QCOMPARE_EQ(requireItem(d2, b.id).startFrame, 1); // 100 - 99
  }

  void restoresTheExactGeometryWhenUndoingAnInTrimAtAFractionalSpeed() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 0, 100, [](ItemInit& i) { i.speed = 0.4; });
    const TimelineDoc d1 = docWith(doc, {item});
    const auto r = applyOp(d1, trim(item.id, TrimEdge::In, 3, false));
    const TimelineDoc d3 = applyOps(r.doc, r.inverse).doc;
    SF_COMPARE(d3.items[0], item);
  }

  // ---- describe('item.clone') ----
  void rejectsADestinationTrackThatCannotHoldTheItemType() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 0, 100);
    const TimelineDoc d1 = docWith(doc, {item});
    ItemClone c;
    c.itemId = item.id;
    c.newItemId = newId(S("itm"));
    c.trackId = trackOfKind(d1, TrackKind::Audio).id;
    SF_THROWS_MATCH(applyOp(d1, c), "cannot live on");
  }

  // ---- describe('locked tracks') ----
  void rejectsEditsToItemsOnALockedTrack() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 0, 100);
    const TimelineDoc d1 = docWith(doc, {item});
    TrackUpdate lock;
    lock.trackId = item.trackId;
    lock.patch.locked = true;
    const TimelineDoc locked = applyOp(d1, lock).doc;

    ItemUpdate upd;
    upd.itemId = item.id;
    upd.patch.volume = 0.5;
    ItemClone clone;
    clone.itemId = item.id;
    clone.newItemId = newId(S("itm"));
    const std::vector<Op> ops = {
        ItemMove{item.id, std::nullopt, 5},
        ItemSplit{item.id, 50, newId(S("itm"))},
        ItemRemove{{item.id}, false},
        upd,
        clone,
        ItemSetSpeed{item.id, 2},
    };
    for (const Op& op : ops) {
      QVERIFY2([&] {
        try {
          applyOp(locked, op);
        } catch (const OpError& e) {
          return QString::fromUtf8(e.what()).contains(QStringLiteral("locked"), Qt::CaseInsensitive);
        }
        return false;
      }(), qPrintable(op.type()));
    }
  }

  void rejectsAddingToALockedTrackAndRemovingIt() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 0, 100);
    const TimelineDoc d1 = docWith(doc, {item});
    TrackUpdate lock;
    lock.trackId = item.trackId;
    lock.patch.locked = true;
    const TimelineDoc locked = applyOp(d1, lock).doc;
    const Item other = videoItem(locked, 200, 10, [&](ItemInit& i) { i.trackId = item.trackId; });
    SF_THROWS_MATCH(applyOp(locked, addItem(other)), "locked");
    SF_THROWS_MATCH(applyOp(locked, TrackRemove{item.trackId}), "locked");
  }

  void stillAllowsUnlockingAndUndoRedoMayBypassTheLock() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 0, 100);
    const TimelineDoc d1 = docWith(doc, {item});
    TrackUpdate lock;
    lock.trackId = item.trackId;
    lock.patch.locked = true;
    const TimelineDoc locked = applyOp(d1, lock).doc;
    TrackUpdate unlock;
    unlock.trackId = item.trackId;
    unlock.patch.locked = false;
    const TimelineDoc unlocked = applyOp(locked, unlock).doc;
    SF_NO_THROW(applyOp(unlocked, ItemMove{item.id, std::nullopt, 5}));
    SF_NO_THROW(applyOp(locked, ItemMove{item.id, std::nullopt, 5}, {.enforceLocks = false}));
  }

  void checksEachOpOfABatchAgainstTheEvolvingDoc() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 0, 100);
    const TimelineDoc d1 = docWith(doc, {item});
    TrackUpdate lock;
    lock.trackId = item.trackId;
    lock.patch.locked = true;
    const TimelineDoc locked = applyOp(d1, lock).doc;
    TrackUpdate unlock;
    unlock.trackId = item.trackId;
    unlock.patch.locked = false;
    const TimelineDoc after = applyOps(locked, {unlock, ItemMove{item.id, std::nullopt, 5}}).doc;
    QCOMPARE_EQ(after.items[0].startFrame, 5);
  }

  // ---- describe('inverse fidelity') ----
  void removesLabelKeysThatDidNotExistBeforeTheUpdate() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 0, 100);
    const TimelineDoc d1 = docWith(doc, {item});
    ItemUpdate up;
    up.itemId = item.id;
    LabelsPatch lp;
    lp.name = std::optional<QString>(S("hello"));
    up.patch.labels = lp;
    const auto r = applyOp(d1, up);
    QCOMPARE(*r.doc.items[0].labels.name, S("hello"));
    const TimelineDoc d3 = applyOps(r.doc, r.inverse).doc;
    QVERIFY(d3.items[0].labels == Labels{}); // toEqual({})
  }

  void removesKeyframeTracksThatDidNotExistBeforeTheUpdate() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 0, 100);
    const TimelineDoc d1 = docWith(doc, {item});
    ItemUpdate up;
    up.itemId = item.id;
    up.patch.keyframes = KeyframeMap{{S("transform.x"), {Keyframe{0, 1, Easing::Linear}}}};
    const auto r = applyOp(d1, up);
    const TimelineDoc d3 = applyOps(r.doc, r.inverse).doc;
    QVERIFY(d3.items[0].keyframes.empty());
  }

  // ---- describe('undo groups') ----
  void undoesAMultiPushGroupNewestFirst() {
    const TimelineDoc doc = makeDoc();
    const auto a = applyOp(doc, ProjectRename{S("A")});
    const auto b = applyOp(a.doc, ProjectRename{S("B")});
    History h;
    h.beginGroup(S("turn"));
    h.push({ProjectRename{S("A")}}, a.inverse);
    h.push({ProjectRename{S("B")}}, b.inverse);
    const auto group = h.undo();
    QVERIFY(group.has_value());
    const TimelineDoc undone = applyOps(b.doc, group->inverses).doc;
    QCOMPARE(undone.project.name, doc.project.name);
  }

  // ---- describe('History.undoWith / redoWith') ----
  void keepsTheEntryUndoableWhenApplyingItsInversesThrows() {
    History h;
    h.push({ProjectRename{S("a")}}, {ProjectRename{S("before-a")}});
    bool threw = false;
    try {
      h.undoWith([](const UndoGroup&) -> int { throw std::runtime_error("doc changed"); });
    } catch (const std::runtime_error& e) {
      threw = QString::fromUtf8(e.what()) == S("doc changed");
    }
    QVERIFY(threw);
    QVERIFY(h.canUndo());
    QVERIFY(!h.canRedo());
    const auto n = h.undoWith([](const UndoGroup& g) { return g.ops.size(); });
    QCOMPARE_EQ(*n, 1u);
    QVERIFY(h.canRedo());
  }

  void keepsTheEntryRedoableWhenRedoThrows() {
    History h;
    h.push({ProjectRename{S("a")}}, {ProjectRename{S("before-a")}});
    h.undoWith([](const UndoGroup&) { return 0; });
    bool threw = false;
    try {
      h.redoWith([](const UndoGroup&) -> int { throw std::runtime_error("nope"); });
    } catch (const std::runtime_error& e) {
      threw = QString::fromUtf8(e.what()) == S("nope");
    }
    QVERIFY(threw);
    QVERIFY(h.canRedo());
    QCOMPARE(*h.redoWith([](const UndoGroup&) { return S("ok"); }), S("ok"));
    QVERIFY(!h.canRedo());
  }

  void returnsNullWhenThereIsNothingToUndoOrRedo() {
    History h;
    QVERIFY(!h.undoWith([](const UndoGroup&) { return 1; }).has_value());
    QVERIFY(!h.redoWith([](const UndoGroup&) { return 1; }).has_value());
  }
};

QTEST_APPLESS_MAIN(TstInvariants)
#include "tst_invariants.moc"
