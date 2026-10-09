#include "app/timeline_view.h"

#include "core/timeline_doc.h"

#include <QCursor>
#include <QFontMetricsF>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace sf::app {

using namespace sf::editor;
using editor::Project; // sf::Project (the schema struct) would be ambiguous

namespace {

const QColor kBg(0x0e, 0x0f, 0x12);
const QColor kRulerBg(0x10, 0x12, 0x11);
const QColor kLane(0x13, 0x15, 0x19);
const QColor kLaneAlt(0x16, 0x18, 0x1d);
const QColor kLine(0x23, 0x26, 0x2d);
const QColor kAccent(0x5F, 0xB7, 0xA1);
const QColor kText(0xe8, 0xe8, 0xea);
const QColor kFaint(0x8b, 0x90, 0x9a);

constexpr double kEdgeZone = 40; // px from the view edge where a drag starts scrolling
constexpr double kMarginFrames = 32;

QFont uiFont(int px, bool mono = false) {
  QFont f(mono ? QStringLiteral("Consolas") : QStringLiteral("Segoe UI"));
  f.setPixelSize(px);
  return f;
}

} // namespace

TimelineView::TimelineView(QQuickItem* parent) : QQuickPaintedItem(parent) {
  setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
  setAcceptHoverEvents(true);
  setAntialiasing(true);
  setFlag(ItemHasContents, true);
  setClip(true);
  edgeTimer_.setInterval(16);
  connect(&edgeTimer_, &QTimer::timeout, this, &TimelineView::edgeScroll);
}

// ---------- wiring ----------

void TimelineView::setProject(Project* p) {
  if (project_ == p) return;
  if (project_) disconnect(project_, nullptr, this, nullptr);
  project_ = p;
  if (project_) {
    connect(project_, &Project::docChanged, this, [this] {
      relayout();
      update();
    });
    connect(project_, &Project::assetsChanged, this, [this] { update(); });
  }
  relayout();
  emit projectChanged();
}

void TimelineView::setModel(TimelineModel* m) {
  if (model_ == m) return;
  if (model_) disconnect(model_, nullptr, this, nullptr);
  model_ = m;
  if (model_) {
    const auto repaint = [this] { update(); };
    connect(model_, &QAbstractItemModel::dataChanged, this, repaint);
    connect(model_, &QAbstractItemModel::modelReset, this, repaint);
    connect(model_, &QAbstractItemModel::rowsInserted, this, repaint);
    connect(model_, &QAbstractItemModel::rowsRemoved, this, repaint);
  }
  emit modelChanged();
  update();
}

void TimelineView::setController(TimelineController* c) {
  if (controller_ == c) return;
  if (controller_) disconnect(controller_, nullptr, this, nullptr);
  controller_ = c;
  if (controller_) {
    connect(controller_, &TimelineController::dragChanged, this, [this] { update(); });
    connect(controller_, &TimelineController::optionsChanged, this, [this] { update(); });
  }
  emit controllerChanged();
}

void TimelineView::setPool(MediaPool* p) {
  if (pool_ == p) return;
  if (pool_) disconnect(pool_, nullptr, this, nullptr);
  pool_ = p;
  if (pool_) connect(pool_, &MediaPool::previewReady, this, [this] { update(); });
  emit poolChanged();
}

void TimelineView::relayout() {
  layout_ = project_ ? trackLayout(project_->doc().tracks) : std::vector<RowLayout>{};
  clampScroll();
  emit viewChanged();
}

double TimelineView::fps() const { return project_ ? static_cast<double>(project_->doc().project.fps) : 30.0; }

void TimelineView::geometryChange(const QRectF& next, const QRectF& old) {
  QQuickPaintedItem::geometryChange(next, old);
  clampScroll();
  emit viewChanged();
  update();
}

// ---------- scrolling and zoom ----------

