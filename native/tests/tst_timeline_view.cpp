// Drives the real painted timeline with synthetic mouse and wheel events in an offscreen window
// (software scene graph, no GPU) and checks the document that comes out the other side.

#include "app/timeline_view.h"
#include "editor_testutil.h"

#include <QGuiApplication>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QWheelEvent>

namespace tst {
using namespace sf;
using namespace sf::editor;
using namespace sf::test;
using sf::editor::Project; // not sf::Project

namespace {
const QString V1 = QStringLiteral("itm_v1"), V2 = QStringLiteral("itm_v2"), A1 = QStringLiteral("itm_a1"), T1 = QStringLiteral("itm_t1");
constexpr int kRuler = kRulerHeight;
// fixture geometry at 3 px/frame: rows text 0-44, video 44-108, audio 108-156 (below the ruler)
constexpr int kVideoY = kRuler + 44 + 30; // middle of the video row
} // namespace

class TstTimelineView : public QObject {
  Q_OBJECT

  std::unique_ptr<QQuickWindow> window_;
  std::unique_ptr<Project> project_;
  std::unique_ptr<TimelineModel> model_;
  std::unique_ptr<MediaPool> pool_;
  std::unique_ptr<TimelineController> controller_;
  sf::app::TimelineView* view_ = nullptr;

  void setup() {
    project_ = std::make_unique<Project>();
    fillProject(*project_);
    model_ = std::make_unique<TimelineModel>(*project_);
    pool_ = std::make_unique<MediaPool>(*project_);
    controller_ = std::make_unique<TimelineController>(*project_);
    controller_->setSnapEnabled(false);
    window_ = std::make_unique<QQuickWindow>();
    window_->resize(1000, 400);
    view_ = new sf::app::TimelineView(window_->contentItem());
    view_->setSize(QSizeF(1000, 400));
    view_->setProject(project_.get());
    view_->setModel(model_.get());
    view_->setController(controller_.get());
    view_->setPool(pool_.get());
    view_->setPxPerFrame(3);
    window_->show();
    QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
  }

private slots:
  void init() { setup(); }
  void cleanup() {
    window_.reset();
    controller_.reset();
    pool_.reset();
    model_.reset();
    project_.reset();
  }

  void coordinateHelpers() {
    QCOMPARE(view_->frameAt(300), qint64(100));
    QCOMPARE(view_->trackIndexAt(kVideoY), 1);
    QCOMPARE(view_->trackIndexAt(kRuler + 120), 2);
    QCOMPARE(view_->xForFrame(100), 300.0);
    QCOMPARE(view_->itemAt(150, kVideoY), V1);
    QCOMPARE(view_->itemAt(700, kVideoY), QString());
  }

  void rulerClickAndDragScrubs() {
    QSignalSpy scrub(view_, &sf::app::TimelineView::scrubbed);
    QTest::mousePress(window_.get(), Qt::LeftButton, {}, QPoint(300, 10));
    QTest::mouseMove(window_.get(), QPoint(360, 10));
    QTest::mouseRelease(window_.get(), Qt::LeftButton, {}, QPoint(360, 10));
    QVERIFY(scrub.count() >= 2);
    QCOMPARE(scrub.first().first().toLongLong(), qint64(100));
    QCOMPARE(scrub.last().first().toLongLong(), qint64(120));
  }

  void clickSelectsAndEmptyClickDeselectsAndSeeks() {
    QSignalSpy scrub(view_, &sf::app::TimelineView::scrubbed);
    QTest::mouseClick(window_.get(), Qt::LeftButton, {}, QPoint(150, kVideoY));
    QCOMPARE(project_->selection(), QStringList{V1});
    QTest::mouseClick(window_.get(), Qt::LeftButton, Qt::ShiftModifier, QPoint(500, kVideoY));
    QCOMPARE(project_->selection(), (QStringList{V1, V2}));
    QTest::mouseClick(window_.get(), Qt::LeftButton, {}, QPoint(800, kVideoY)); // empty lane
    QVERIFY(project_->selection().isEmpty());
    QCOMPARE(scrub.last().first().toLongLong(), qint64(267)); // 800 / 3
  }

