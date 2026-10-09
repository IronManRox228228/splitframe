#include "editor/timeline_math.h"
#include "editor_testutil.h"

using namespace sf;
using namespace sf::editor;

class TstTimelineMath : public QObject {
  Q_OBJECT
private slots:
  void rowHeights() {
    QCOMPARE(rowHeightForKind(TrackKind::Video), 64);
    QCOMPARE(rowHeightForKind(TrackKind::Audio), 48);
    QCOMPARE(rowHeightForKind(TrackKind::Text), 44);
    QCOMPARE(rowHeightForKind(TrackKind::Overlay), 44);
  }
  void layoutAndRowLookup() {
    Track a, b, c;
    a.kind = TrackKind::Text;
    b.kind = TrackKind::Video;
    c.kind = TrackKind::Audio;
    const auto l = trackLayout({a, b, c});
    QCOMPARE(l[0].top, 0);
    QCOMPARE(l[1].top, 44);
    QCOMPARE(l[2].top, 108);
    QCOMPARE(totalRowsHeight(l), 156);
    QCOMPARE(rowIndexAtY(l, 10), 0);
    QCOMPARE(rowIndexAtY(l, 44), 1);
    QCOMPARE(rowIndexAtY(l, 5000), 2); // clamped
    QCOMPARE(rowIndexAtY({}, 3), -1);
  }
  void rulerIntervals() {
    // 30 fps at 3 px/frame: 88 px needs 30 frames = 1 s
    QCOMPARE(rulerInterval(3, 30), Frame(30));
    QCOMPARE(rulerInterval(40, 30), Frame(5));    // zoomed in: sub-second ticks
    QCOMPARE(rulerInterval(0.2, 30), Frame(450)); // 15 s
    QCOMPARE(rulerSubdivisions(30, 3), 5);
    QCOMPARE(rulerSubdivisions(30, 0.2), 0);
  }
  void trimHandles() {
    QCOMPARE(trimHandleWidth(10), 0.0);
    QCOMPARE(trimHandleWidth(24), 8.0);
    QCOMPARE(trimHandleWidth(20), 20.0 / 3);
  }
  void badge() {
    QCOMPARE(formatDurationBadge(45, 30), QStringLiteral("1.50s"));
    QCOMPARE(formatDurationBadge(450, 30), QStringLiteral("15.0s"));
  }
  void edges() {
    TimelineDoc doc;
    doc.items.resize(2);
    doc.items[0].startFrame = 10;
    doc.items[0].durationFrames = 20;
    doc.items[1].startFrame = 30;
    doc.items[1].durationFrames = 10;
    const auto e = clipEdges(doc);
    QCOMPARE(e, (std::vector<Frame>{10, 30, 40}));
    QCOMPARE(neighborEdge(e, 30, 1).value(), Frame(40));
    QCOMPARE(neighborEdge(e, 30, -1).value(), Frame(10));
    QVERIFY(!neighborEdge(e, 40, 1));
    QVERIFY(!neighborEdge(e, 10, -1));
  }
  void zoomKeepsAnchor() {
    // frame 100 under x=200: at 4 px/frame the view must start at 100*4-200
    QCOMPARE(scrollForZoom(100, 200, 4), 200.0);
    QCOMPARE(scrollForZoom(1, 200, 4), 0.0);
  }
};

QTEST_GUILESS_MAIN(TstTimelineMath)
#include "tst_timeline_math.moc"