double TimelineView::contentWidth() const {
  const double total = controller_ ? static_cast<double>(controller_->durationFrames()) : 0;
  const double frames = std::max(total + std::round(160 / px_), std::round(240 / px_));
  return frames * px_;
}

double TimelineView::contentHeight() const { return totalRowsHeight(layout_); }

void TimelineView::clampScroll() {
  const double maxX = std::max(0.0, contentWidth() - width());
  const double maxY = std::max(0.0, contentHeight() - std::max(0.0, height() - kRulerHeight));
  scrollX_ = std::clamp(scrollX_, 0.0, maxX);
  scrollY_ = std::clamp(scrollY_, 0.0, maxY);
}

void TimelineView::setScrollX(double x) {
  const double before = scrollX_;
  scrollX_ = x;
  clampScroll();
  if (scrollX_ == before) return;
  emit viewChanged();
  update();
}

void TimelineView::setScrollY(double y) {
  const double before = scrollY_;
  scrollY_ = y;
  clampScroll();
  if (scrollY_ == before) return;
  emit viewChanged();
  update();
}

void TimelineView::setPlayhead(qint64 f) {
  if (playhead_ == f) return;
  playhead_ = f;
  emit viewChanged();
}

void TimelineView::zoomTo(double px, std::optional<double> anchorViewX) {
  px = clampPxPerFrame(px);
  if (px == px_) return;
  double anchorX;
  if (anchorViewX) {
    anchorX = *anchorViewX;
  } else {
    // keep the playhead put when it is on screen, else the middle of the view
    const double phX = static_cast<double>(playhead_) * px_ - scrollX_;
    anchorX = phX >= 0 && phX <= width() ? phX : width() / 2;
  }
  const double anchorFrame = (scrollX_ + anchorX) / px_;
  px_ = px;
  scrollX_ = scrollForZoom(anchorFrame, anchorX, px_);
  clampScroll();
  if (controller_) controller_->setSnapThresholdFrames(std::round(8 / px_));
  emit viewChanged();
  update();
}

void TimelineView::zoomBy(double factor) { zoomTo(px_ * factor, std::nullopt); }

void TimelineView::zoomFit() {
  const double total = std::max<double>(1, controller_ ? controller_->durationFrames() : 1);
  const double px = clampPxPerFrame(std::max(120.0, width() - 24) / total);
  px_ = px;
  scrollX_ = 0;
  clampScroll();
  if (controller_) controller_->setSnapThresholdFrames(std::round(8 / px_));
  emit viewChanged();
  update();
}

void TimelineView::followPlayhead(qint64 frame) {
  const double x = static_cast<double>(frame) * px_;
  if (x < scrollX_ || x > scrollX_ + width() - 8) setScrollX(std::max(0.0, x - kMarginFrames));
}

qint64 TimelineView::frameAt(double x) const { return std::max<qint64>(0, static_cast<qint64>(std::llround((x + scrollX_) / px_))); }

int TimelineView::trackIndexAt(double y) const { return rowIndexAtY(layout_, y - kRulerHeight + scrollY_); }

// ---------- geometry of clips ----------

QRectF TimelineView::clipRect(const ItemRow& r, qint64 start, qint64 duration, int trackIndex) const {
  if (trackIndex < 0 || trackIndex >= static_cast<int>(layout_.size())) return {};
  const RowLayout& row = layout_[static_cast<size_t>(trackIndex)];
  (void)r;
  return {static_cast<double>(start) * px_ - scrollX_, kRulerHeight + row.top + kRowPad - scrollY_, std::max(4.0, static_cast<double>(duration) * px_),
          static_cast<double>(row.height - 2 * kRowPad)};
}