  void draggingAClipMovesIt() {
    QTest::mousePress(window_.get(), Qt::LeftButton, {}, QPoint(300, kVideoY)); // v2 starts at 360: not v2
    QTest::mouseRelease(window_.get(), Qt::LeftButton, {}, QPoint(300, kVideoY));
    QTest::mousePress(window_.get(), Qt::LeftButton, {}, QPoint(450, kVideoY)); // inside v2 (x 360..630)
    QTest::mouseMove(window_.get(), QPoint(480, kVideoY));
    QTest::mouseMove(window_.get(), QPoint(510, kVideoY)); // +60 px = +20 frames
    QVERIFY(controller_->dragging());
    QCOMPARE(itemOf(*project_, V2).startFrame, Frame(120)); // not yet
    QTest::mouseRelease(window_.get(), Qt::LeftButton, {}, QPoint(510, kVideoY));
    QCOMPARE(itemOf(*project_, V2).startFrame, Frame(140));
    QVERIFY(project_->undo());
    QCOMPARE(itemOf(*project_, V2).startFrame, Frame(120));
  }

  void draggingToAnotherTrackKindIsRefused() {
    QTest::mousePress(window_.get(), Qt::LeftButton, {}, QPoint(450, kVideoY));
    QTest::mouseMove(window_.get(), QPoint(450, kRuler + 120)); // audio row
    QVERIFY(controller_->hoverBlocked());
    QTest::mouseRelease(window_.get(), Qt::LeftButton, {}, QPoint(450, kRuler + 120));
    QCOMPARE(itemOf(*project_, V2).trackId, trackNamed(project_->doc(), TrackKind::Video).id);
  }

  void trimHandlesTrim() {
    // v1 spans x 0..270; its right handle is the last 8 px
    QTest::mousePress(window_.get(), Qt::LeftButton, {}, QPoint(267, kVideoY));
    QTest::mouseMove(window_.get(), QPoint(250, kVideoY));
    QTest::mouseMove(window_.get(), QPoint(240, kVideoY));
    QTest::mouseRelease(window_.get(), Qt::LeftButton, {}, QPoint(240, kVideoY));
    QCOMPARE(itemOf(*project_, V1).durationFrames, Frame(80));
    // v2's left handle: x 360..368
    QTest::mousePress(window_.get(), Qt::LeftButton, {}, QPoint(363, kVideoY));
    QTest::mouseMove(window_.get(), QPoint(390, kVideoY));
    QTest::mouseRelease(window_.get(), Qt::LeftButton, {}, QPoint(390, kVideoY));
    QCOMPARE(itemOf(*project_, V2).startFrame, Frame(130));
    QCOMPARE(itemOf(*project_, V2).durationFrames, Frame(80));
  }

  void lockedTrackClipsDoNotDrag() {
    TrackUpdate lock;
    lock.trackId = trackNamed(project_->doc(), TrackKind::Video).id;
    lock.patch.locked = true;
    QVERIFY(project_->apply(lock));
    QSignalSpy msg(controller_.get(), &TimelineController::message);
    QTest::mousePress(window_.get(), Qt::LeftButton, {}, QPoint(450, kVideoY));
    QTest::mouseMove(window_.get(), QPoint(520, kVideoY));
    QTest::mouseRelease(window_.get(), Qt::LeftButton, {}, QPoint(520, kVideoY));
    QCOMPARE(itemOf(*project_, V2).startFrame, Frame(120));
    QVERIFY(msg.count() >= 1);
  }

  void marqueeSelects() {
    QTest::mousePress(window_.get(), Qt::LeftButton, {}, QPoint(800, kRuler + 120)); // empty audio lane
    QTest::mouseMove(window_.get(), QPoint(500, kRuler + 80));
    QTest::mouseMove(window_.get(), QPoint(20, kRuler + 70));
    QTest::mouseRelease(window_.get(), Qt::LeftButton, {}, QPoint(20, kRuler + 70));
    QVERIFY(project_->isSelected(V1));
    QVERIFY(project_->isSelected(V2));
    QVERIFY(project_->isSelected(A1));
    QVERIFY(!project_->isSelected(T1));
  }

