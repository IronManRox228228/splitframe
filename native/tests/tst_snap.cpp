#include "core/snap.h"
#include "test_helpers.h"

// Ports packages/editor-core/src/snap.test.ts case by case.
using namespace sf;
using namespace sf::test;

namespace {

struct Fixture {
  TimelineDoc doc;
  Item a;
  Item b;
};

Fixture docWithItems() {
  const TimelineDoc doc = makeDoc();
  Fixture f{doc, videoItem(doc, 0, 60), videoItem(doc, 90, 60)};
  f.doc = docWith(doc, {f.a, f.b});
  return f;
}

} // namespace

class TstSnap : public QObject {
  Q_OBJECT
private slots:
  void collectsItemEdgesAndZero() {
    const auto f = docWithItems();
    const auto candidates = getSnapCandidates(f.doc);
    std::vector<double> frames;
    for (const auto& c : candidates) frames.push_back(c.frame);
    std::sort(frames.begin(), frames.end());
    QCOMPARE(frames, (std::vector<double>{0, 0, 60, 90, 150}));
  }

  void excludesTheDraggedItemFromItsOwnCandidates() {
    const auto f = docWithItems();
    SnapOptions opts;
    opts.excludeItemIds = {f.b.id};
    const auto candidates = getSnapCandidates(f.doc, opts);
    const auto n = std::count_if(candidates.begin(), candidates.end(), [&](const SnapCandidate& c) { return c.source == f.b.id; });
    QCOMPARE_EQ(n, 0);
  }

  void snapsToTheNearestCandidateWithinThreshold() {
    const auto f = docWithItems();
    const auto candidates = getSnapCandidates(f.doc);
    QCOMPARE_EQ(snapFrame(62, candidates, 5)->frame, 60);
    QCOMPARE_EQ(snapFrame(88, candidates, 5)->frame, 90);
    QVERIFY(!snapFrame(80, candidates, 5).has_value());
  }

  void snapItemStartConsidersBothEdgesOfTheMovingItem() {
    const auto f = docWithItems();
    // dragging b so its OUT edge lands on 60 (a's end) -> start = 0
    QCOMPARE_EQ(snapItemStart(f.doc, f.b.id, 2, 5), 0);
    // dragging b so its IN edge lands on 60 -> start = 60
    QCOMPARE_EQ(snapItemStart(f.doc, f.b.id, 58, 5), 60);
    // no candidate nearby -> unchanged
    QCOMPARE_EQ(snapItemStart(f.doc, f.b.id, 45, 3), 45);
  }

  void includesMarkersAndThePlayheadWhenAsked() {
    const auto f = docWithItems();
    const TimelineDoc d1 = applyOp(f.doc, MarkerAdd{Marker{QStringLiteral("mrk_test"), 45, QStringLiteral("hit"), std::nullopt}}).doc;
    SnapOptions opts;
    opts.includePlayhead = 47;
    const auto candidates = getSnapCandidates(d1, opts);
    const auto has = [&](double frame) {
      return std::any_of(candidates.begin(), candidates.end(), [&](const SnapCandidate& c) { return c.frame == frame; });
    };
    QVERIFY(has(45));
    QVERIFY(has(47));
  }
};

QTEST_APPLESS_MAIN(TstSnap)
#include "tst_snap.moc"