TimelineView::Hit TimelineView::hitTest(double x, double y) const {
  Hit hit;
  if (!model_ || y < kRulerHeight) return hit;
  const auto& rows = model_->rows();
  for (int i = static_cast<int>(rows.size()) - 1; i >= 0; --i) {
    const ItemRow& r = rows[static_cast<size_t>(i)];
    const QRectF rect = clipRect(r, r.start, r.duration, r.trackIndex);
    if (!rect.contains(x, y)) continue;
    hit.row = i;
    const double hw = trimHandleWidth(rect.width());
    if (hw > 0 && !r.trackLocked) {
      if (x < rect.left() + hw) hit.zone = Hit::TrimIn;
      else if (x > rect.right() - hw) hit.zone = Hit::TrimOut;
    }
    return hit;
  }
  return hit;
}

QString TimelineView::itemAt(double x, double y) const {
  const Hit h = hitTest(x, y);
  return h.row < 0 ? QString() : model_->rows()[static_cast<size_t>(h.row)].id;
}

// ---------- drop preview ----------

void TimelineView::showDropPreview(double x, double y, const QString& assetId) {
  if (!controller_ || !project_) return;
  const Asset* asset = project_->asset(assetId);
  if (!asset) return;
  const int ti = trackIndexAt(y);
  drop_.active = true;
  drop_.trackIndex = ti;
  drop_.start = controller_->snappedDropStart(assetId, frameAt(x));
  drop_.duration = controller_->assetDurationFrames(assetId);
  const ItemType type = asset->kind == AssetKind::Audio ? ItemType::Audio : asset->kind == AssetKind::Image ? ItemType::Image : ItemType::Video;
  const auto& tracks = project_->doc().tracks;
  drop_.valid = ti >= 0 && ti < static_cast<int>(tracks.size()) && trackAllowsItem(tracks[static_cast<size_t>(ti)].kind, type) && !tracks[static_cast<size_t>(ti)].locked;
  update();
}

void TimelineView::clearDropPreview() {
  if (!drop_.active) return;
  drop_.active = false;
  update();
}

bool TimelineView::dropAsset(double x, double y, const QString& assetId) {
  clearDropPreview();
  return controller_ && controller_->addAsset(assetId, frameAt(x), trackIndexAt(y));
}

// ---------- pointer ----------

void TimelineView::mousePressEvent(QMouseEvent* e) {
  emit interacted();
  const QPointF pos = e->position();
  last_ = pressPos_ = pos;
  pressWasShift_ = e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
  if (!controller_ || !project_) return;

  if (e->button() == Qt::RightButton) {
    const Hit hit = hitTest(pos.x(), pos.y());
    if (hit.row < 0) return;
    const ItemRow& r = model_->rows()[static_cast<size_t>(hit.row)];
    if (!project_->isSelected(r.id)) project_->select(r.id);
    emit contextMenuRequested(r.id, pos.x(), pos.y());
    return;
  }
  if (e->button() != Qt::LeftButton) return;

  if (pos.y() < kRulerHeight) {
    mode_ = Mode::Scrub;
    emit scrubbed(frameAt(pos.x()));
    edgeTimer_.start();
    e->accept();
    return;
  }
  const Hit hit = hitTest(pos.x(), pos.y());
  if (hit.row >= 0) {
    const ItemRow& r = model_->rows()[static_cast<size_t>(hit.row)];
    const bool started = hit.zone == Hit::Body ? controller_->beginMove(r.id, frameAt(pos.x()), pressWasShift_)
                                               : controller_->beginTrim(r.id, hit.zone == Hit::TrimIn, pressWasShift_);
    mode_ = started ? Mode::Clip : Mode::Idle;
    if (started) edgeTimer_.start();
  } else {
    mode_ = Mode::Marquee;
    marqueeMoved_ = false;
    controller_->beginMarquee(pressWasShift_);
    edgeTimer_.start();
  }
  e->accept();
  update();
}

