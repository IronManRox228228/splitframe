#pragma once

// The timeline surface: ruler, lanes and clips painted in one item (QPainter, culled to the visible
// range, so hundreds of clips cost one paint, not hundreds of delegates), plus all pointer handling
// on it. It owns only view state (zoom, scroll, hover, marquee); every edit goes through the
// TimelineController, and the playhead line is a QML overlay so playback does not repaint the clips.
//
// Layout: the ruler occupies the top kRulerHeight pixels; lanes start below it and scroll with
// scrollY. Track headers are separate QML items to the left that follow scrollY.

#include "editor/media_pool.h"
#include "editor/project.h"
#include "editor/timeline_controller.h"
#include "editor/timeline_model.h"

#include <QElapsedTimer>
#include <QPointF>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickPaintedItem>
#include <QTimer>

namespace sf::app {

class TimelineView : public QQuickPaintedItem {
  Q_OBJECT
  QML_ELEMENT
  Q_PROPERTY(sf::editor::Project* project READ project WRITE setProject NOTIFY projectChanged)
  Q_PROPERTY(sf::editor::TimelineModel* model READ model WRITE setModel NOTIFY modelChanged)
  Q_PROPERTY(sf::editor::TimelineController* controller READ controller WRITE setController NOTIFY controllerChanged)
  Q_PROPERTY(sf::editor::MediaPool* pool READ pool WRITE setPool NOTIFY poolChanged)
  Q_PROPERTY(double pxPerFrame READ pxPerFrame WRITE setPxPerFrame NOTIFY viewChanged)
  Q_PROPERTY(double scrollX READ scrollX WRITE setScrollX NOTIFY viewChanged)
  Q_PROPERTY(double scrollY READ scrollY WRITE setScrollY NOTIFY viewChanged)
  Q_PROPERTY(double contentWidth READ contentWidth NOTIFY viewChanged)
  Q_PROPERTY(double contentHeight READ contentHeight NOTIFY viewChanged)
  Q_PROPERTY(double rulerHeight READ rulerHeight CONSTANT)
  Q_PROPERTY(double fps READ fps NOTIFY viewChanged)
  Q_PROPERTY(qint64 playhead READ playhead WRITE setPlayhead NOTIFY viewChanged)

public:
  explicit TimelineView(QQuickItem* parent = nullptr);

  editor::Project* project() const { return project_; }
  void setProject(editor::Project* p);
  editor::TimelineModel* model() const { return model_; }
  void setModel(editor::TimelineModel* m);
  editor::TimelineController* controller() const { return controller_; }
  void setController(editor::TimelineController* c);
  editor::MediaPool* pool() const { return pool_; }
  void setPool(editor::MediaPool* p);

  double pxPerFrame() const { return px_; }
  void setPxPerFrame(double px) { zoomTo(px, std::nullopt); }
  double scrollX() const { return scrollX_; }
  void setScrollX(double x);
  double scrollY() const { return scrollY_; }
  void setScrollY(double y);
  double contentWidth() const;
  double contentHeight() const;
  double rulerHeight() const { return editor::kRulerHeight; }
  double fps() const;
  qint64 playhead() const { return playhead_; }
  void setPlayhead(qint64 f);

  Q_INVOKABLE void zoomBy(double factor);
  Q_INVOKABLE void zoomFit();
  Q_INVOKABLE double xForFrame(double frame) const { return frame * px_ - scrollX_; }
  Q_INVOKABLE qint64 frameAt(double x) const;
  Q_INVOKABLE int trackIndexAt(double y) const;
  // Scrolls so the frame is on screen (keeps a margin); used while the playhead moves in playback.
  Q_INVOKABLE void followPlayhead(qint64 frame);
  Q_INVOKABLE QString itemAt(double x, double y) const;

  // media dragged in from the pool
  Q_INVOKABLE void showDropPreview(double x, double y, const QString& assetId);
  Q_INVOKABLE void clearDropPreview();
  Q_INVOKABLE bool dropAsset(double x, double y, const QString& assetId);

  void paint(QPainter* painter) override;

signals:
  void projectChanged();
  void modelChanged();
  void controllerChanged();
  void poolChanged();
  void viewChanged();
  void scrubbed(qint64 frame);          // ruler click/drag: seek the player
  void interacted();                    // the user pressed in the timeline: take keyboard focus
  void contextMenuRequested(const QString& itemId, double x, double y);

protected:
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void hoverMoveEvent(QHoverEvent* e) override;
  void hoverLeaveEvent(QHoverEvent* e) override;
  void wheelEvent(QWheelEvent* e) override;
  void geometryChange(const QRectF& next, const QRectF& old) override;

private:
  enum class Mode { Idle, Scrub, Clip, Marquee };
  struct Hit {
    int row = -1; // index into model rows
    enum Zone { Body, TrimIn, TrimOut } zone = Body;
  };

  void relayout();
  void clampScroll();
  void zoomTo(double px, std::optional<double> anchorViewX);
  Hit hitTest(double x, double y) const;
  QRectF clipRect(const editor::ItemRow& r, qint64 start, qint64 duration, int trackIndex) const;
  void handleDrag(const QPointF& pos);
  void updateCursor(const QPointF& pos);
  void edgeScroll();

  void paintRuler(QPainter* p);
  void paintLanes(QPainter* p);
  void paintClip(QPainter* p, const editor::ItemRow& r, const QRectF& rect, bool lifted, bool trimming, qint64 trimStart, qint64 trimDur);

  QPointer<editor::Project> project_;
  QPointer<editor::TimelineModel> model_;
  QPointer<editor::TimelineController> controller_;
  QPointer<editor::MediaPool> pool_;

  double px_ = editor::kDefaultPxPerFrame;
  double scrollX_ = 0;
  double scrollY_ = 0;
  qint64 playhead_ = 0;
  std::vector<editor::RowLayout> layout_;

  Mode mode_ = Mode::Idle;
  QPointF last_;
  QPointF pressPos_;
  bool marqueeMoved_ = false;
  bool pressWasShift_ = false;
  QRectF marquee_; // content coordinates (x in frames * px, y in lane space)
  QTimer edgeTimer_;

  QString hoverItem_;
  Hit::Zone hoverZone_ = Hit::Body;

  struct DropPreview {
    bool active = false;
    qint64 start = 0;
    qint64 duration = 0;
    int trackIndex = 0;
    bool valid = true;
  } drop_;
};

} // namespace sf::app
