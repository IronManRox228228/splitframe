#include "copilot/review_engine.h"

#include <QTest>

using namespace sf;
using namespace sf::copilot;

class TstCopilotReview : public QObject {
  Q_OBJECT

private:
  TimelineDoc createCleanDoc() {
    TimelineDoc doc;
    doc.project.fps = 30;

    Track v1{.id = QStringLiteral("trk_v1"), .kind = TrackKind::Video, .name = QStringLiteral("V1")};
    doc.tracks = {v1};

    Item it;
    it.id = QStringLiteral("itm_clean");
    it.trackId = QStringLiteral("trk_v1");
    it.startFrame = 0;
    it.durationFrames = 150; // 5.0s
    it.volume = 0.8;
    VideoProps vp;
    vp.fadeOutFrames = 5;
    it.props = vp;
    doc.items.push_back(it);

    return doc;
  }

private slots:
  void testCleanTimeline() {
    ReviewEngine engine;
    TimelineDoc doc = createCleanDoc();

    const auto res = engine.reviewTimeline(doc, {});
    QVERIFY(res.passed);
    QCOMPARE(res.score, 1.0);
    QCOMPARE(res.issues.size(), 0);
    QVERIFY(res.summary.contains(QStringLiteral("Self-review passed")));
  }

  void testTimelineWithErrors() {
    ReviewEngine engine;
    TimelineDoc doc = createCleanDoc();

    // Add missing media item
    Item broken;
    broken.id = QStringLiteral("itm_broken");
    broken.trackId = QStringLiteral("trk_v1");
    broken.assetId = QStringLiteral("ast_missing");
    broken.startFrame = 150;
    broken.durationFrames = 90;
    broken.props = VideoProps{};
    doc.items.push_back(broken);

    const auto res = engine.reviewTimeline(doc, {});
    QVERIFY(!res.passed);
    QVERIFY(res.score < 0.70);
    QVERIFY(res.issues.size() >= 1);
    QVERIFY(res.summary.contains(QStringLiteral("Requires refinement")));
  }

  void testTimelineWithWarningOnly() {
    ReviewEngine engine;
    TimelineDoc doc = createCleanDoc();

    // Add flash frame item (warning, not error)
    Item flash;
    flash.id = QStringLiteral("itm_flash");
    flash.trackId = QStringLiteral("trk_v1");
    flash.startFrame = 150;
    flash.durationFrames = 2; // flash frame
    flash.props = VideoProps{};
    doc.items.push_back(flash);

    ReviewOptions opts;
    opts.passThreshold = 0.70;

    const auto res = engine.reviewTimeline(doc, {}, nullptr, opts);
    // Warning only: score drops by 0.10 to 0.90, passes threshold 0.70
    QVERIFY(res.passed);
    QCOMPARE(res.score, 0.90);
    QCOMPARE(res.issues.size(), 1);
  }

  void testApplySafeFixes() {
    ReviewEngine engine;
    TimelineDoc doc = createCleanDoc();

    // Add item with abrupt audio (warning with auto-fix)
    Item audio;
    audio.id = QStringLiteral("itm_abrupt");
    audio.trackId = QStringLiteral("trk_v1");
    audio.startFrame = 150;
    audio.durationFrames = 60;
    audio.volume = 0.9;
    AudioProps ap;
    ap.fadeOutFrames = 0; // abrupt
    audio.props = ap;
    doc.items.push_back(audio);

    ReviewOptions opts;
    opts.applySafeFixes = true;

    const auto res = engine.reviewTimeline(doc, {}, nullptr, opts);
    QVERIFY(res.autoFixApplied);
    QVERIFY(!res.autoFixOps.isEmpty());
    QCOMPARE(res.autoFixOps[0].value(QStringLiteral("type")).toString(), QStringLiteral("item.update"));
  }
};

QTEST_GUILESS_MAIN(TstCopilotReview)
#include "tst_copilot_review.moc"