void TimelineView::handleDrag(const QPointF& pos) {
  if (!controller_) return;
  switch (mode_) {
  case Mode::Scrub: emit scrubbed(frameAt(pos.x())); break;
  case Mode::Clip:
    if (controller_->dragKind() == TimelineController::DragKind::Move) controller_->updateMove(frameAt(pos.x()), trackIndexAt(pos.y()));
    else controller_->updateTrim(frameAt(pos.x()));
    updateCursor(pos);
    break;
  case Mode::Marquee: {
    if (!marqueeMoved_ && std::hypot(pos.x() - pressPos_.x(), pos.y() - pressPos_.y()) < 4) break;
    marqueeMoved_ = true;
    const double ax = pressPos_.x() + scrollX_, bx = pos.x() + scrollX_;
    const double ay = pressPos_.y() - kRulerHeight + scrollY_, by = pos.y() - kRulerHeight + scrollY_;
    marquee_ = QRectF(QPointF(ax, ay), QPointF(bx, by)).normalized();
    controller_->updateMarquee(ax / px_, ay, bx / px_, by);
    update();
    break;
  }
  case Mode::Idle: break;
  }
}

void TimelineView::mouseMoveEvent(QMouseEvent* e) {
  last_ = e->position();
  handleDrag(last_);
  e->accept();
}

void TimelineView::mouseReleaseEvent(QMouseEvent* e) {
  if (e->button() != Qt::LeftButton) return;
  const Mode mode = mode_;
  mode_ = Mode::Idle;
  edgeTimer_.stop();
  if (!controller_) return;
  if (mode == Mode::Clip) {
    controller_->commitDrag();
  } else if (mode == Mode::Marquee) {
    if (!marqueeMoved_) {
      // a click on empty lane space parks the playhead there and clears the selection
      emit scrubbed(frameAt(e->position().x()));
      if (!pressWasShift_) controller_->deselect();
    }
    marquee_ = {};
    update();
  }
  updateCursor(e->position());
  e->accept();
}

void TimelineView::edgeScroll() {
  if (mode_ == Mode::Idle) return;
  double dx = 0;
  if (last_.x() > width() - kEdgeZone) dx = std::min(24.0, (last_.x() - (width() - kEdgeZone)) / 2);
  else if (last_.x() < kEdgeZone && scrollX_ > 0) dx = -std::min(24.0, (kEdgeZone - last_.x()) / 2);
  double dy = 0;
  if (mode_ == Mode::Marquee || mode_ == Mode::Clip) {
    if (last_.y() > height() - 20) dy = 8;
    else if (last_.y() < kRulerHeight + 8 && last_.y() >= 0 && mode_ == Mode::Marquee) dy = -8;
  }
  if (dx == 0 && dy == 0) return;
  const double bx = scrollX_, by = scrollY_;
  setScrollX(scrollX_ + dx);
  setScrollY(scrollY_ + dy);
  if (scrollX_ != bx || scrollY_ != by) handleDrag(last_);
}

void TimelineView::hoverMoveEvent(QHoverEvent* e) { updateCursor(e->position()); }

void TimelineView::hoverLeaveEvent(QHoverEvent*) {
  if (!hoverItem_.isEmpty()) {
    hoverItem_.clear();
    update();
  }
  unsetCursor();
}

void TimelineView::updateCursor(const QPointF& pos) {
  Qt::CursorShape shape = Qt::ArrowCursor;
  QString item;
  Hit::Zone zone = Hit::Body;
  if (mode_ == Mode::Clip && controller_) {
    shape = controller_->dragKind() != TimelineController::DragKind::Move ? Qt::SizeHorCursor : controller_->hoverBlocked() ? Qt::ForbiddenCursor : Qt::ClosedHandCursor;
  } else if (mode_ == Mode::Scrub) {
    shape = Qt::SizeHorCursor;
  } else if (mode_ == Mode::Idle) {
    if (pos.y() < kRulerHeight) {
      shape = Qt::SizeHorCursor;
    } else {
      const Hit h = hitTest(pos.x(), pos.y());
      if (h.row >= 0) {
        const ItemRow& r = model_->rows()[static_cast<size_t>(h.row)];
        item = r.id;
        zone = h.zone;
        shape = r.trackLocked ? Qt::ArrowCursor : h.zone == Hit::Body ? Qt::OpenHandCursor : Qt::SizeHorCursor;
      }
    }
  }
  setCursor(shape);
  if (item != hoverItem_ || zone != hoverZone_) {
    hoverItem_ = item;
    hoverZone_ = zone;
    update();
  }
}

