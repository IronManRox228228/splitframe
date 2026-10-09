#include "test_helpers.h"

// Ports packages/editor-core/src/trim-split.test.ts case by case.
using namespace sf;
using namespace sf::test;

namespace {

QString S(const char* s) { return QString::fromLatin1(s); }
Op trim(const QString& id, TrimEdge edge, Frame frame, bool ripple) { return ItemTrim{id, edge, frame, ripple}; }

QString frames(const std::vector<TimeRemapPoint>& v) {
  QStringList l;
  for (const auto& p : v) l << QString::number(p.frame);
  return l.join(QLatin1Char(','));
}

} // namespace

class TstTrimSplit : public QObject {
  Q_OBJECT
private slots:
  // ---- describe('trim (plain)') ----
  void trimsTheInEdgeStartMovesSourceAdvancesEndStays() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 30, 90, [](ItemInit& i) { i.sourceInFrame = 100; });
    const TimelineDoc d1 = applyOp(doc, addItem(item)).doc;
    const auto r = applyOp(d1, trim(item.id, TrimEdge::In, 60, false));
    const Item& t = r.doc.items[0];
    QCOMPARE_EQ(t.startFrame, 60);
    QCOMPARE_EQ(t.durationFrames, 60); // end stays at 120
    QCOMPARE_EQ(*t.sourceInFrame, 130); // advanced 30 frames * speed 1
    const TimelineDoc d3 = applyOps(r.doc, r.inverse).doc;
    SF_COMPARE(d3.items[0], item);
  }

  void trimsTheOutEdgeDurationShrinksStartStays() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 30, 90);
    const TimelineDoc d1 = applyOp(doc, addItem(item)).doc;
    const auto r = applyOp(d1, trim(item.id, TrimEdge::Out, 90, false));
    const Item& t = r.doc.items[0];
    QCOMPARE_EQ(t.startFrame, 30);
    QCOMPARE_EQ(t.durationFrames, 60);
    const TimelineDoc d3 = applyOps(r.doc, r.inverse).doc;
    SF_COMPARE(d3.items[0], item);
  }

  void rejectsANoOpTrim() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 30, 90);
    const TimelineDoc d1 = applyOp(doc, addItem(item)).doc;
    SF_THROWS_MATCH(applyOp(d1, trim(item.id, TrimEdge::Out, 120, false)), "would not change");
  }

  void respectsSpeedWhenAdvancingSourceIn() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 0, 60, [](ItemInit& i) {
      i.sourceInFrame = 0;
      i.speed = 0.5; // slow-mo
    });
    const TimelineDoc d1 = applyOp(doc, addItem(item)).doc;
    const TimelineDoc d2 = applyOp(d1, trim(item.id, TrimEdge::In, 30, false)).doc;
    QCOMPARE_EQ(*d2.items[0].sourceInFrame, 15); // 30 timeline frames * 0.5
  }

  void clampsSourceInAtZeroWhenExtendingTheHead() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 50, 50, [](ItemInit& i) { i.sourceInFrame = 10; });
    const TimelineDoc d1 = applyOp(doc, addItem(item)).doc;
    const TimelineDoc d2 = applyOp(d1, trim(item.id, TrimEdge::In, 0, false)).doc;
    const Item& t = d2.items[0];
    QCOMPARE_EQ(*t.sourceInFrame, 0); // can't go below source 0
    QCOMPARE_EQ(t.startFrame, 40);    // start adjusted to match available source
    QCOMPARE_EQ(t.durationFrames, 60);
  }

  // ---- describe('trim (ripple)') ----
  void shorteningTheOutEdgePullsDownstreamItemsLeft() {
    const TimelineDoc doc = makeDoc();
    const Item a = videoItem(doc, 0, 90);
    const Item b = videoItem(doc, 90, 90);
    const Item c = videoItem(doc, 180, 90);
    const TimelineDoc d1 = applyOps(doc, {addItem(a), addItem(b), addItem(c)}).doc;
    const auto r = applyOp(d1, trim(b.id, TrimEdge::Out, 150, true)); // shorten by 30
    QCOMPARE_EQ(requireItem(r.doc, b.id).durationFrames, 60);
    QCOMPARE_EQ(requireItem(r.doc, c.id).startFrame, 150);
    const TimelineDoc d3 = applyOps(r.doc, r.inverse).doc;
    SF_COMPARE(d3.items, d1.items);
  }

  void rippleTrimInAnchorsTheItemStartAndSlidesDownstream() {
    const TimelineDoc doc = makeDoc();
    const Item a = videoItem(doc, 0, 90);
    const Item b = videoItem(doc, 90, 90);
    const TimelineDoc d1 = applyOps(doc, {addItem(a), addItem(b)}).doc;
    const auto r = applyOp(d1, trim(b.id, TrimEdge::In, 120, true)); // cut 30 frames off the head of b
    const Item& bb = requireItem(r.doc, b.id);
    QCOMPARE_EQ(bb.startFrame, 90); // anchored
    QCOMPARE_EQ(bb.durationFrames, 60);
    QCOMPARE_EQ(*bb.sourceInFrame, 30);
    QCOMPARE_EQ(requireItem(r.doc, a.id).startFrame, 0);
    const TimelineDoc d3 = applyOps(r.doc, r.inverse).doc;
    SF_COMPARE(d3.items, d1.items);
  }

  void rippleExtendingTheOutEdgePushesDownstreamRight() {
    const TimelineDoc doc = makeDoc();
    const Item a = videoItem(doc, 0, 90);
    const Item b = videoItem(doc, 90, 90);
    const TimelineDoc d1 = applyOps(doc, {addItem(a), addItem(b)}).doc;
    const auto r = applyOp(d1, trim(a.id, TrimEdge::Out, 120, true));
    QCOMPARE_EQ(itemEnd(requireItem(r.doc, a.id)), 120);
    QCOMPARE_EQ(requireItem(r.doc, b.id).startFrame, 120);
    const TimelineDoc d3 = applyOps(r.doc, r.inverse).doc;
    SF_COMPARE(d3.items, d1.items);
  }

  // ---- describe('split') ----
  void splitsIntoTwoItemsWithCorrectSourceAndRemapAndInvertsExactly() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 10, 100, [](ItemInit& i) {
      i.sourceInFrame = 200;
      i.timeRemap = std::vector<TimeRemapPoint>{{0, 200}, {50, 400}, {100, 800}};
      i.keyframes = KeyframeMap{{QStringLiteral("transform.scale"),
                                 {Keyframe{10, 1, Easing::Linear}, Keyframe{60, 2, Easing::Linear}}}};
    });
    const TimelineDoc d1 = applyOp(doc, addItem(item)).doc;
    const QString newIdB = newId(S("itm"));
    const auto r = applyOp(d1, ItemSplit{item.id, 60, newIdB});
    const Item& a = requireItem(r.doc, item.id);
    const Item& b = requireItem(r.doc, newIdB);
    QCOMPARE_EQ(a.durationFrames, 50);
    QCOMPARE_EQ(b.startFrame, 60);
    QCOMPARE_EQ(b.durationFrames, 50);
    // sourceIn for the right part = sourceIn + splitLocal * speed = 200 + 50
    QCOMPARE_EQ(*b.sourceInFrame, 250);
    QCOMPARE(frames(a.timeRemap), S("0"));
    QCOMPARE(frames(b.timeRemap), S("0,50"));
    // keyframe at local 60 moves into b at 60 - 50 = 10
    const auto& bk = b.keyframes.at(S("transform.scale"));
    QCOMPARE_EQ(bk.size(), 1u);
    QCOMPARE_EQ(bk[0].frame, 10);
    const TimelineDoc d3 = applyOps(r.doc, r.inverse).doc;
    SF_COMPARE(d3.items, d1.items);
  }

  void rejectsSplitsOutsideTheItem() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 10, 100);
    const TimelineDoc d1 = applyOp(doc, addItem(item)).doc;
    SF_THROWS_MATCH(applyOp(d1, ItemSplit{item.id, 200, newId(S("itm"))}), "outside");
  }

  // ---- describe('slip / speed / remap ops') ----
  void slipChangesSourceInOnlyAndInverts() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 0, 90, [](ItemInit& i) { i.sourceInFrame = 0; });
    const TimelineDoc d1 = applyOp(doc, addItem(item)).doc;
    const auto r = applyOp(d1, ItemSlip{item.id, 40});
    QCOMPARE_EQ(*r.doc.items[0].sourceInFrame, 40);
    QCOMPARE_EQ(r.doc.items[0].startFrame, 0);
    const TimelineDoc d3 = applyOps(r.doc, r.inverse).doc;
    SF_COMPARE(d3.items[0], item);
  }

  void audioCountsAsAMediaItemAndCanSlip() {
    const TimelineDoc doc = makeDoc();
    const Item item = audioItem(doc, 0, 90);
    const TimelineDoc d1 = applyOp(doc, addItem(item)).doc;
    const TimelineDoc d2 = applyOp(d1, ItemSlip{item.id, 5}).doc;
    QCOMPARE_EQ(*d2.items[0].sourceInFrame, 5);
  }

  void setSpeedKeepsDurationClearsRemapAndRestoresBothOnUndo() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 0, 60, [](ItemInit& i) {
      i.sourceInFrame = 0;
      i.timeRemap = std::vector<TimeRemapPoint>{{0, 0}, {60, 120}};
    });
    const TimelineDoc d1 = applyOp(doc, addItem(item)).doc;
    const auto r = applyOp(d1, ItemSetSpeed{item.id, 2});
    const Item& t = r.doc.items[0];
    QCOMPARE_EQ(t.speed, 2);
    QCOMPARE_EQ(t.timeRemap.size(), 0u);
    QCOMPARE_EQ(t.durationFrames, 60);
    QCOMPARE_EQ(sourceOutFrame(t), 120);
    const TimelineDoc d3 = applyOps(r.doc, r.inverse).doc;
    SF_COMPARE(d3.items[0], item);
  }

  void setKeyframesReplacesOnePropertyAndInverts() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 0, 60);
    const TimelineDoc d1 = applyOp(doc, addItem(item)).doc;
    const auto r = applyOp(d1, ItemSetKeyframes{item.id, S("transform.opacity"),
                                                {Keyframe{0, 0, Easing::Linear}, Keyframe{60, 1, Easing::Linear}}});
    QCOMPARE_EQ(r.doc.items[0].keyframes.at(S("transform.opacity")).size(), 2u);
    const TimelineDoc d3 = applyOps(r.doc, r.inverse).doc;
    QVERIFY(d3.items[0].keyframes.find(S("transform.opacity")) == d3.items[0].keyframes.end());
  }

  // ---- describe('clone') ----
  void clonesAfterTheOriginalAndInverts() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 0, 60);
    const TimelineDoc d1 = applyOp(doc, addItem(item)).doc;
    const QString cloneId = newId(S("itm"));
    ItemClone c;
    c.itemId = item.id;
    c.newItemId = cloneId;
    const auto r = applyOp(d1, c);
    QCOMPARE_EQ(requireItem(r.doc, cloneId).startFrame, 60);
    const TimelineDoc d3 = applyOps(r.doc, r.inverse).doc;
    SF_COMPARE(d3.items, d1.items);
  }

  // ---- describe('splitting captions') ----
  void rebasesWordTimesForTheSecondHalfAndRestoresThemOnUndo() {
    const TimelineDoc doc = makeDoc();
    const Track& textTrack = trackOfKind(doc, TrackKind::Text);
    ItemInit init;
    init.id = newId(S("itm"));
    init.trackId = textTrack.id;
    init.startFrame = 100;
    init.durationFrames = 90; // 3s at 30fps
    CaptionProps props;
    props.words = {{S("one"), 0, 900, std::nullopt, std::nullopt},
                   {S("two"), 1000, 1900, std::nullopt, std::nullopt},
                   {S("three"), 2000, 2900, std::nullopt, std::nullopt}};
    props.style.fontFamily = S("Inter");
    props.style.fontSize = 64;
    props.style.color = S("#fff");
    init.props = props;
    const Item caption = createItem(ItemType::Caption, init);
    const TimelineDoc d1 = applyOp(doc, addItem(caption)).doc;
    const QString newItemId = newId(S("itm"));
    const auto r = applyOp(d1, ItemSplit{caption.id, 130, newItemId}); // cut at 1000ms
    const auto& a = std::get<CaptionProps>(requireItem(r.doc, caption.id).props);
    const auto& b = std::get<CaptionProps>(requireItem(r.doc, newItemId).props);
    QCOMPARE_EQ(a.words.size(), 1u);
    QCOMPARE(a.words[0].w, S("one"));
    const std::vector<TranscriptWord> expected = {{S("two"), 0, 900, std::nullopt, std::nullopt},
                                                  {S("three"), 1000, 1900, std::nullopt, std::nullopt}};
    QVERIFY(b.words == expected);
    const TimelineDoc d3 = applyOps(r.doc, r.inverse).doc;
    SF_COMPARE(d3.items, d1.items);
  }
};

QTEST_APPLESS_MAIN(TstTrimSplit)
#include "tst_trim_split.moc"
