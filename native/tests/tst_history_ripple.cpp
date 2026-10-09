#include "test_helpers.h"

// Ports packages/editor-core/src/history-ripple.test.ts case by case.
using namespace sf;
using namespace sf::test;

namespace {
QString S(const char* s) { return QString::fromLatin1(s); }
Op rename(const char* name) { return ProjectRename{S(name)}; }
std::vector<Op> ops1(const Op& op) { return {op}; }
} // namespace

class TstHistoryRipple : public QObject {
  Q_OBJECT
private slots:
  // ---- describe('ripple delete') ----
  void closesTheGapAndFullyRestoresOnUndo() {
    const TimelineDoc doc = makeDoc();
    const Item a = videoItem(doc, 0, 60);
    const Item b = videoItem(doc, 60, 60);
    const Item c = videoItem(doc, 120, 60);
    const TimelineDoc d1 = docWith(doc, {a, b, c});
    const auto r = applyOp(d1, ItemRemove{{b.id}, true});
    QCOMPARE_EQ(r.doc.items.size(), 2u);
    QCOMPARE_EQ(requireItem(r.doc, c.id).startFrame, 60);
    const TimelineDoc d3 = applyOps(r.doc, r.inverse).doc;
    // item array order is not semantic; compare as sorted sets
    SF_COMPARE(byId(d3.items), byId(d1.items));
  }

  void multiItemRippleDeleteOnOneTrack() {
    const TimelineDoc doc = makeDoc();
    const Item a = videoItem(doc, 0, 30);
    const Item b = videoItem(doc, 30, 30);
    const Item c = videoItem(doc, 60, 30);
    const Item d = videoItem(doc, 90, 30);
    const TimelineDoc d1 = docWith(doc, {a, b, c, d});
    const auto r = applyOp(d1, ItemRemove{{b.id, c.id}, true});
    QCOMPARE_EQ(requireItem(r.doc, d.id).startFrame, 30);
    const TimelineDoc d3 = applyOps(r.doc, r.inverse).doc;
    SF_COMPARE(byId(d3.items), byId(d1.items));
  }

  void nonRippleRemoveLeavesDownstreamItemsAlone() {
    const TimelineDoc doc = makeDoc();
    const Item a = videoItem(doc, 0, 60);
    const Item b = videoItem(doc, 60, 60);
    const TimelineDoc d1 = docWith(doc, {a, b});
    const auto r = applyOp(d1, ItemRemove{{a.id}, false});
    QCOMPARE_EQ(requireItem(r.doc, b.id).startFrame, 60);
  }

  // ---- describe('History') ----
  void undoRedoWalksTheStackInOrder() {
    History h;
    QVERIFY(!h.canUndo());
    h.push(ops1(rename("a")), ops1(rename("x")));
    h.push(ops1(rename("b")), ops1(rename("a")));
    QVERIFY(h.canUndo());
    QVERIFY(h.undo()->ops[0] == rename("b"));
    QVERIFY(h.undo()->ops[0] == rename("a"));
    QVERIFY(!h.canUndo());
    QVERIFY(h.redo()->ops[0] == rename("a"));
    QVERIFY(h.redo()->ops[0] == rename("b"));
    QVERIFY(!h.canRedo());
  }

  void aNewPushDiscardsTheRedoTail() {
    History h;
    h.push(ops1(rename("a")), ops1(rename("x")));
    h.push(ops1(rename("b")), ops1(rename("a")));
    h.undo();
    h.push(ops1(rename("c")), ops1(rename("x")));
    QVERIFY(!h.canRedo());
    QVERIFY(h.undo()->ops[0] == rename("c"));
    QVERIFY(h.undo()->ops[0] == rename("a"));
  }

  void groupsAgentTurnsBeginGroupPushesCommitGroupIsOneUndo() {
    History h;
    h.beginGroup(S("Agent turn"));
    h.push(ops1(rename("a")), ops1(rename("x")));
    h.push(ops1(rename("b")), ops1(rename("a")));
    const auto group = h.commitGroup();
    QVERIFY(group.has_value());
    QCOMPARE(*group->label, S("Agent turn"));
    QCOMPARE_EQ(group->ops.size(), 2u);
    QCOMPARE_EQ(h.undo()->ops.size(), 2u);
    QVERIFY(!h.canUndo());
  }

  void uncommittedEmptyGroupsAreDropped() {
    History h;
    h.beginGroup();
    QVERIFY(!h.commitGroup().has_value());
    QVERIFY(!h.canUndo());
  }

  void respectsTheSizeLimit() {
    History h(3);
    for (int i = 0; i < 5; ++i)
      h.push(ops1(ProjectRename{QStringLiteral("n%1").arg(i)}), ops1(ProjectRename{QStringLiteral("p%1").arg(i)}));
    int undos = 0;
    while (h.undo()) ++undos;
    QCOMPARE_EQ(undos, 3);
  }

  // ---- describe('undo roundtrip over a full edit sequence') ----
  void aRealisticSessionReplaysBackToTheEmptyDoc() {
    const TimelineDoc doc = makeDoc();
    std::vector<Op> ops;
    const Item item = videoItem(doc, 0, 120);
    ops.push_back(addItem(item));
    ops.push_back(ItemTrim{item.id, TrimEdge::Out, 90, false});
    const QString splitId = newId(S("itm"));
    ops.push_back(ItemSplit{item.id, 45, splitId});
    ItemUpdate vol;
    vol.itemId = splitId;
    vol.patch.volume = 0.5;
    ops.push_back(vol);
    ops.push_back(MarkerAdd{Marker{newId(S("mrk")), 30, S("start"), std::nullopt}});
    const auto r = applyOps(doc, ops);
    QCOMPARE_EQ(r.doc.items.size(), 2u);
    const TimelineDoc restored = applyOps(r.doc, r.inverse).doc;
    SF_COMPARE(restored.items, doc.items);
    SF_COMPARE(restored.markers, doc.markers);
    SF_COMPARE(restored.tracks, doc.tracks);
  }

  // ---- describe('History.dropReferences') ----
  void dropsTheReferencingUndoGroupAndOlderOnesKeepsNewerAndTrimsTheRedoTail() {
    // group(id, ref): ops [rename id+ref], inverses [rename x], label id
    const auto push = [](History& h, const QString& id, const QString& ref = {}) {
      h.push({ProjectRename{id + ref}}, {rename("x")}, id);
    };
    History h;
    push(h, S("a"));
    push(h, S("b"), S("ast_1"));
    push(h, S("c"));
    push(h, S("d"), S("ast_1"));
    push(h, S("e"));
    h.undo(); // e
    h.undo(); // d: now in the redo tail with e
    h.dropReferences(S("ast_1"));
    // undo side: b referenced it, so a and b are gone; c stays
    QCOMPARE(*h.undo()->label, S("c"));
    QVERIFY(!h.undo().has_value());
    // redo side: d referenced it, so d and e are gone
    QVERIFY(h.canRedo()); // c itself is redoable now
    QCOMPARE(*h.redo()->label, S("c"));
    QVERIFY(!h.redo().has_value());
  }
};

QTEST_APPLESS_MAIN(TstHistoryRipple)
#include "tst_history_ripple.moc"