void TimelineView::wheelEvent(QWheelEvent* e) {
  const QPoint angle = e->angleDelta();
  const QPoint pixels = e->pixelDelta();
  if (e->modifiers() & Qt::ControlModifier) {
    zoomTo(px_ * std::exp(angle.y() * 0.0012), e->position().x());
  } else if ((e->modifiers() & Qt::ShiftModifier) || (angle.x() != 0 && angle.y() == 0)) {
    const double delta = !pixels.isNull() ? (pixels.x() != 0 ? pixels.x() : pixels.y()) : (angle.x() != 0 ? angle.x() : angle.y());
    setScrollX(scrollX_ - delta);
  } else {
    const double delta = !pixels.isNull() ? pixels.y() : angle.y();
    if (contentHeight() > height() - kRulerHeight) setScrollY(scrollY_ - delta);
    else setScrollX(scrollX_ - delta); // nothing to scroll vertically: the wheel pans in time
  }
  e->accept();
}

// ---------- painting ----------

void TimelineView::paint(QPainter* p) {
  p->setRenderHint(QPainter::Antialiasing, true);
  p->setRenderHint(QPainter::SmoothPixmapTransform, true);
  p->fillRect(QRectF(0, 0, width(), height()), kBg);
  if (!project_ || !model_) return;

  p->save();
  p->setClipRect(QRectF(0, kRulerHeight, width(), height() - kRulerHeight));
  paintLanes(p);

  const auto& rows = model_->rows();
  const bool dragging = controller_ && controller_->dragging();
  const auto lifted = [&](const QString& id) {
    if (!dragging) return false;
    const auto& g = controller_->ghosts();
    return std::any_of(g.begin(), g.end(), [&](const TimelineController::Ghost& x) { return x.itemId == id; });
  };
  for (const ItemRow& r : rows) {
    const QRectF rect = clipRect(r, r.start, r.duration, r.trackIndex);
    if (rect.right() < 0 || rect.left() > width() || rect.bottom() < kRulerHeight || rect.top() > height()) continue;
    if (lifted(r.id)) continue;
    paintClip(p, r, rect, false, false, 0, 0);
  }
  if (dragging) {
    const bool trim = controller_->dragKind() != TimelineController::DragKind::Move;
    for (const auto& g : controller_->ghosts()) {
      const ItemRow* r = model_->find(g.itemId);
      if (!r) continue;
      paintClip(p, *r, clipRect(*r, g.start, g.duration, g.trackIndex), !trim, trim, g.start, g.duration);
    }
  }
  if (drop_.active && drop_.trackIndex >= 0) {
    const QRectF rect = clipRect(ItemRow{}, drop_.start, drop_.duration, drop_.trackIndex);
    p->setPen(QPen(drop_.valid ? kAccent : QColor(0xf0, 0x8a, 0x8a), 1.5, Qt::DashLine));
    p->setBrush(QColor(drop_.valid ? 0x5F : 0xf0, drop_.valid ? 0xB7 : 0x8a, drop_.valid ? 0xA1 : 0x8a, 40));
    p->drawRoundedRect(rect, 6, 6);
  }
  // snap guide
  if (dragging && controller_->guideFrame() >= 0) {
    const double x = xForFrame(static_cast<double>(controller_->guideFrame()));
    p->setPen(QPen(QColor(255, 255, 255, 190), 1));
    p->drawLine(QPointF(x, kRulerHeight), QPointF(x, height()));
  }
  // marquee
  if (mode_ == Mode::Marquee && marqueeMoved_) {
    const QRectF m(marquee_.x() - scrollX_, marquee_.y() + kRulerHeight - scrollY_, marquee_.width(), marquee_.height());
    p->setPen(QPen(kAccent, 1));
    p->setBrush(QColor(0x5F, 0xB7, 0xA1, 28));
    p->drawRect(m);
  }
  p->restore();
  paintRuler(p);
}