  void ctrlWheelZoomsAroundThePointer() {
    view_->setPxPerFrame(10); // wide enough content that the scroll offset is not clamped at 0
    view_->setScrollX(400);
    const double before = view_->pxPerFrame();
    const qint64 under = view_->frameAt(300);
    QWheelEvent e(QPointF(300, 150), window_->mapToGlobal(QPointF(300, 150)), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(window_.get(), &e);
    QVERIFY(view_->pxPerFrame() > before);
    QVERIFY(std::abs(view_->frameAt(300) - under) <= 1); // the frame under the pointer stays put
  }

  void shiftWheelScrollsSideways() {
    view_->setPxPerFrame(20); // content now much wider than the view
    const double x0 = view_->scrollX();
    QWheelEvent e(QPointF(300, 150), window_->mapToGlobal(QPointF(300, 150)), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::ShiftModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(window_.get(), &e);
    QVERIFY(view_->scrollX() > x0);
  }

  void zoomFitShowsTheWholeProject() {
    view_->zoomFit();
    const double contentEnd = view_->xForFrame(static_cast<double>(controller_->durationFrames()));
    QVERIFY(contentEnd <= view_->width());
    QVERIFY(contentEnd > view_->width() * 0.7);
    QCOMPARE(view_->scrollX(), 0.0);
  }

  void followPlayheadScrollsIntoView() {
    view_->setPxPerFrame(20);
    view_->followPlayhead(100);
    QVERIFY(view_->xForFrame(100) >= 0 && view_->xForFrame(100) < view_->width());
  }

  void rightClickRequestsContextMenuAndSelects() {
    QSignalSpy menu(view_, &sf::app::TimelineView::contextMenuRequested);
    QTest::mouseClick(window_.get(), Qt::RightButton, {}, QPoint(450, kVideoY));
    QCOMPARE(menu.count(), 1);
    QCOMPARE(menu.first().first().toString(), V2);
    QCOMPARE(project_->selection(), QStringList{V2});
  }

  void droppingMediaAddsAClip() {
    view_->showDropPreview(700, kVideoY, QStringLiteral("ast_video"));
    QVERIFY(view_->dropAsset(700, kVideoY, QStringLiteral("ast_video")));
    QCOMPARE(project_->doc().items.size(), size_t(5));
    const Item& added = itemOf(*project_, project_->selection().first());
    QCOMPARE(added.startFrame, Frame(233));
    QVERIFY(!view_->dropAsset(700, kVideoY, QStringLiteral("ast_missing")));
  }

  void paintsClipsAndRuler() {
    QImage img = window_->grabWindow();
    QVERIFY(!img.isNull());
    const QColor lane = img.pixelColor(900, kVideoY);
    const QColor clip = img.pixelColor(200, kVideoY + 15); // inside v1, below the name pill
    QVERIFY(lane != clip);
    QCOMPARE(clip.green() > clip.red(), true); // the video clip colour is teal-ish
    // the ruler paints something other than the lane background along its top
    bool ruled = false;
    for (int x = 0; x < 300 && !ruled; ++x) ruled = img.pixelColor(x, 20) != img.pixelColor(900, 20);
    QVERIFY(ruled);
  }

  void staysResponsiveWithHundredsOfClips() {
    std::vector<Op> ops;
    for (int i = 0; i < 800; ++i) {
      ItemInit init;
      init.id = QStringLiteral("itm_bulk%1").arg(i, 4, 10, QLatin1Char('0'));
      init.trackId = trackNamed(project_->doc(), TrackKind::Video).id;
      init.startFrame = 1000 + i * 12;
      init.durationFrames = 10;
      init.assetId = QStringLiteral("ast_video");
      init.sourceInFrame = 0;
      ops.emplace_back(ItemAdd{createItem(ItemType::Video, init), false});
    }
    QVERIFY(project_->apply(ops));
    view_->zoomFit();
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < 10; ++i) {
      view_->setScrollX(view_->scrollX() + 5);
      QVERIFY(!window_->grabWindow().isNull());
    }
    QVERIFY2(t.elapsed() < 8000, qPrintable(QStringLiteral("10 repaints of 800 clips took %1 ms").arg(t.elapsed())));
  }
};

} // namespace tst

int main(int argc, char** argv) {
  QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
  QGuiApplication app(argc, argv);
  tst::TstTimelineView t;
  return QTest::qExec(&t, argc, argv);
}
#include "tst_timeline_view.moc"