void TimelineView::paintLanes(QPainter* p) {
  const auto& tracks = project_->doc().tracks;
  for (size_t i = 0; i < tracks.size() && i < layout_.size(); ++i) {
    const RowLayout& row = layout_[i];
    const QRectF rect(0, kRulerHeight + row.top - scrollY_, width(), row.height);
    if (rect.bottom() < kRulerHeight || rect.top() > height()) continue;
    p->fillRect(rect, i % 2 == 0 ? kLane : kLaneAlt);
    if (tracks[i].locked) p->fillRect(rect, QBrush(QColor(255, 255, 255, 14), Qt::BDiagPattern));
    p->setPen(kLine);
    p->drawLine(QPointF(0, rect.bottom() - 0.5), QPointF(width(), rect.bottom() - 0.5));
  }
  // markers run through the lanes as faint lines
  p->setPen(QColor(0xfb, 0xbf, 0x24, 70));
  for (const Marker& m : project_->doc().markers) {
    const double x = xForFrame(static_cast<double>(m.frame));
    if (x >= 0 && x <= width()) p->drawLine(QPointF(x, kRulerHeight), QPointF(x, height()));
  }
}

void TimelineView::paintRuler(QPainter* p) {
  p->save();
  p->fillRect(QRectF(0, 0, width(), kRulerHeight), kRulerBg);
  p->setPen(kLine);
  p->drawLine(QPointF(0, kRulerHeight - 0.5), QPointF(width(), kRulerHeight - 0.5));
  const double fps = this->fps();
  const Frame interval = rulerInterval(px_, fps);
  const int subs = rulerSubdivisions(interval, px_);
  const double f0 = scrollX_ / px_;
  const double f1 = (scrollX_ + width()) / px_;
  p->setFont(uiFont(11, true));
  const Frame first = std::max<Frame>(0, static_cast<Frame>(std::floor(f0 / static_cast<double>(interval))) * interval);
  for (Frame f = first; static_cast<double>(f) <= f1; f += interval) {
    const double x = xForFrame(static_cast<double>(f));
    p->setPen(kLine);
    p->drawLine(QPointF(x, 0), QPointF(x, kRulerHeight));
    p->setPen(kFaint);
    p->drawText(QPointF(x + 7, 19), formatTimecode(f, fps));
    if (subs > 0) {
      p->setPen(QColor(255, 255, 255, 40));
      const double step = static_cast<double>(interval) / subs;
      for (int s = 1; s < subs; ++s) {
        const double sx = xForFrame(static_cast<double>(f) + step * s);
        p->drawLine(QPointF(sx, kRulerHeight - 6), QPointF(sx, kRulerHeight));
      }
    }
  }
  p->setPen(Qt::NoPen);
  p->setBrush(QColor(0xfb, 0xbf, 0x24));
  for (const Marker& m : project_->doc().markers) {
    const double x = xForFrame(static_cast<double>(m.frame));
    if (x < -6 || x > width() + 6) continue;
    const QPointF tri[3] = {{x - 5, kRulerHeight - 10}, {x + 5, kRulerHeight - 10}, {x, kRulerHeight - 1}};
    p->drawPolygon(tri, 3);
  }
  if (controller_ && controller_->dragging() && controller_->guideFrame() >= 0) {
    const double x = xForFrame(static_cast<double>(controller_->guideFrame()));
    p->setPen(QPen(QColor(255, 255, 255, 190), 1));
    p->drawLine(QPointF(x, 0), QPointF(x, kRulerHeight));
  }
  p->restore();
}

void TimelineView::paintClip(QPainter* p, const ItemRow& r, const QRectF& rect, bool lifted, bool trimming, qint64 trimStart, qint64 trimDur) {
  p->save();
  QPainterPath path;
  path.addRoundedRect(rect, 6, 6);
  const bool dim = r.trackHidden || r.trackLocked;
  p->setOpacity(lifted ? 0.92 : (trimming ? 0.8 : (dim ? 0.6 : 1.0)));
  if (lifted) {
    p->setPen(Qt::NoPen);
    p->setBrush(QColor(0, 0, 0, 90));
    p->drawRoundedRect(rect.translated(0, 3), 6, 6);
  }
  p->fillPath(path, colorForType(r.type));

  // content, clipped to the rounded clip
  p->save();
  p->setClipPath(path, Qt::IntersectClip);
  const std::shared_ptr<const AssetPreview> preview = pool_ && !r.assetId.isEmpty() ? pool_->preview(r.assetId) : nullptr;
  const double fps = this->fps();
  if (preview && (r.type == ItemType::Video || r.type == ItemType::Image) && !preview->strip.empty()) {
    const auto& strip = preview->strip;
    const double tileH = rect.height();
    const QSize first = strip.front().image.size();
    const double tileW = std::max(24.0, tileH * first.width() / std::max(1, first.height()));
    const double from = std::max(rect.left(), 0.0);
    const double to = std::min(rect.right(), width());
    const double startX = rect.left() + std::floor((from - rect.left()) / tileW) * tileW;
    p->setOpacity(p->opacity() * 0.85);
    for (double x = startX; x < to; x += tileW) {
      const double frames = (x - rect.left()) / px_;
      const double sec = (static_cast<double>(r.sourceIn) + frames * r.speed) / fps;
      size_t idx = 0;
      if (r.type == ItemType::Video) {
        // the last thumbnail taken at or before this moment of the source
        const auto it = std::upper_bound(strip.begin(), strip.end(), sec, [](double s, const Thumbnail& t) { return s < t.requestedSec; });
        idx = it == strip.begin() ? 0 : static_cast<size_t>(it - strip.begin()) - 1;
      }
      p->drawImage(QRectF(x, rect.top(), tileW, tileH), strip[idx].image);
    }
    p->setOpacity(lifted ? 0.92 : (trimming ? 0.8 : (dim ? 0.6 : 1.0)));
  } else if (preview && r.type == ItemType::Audio && preview->peaks.bucketCount() > 0) {
    const WaveformPeaks& wp = preview->peaks;
    const double mid = rect.center().y();
    const double half = rect.height() / 2 - 3;
    const double from = std::max(rect.left(), 0.0);
    const double to = std::min(rect.right(), width());
    const double bucketsPerSec = static_cast<double>(wp.sampleRate) / wp.samplesPerPeak;
    p->setPen(QPen(QColor(0x7f, 0xd6, 0xc0, 190), 1));
    for (double x = std::floor(from); x < to; x += 2) {
      const double f0 = (x - rect.left()) / px_, f1 = (x + 2 - rect.left()) / px_;
      const double s0 = (static_cast<double>(r.sourceIn) + f0 * r.speed) / fps;
      const double s1 = (static_cast<double>(r.sourceIn) + f1 * r.speed) / fps;
      const qint64 b0 = std::clamp<qint64>(static_cast<qint64>(s0 * bucketsPerSec), 0, wp.bucketCount() - 1);
      const qint64 b1 = std::clamp<qint64>(static_cast<qint64>(s1 * bucketsPerSec), b0, wp.bucketCount() - 1);
      const qint64 stride = std::max<qint64>(1, (b1 - b0) / 8);
      float lo = 0, hi = 0;
      for (qint64 b = b0; b <= b1; b += stride) {
        lo = std::min(lo, wp.minAt(b));
        hi = std::max(hi, wp.maxAt(b));
      }
      p->drawLine(QPointF(x + 0.5, mid - std::max(1.0, static_cast<double>(hi) * half)), QPointF(x + 0.5, mid - static_cast<double>(lo) * half));
    }
  } else if (r.type == ItemType::Caption || r.type == ItemType::Text) {
    // a hint of text so the clip reads as a title at a glance
    p->setPen(QColor(255, 255, 255, 28));
    p->setBrush(Qt::NoBrush);
    p->drawLine(QPointF(rect.left() + 8, rect.center().y() + 8), QPointF(rect.right() - 8, rect.center().y() + 8));
  }
  if (r.missing) {
    p->fillRect(rect, QBrush(QColor(0xf0, 0x8a, 0x8a, 60), Qt::BDiagPattern));
  }
  p->restore();

  p->setBrush(Qt::NoBrush);
  p->setPen(r.selected ? QPen(kAccent, 1.5) : QPen(QColor(255, 255, 255, 22), 1));
  p->drawPath(path);

  // trim handle highlight under the pointer
  if (hoverItem_ == r.id && hoverZone_ != Hit::Body && !lifted) {
    p->setPen(Qt::NoPen);
    p->setBrush(QColor(255, 255, 255, 170));
    const double bar = 3;
    p->drawRoundedRect(hoverZone_ == Hit::TrimIn ? QRectF(rect.left(), rect.top() + 3, bar, rect.height() - 6) : QRectF(rect.right() - bar, rect.top() + 3, bar, rect.height() - 6), 1.5, 1.5);
  }

  // name pill, keyframe badge
  p->setOpacity(1.0);
  if (rect.width() > 36) {
    p->setFont(uiFont(11));
    const QFontMetricsF fm(p->font());
    const QString label = fm.elidedText(r.missing ? r.name + QStringLiteral(" (missing)") : r.name, Qt::ElideRight, static_cast<qreal>(std::max(8.0, rect.width() - 24)));
    const QRectF pill(rect.left() + 6, rect.top() + 6, fm.horizontalAdvance(label) + 12, 17);
    const double clampedLeft = std::max(pill.left(), std::min(rect.right() - pill.width() - 4, std::max(rect.left() + 6, 6.0)));
    const QRectF drawn(clampedLeft, pill.top(), pill.width(), pill.height());
    p->setPen(Qt::NoPen);
    p->setBrush(QColor(8, 9, 11, 150));
    p->drawRoundedRect(drawn, 8.5, 8.5);
    p->setPen(r.missing ? QColor(0xf0, 0x8a, 0x8a) : kText);
    p->drawText(drawn.adjusted(6, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft, label);
  }
  if (r.keyframeCount > 0 && rect.width() > 48) {
    p->setFont(uiFont(10, true));
    const QString txt = QString::number(r.keyframeCount);
    const QRectF badge(rect.right() - 30, rect.bottom() - 19, 26, 15);
    p->setPen(Qt::NoPen);
    p->setBrush(QColor(8, 9, 11, 160));
    p->drawRoundedRect(badge, 4, 4);
    p->setBrush(kText);
    p->save();
    p->translate(badge.left() + 7, badge.center().y());
    p->rotate(45);
    p->drawRect(QRectF(-2.5, -2.5, 5, 5));
    p->restore();
    p->setPen(kText);
    p->drawText(badge.adjusted(12, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft, txt);
  }
  if (trimming) {
    p->setFont(uiFont(11, true));
    const QString txt = QStringLiteral("%1 · %2").arg(formatTimecode(trimStart, fps), formatDurationBadge(trimDur, fps));
    const QFontMetricsF fm(p->font());
    const QRectF badge(rect.right() + 8, rect.center().y() - 9, fm.horizontalAdvance(txt) + 14, 18);
    p->setPen(QPen(QColor(255, 255, 255, 50), 1));
    p->setBrush(QColor(8, 9, 11, 235));
    p->drawRoundedRect(badge, 9, 9);
    p->setPen(kText);
    p->drawText(badge, Qt::AlignCenter, txt);
  }
  p->restore();
}

} // namespace sf::app
