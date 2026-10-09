#pragma once

// Timeline editing: what the pointer and the keyboard do to the document. Port of the reference's
// Timeline.tsx drag logic, timeline-math.ts dragCommit and the store's edit actions, with one rule:
// every change is built as core ops and goes through Project::apply (so undo/redo, locks and the
// op log behave the same as for the agent).
//
// Drags are ghosts: while the pointer moves the document is untouched and the controller only
// tracks where the clips would land (with snapping); the ops are produced once, on commit.
// The controller speaks frames and track indices, not pixels; the view converts.

#include "editor/project.h"
#include "editor/timeline_math.h"

#include <QObject>

#include <algorithm>
#include <optional>

namespace sf::editor {

class TimelineController : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool snapEnabled READ snapEnabled WRITE setSnapEnabled NOTIFY optionsChanged)
  Q_PROPERTY(bool rippleEnabled READ rippleEnabled WRITE setRippleEnabled NOTIFY optionsChanged)
  Q_PROPERTY(qint64 playhead READ playhead WRITE setPlayhead NOTIFY playheadChanged)
  Q_PROPERTY(double snapThresholdFrames READ snapThresholdFrames WRITE setSnapThresholdFrames)
  Q_PROPERTY(bool dragging READ dragging NOTIFY dragChanged)
  Q_PROPERTY(qint64 guideFrame READ guideFrame NOTIFY dragChanged)
  Q_PROPERTY(bool hoverBlocked READ hoverBlocked NOTIFY dragChanged)

public:
  enum class DragKind { None, Move, TrimIn, TrimOut };

  // Where a dragged clip would sit right now.
  struct Ghost {
    QString itemId;
    Frame start = 0;
    Frame duration = 1;
    int trackIndex = 0;
  };

  explicit TimelineController(Project& project, QObject* parent = nullptr);

  bool snapEnabled() const { return snap_; }
  void setSnapEnabled(bool on);
  bool rippleEnabled() const { return ripple_; }
  void setRippleEnabled(bool on);
  qint64 playhead() const { return playhead_; }
  void setPlayhead(qint64 frame);
  double snapThresholdFrames() const { return threshold_; }
  void setSnapThresholdFrames(double frames) { threshold_ = std::max(1.0, frames); }

  // ---- drags (pointer) ----
  // Pointer pressed on a clip body / trim handle. False when nothing started (locked track, or a
  // shift-click that only toggled the selection).
  bool beginMove(const QString& itemId, qint64 pointerFrame, bool shift);
  bool beginTrim(const QString& itemId, bool inEdge, bool shift);
  void updateMove(qint64 pointerFrame, int pointerTrackIndex);
  void updateTrim(qint64 pointerFrame);
  // Applies the drag as one undo step. False if the drag was a no-op or an op was rejected.
  bool commitDrag();
  void cancelDrag();

  bool dragging() const { return kind_ != DragKind::None; }
  DragKind dragKind() const { return kind_; }
  const std::vector<Ghost>& ghosts() const { return ghosts_; }
  qint64 guideFrame() const { return guide_.value_or(-1); }
  // The pointer is over a track the dragged clip may not go to (kind mismatch or locked)
  bool hoverBlocked() const { return hoverBlocked_; }

  // ---- marquee ----
  void beginMarquee(bool additive);
  // Items whose row and frame range touch the rectangle; frames and y in lane space (below the ruler).
  void updateMarquee(double frameA, double yA, double frameB, double yB);

  // ---- commands ----
  Q_INVOKABLE bool splitAtPlayhead();
  Q_INVOKABLE bool deleteSelection();
  Q_INVOKABLE bool cloneSelection();
  Q_INVOKABLE void selectAll();
  Q_INVOKABLE void deselect();
  // Moves the selected clips by `frames` (clamped so none goes before frame 0).
  Q_INVOKABLE bool nudgeSelection(qint64 frames);
  // Adds an asset as a clip: on trackIndex when it fits there, else on the first track of the right
  // kind; frame is snapped to clip edges when snapping is on. The new clip becomes the selection.
  Q_INVOKABLE bool addAsset(const QString& assetId, qint64 frame, int trackIndex = -1);
  Q_INVOKABLE qint64 assetDurationFrames(const QString& assetId) const;
  Q_INVOKABLE qint64 snappedDropStart(const QString& assetId, qint64 frame) const;
  Q_INVOKABLE bool addText(const QString& text = QStringLiteral("Your title"), qint64 frame = -1, double seconds = 3, int fontSize = 96);
  Q_INVOKABLE bool addShape(qint64 frame = -1, double seconds = 3);

  // ---- tracks ----
  Q_INVOKABLE bool addTrack(const QString& kind);
  Q_INVOKABLE bool removeTrack(int index);
  Q_INVOKABLE bool moveTrack(int from, int to);
  Q_INVOKABLE bool renameTrack(int index, const QString& name);
  Q_INVOKABLE bool setTrackFlag(int index, const QString& flag, bool value); // locked, muted, hidden

  // ---- navigation ----
  // Playhead target for the Up/Down shortcuts; -1 when there is no further edge.
  Q_INVOKABLE qint64 edgeFrom(qint64 frame, int dir) const;
  Q_INVOKABLE qint64 durationFrames() const;

signals:
  void optionsChanged();
  void playheadChanged();
  void dragChanged();
  // kind: "info" or "error"
  void message(const QString& text, const QString& kind);
  void seekRequested(qint64 frame);

private:
  bool start(const QString& itemId, bool shift, DragKind kind, std::vector<QString>* group);
  std::optional<double> snapEdge(double frame, const std::vector<QString>& exclude, double* guide) const;
  void clearDrag();
  bool fail(const QString& text);

  Project& project_;
  bool snap_ = true;
  bool ripple_ = false;
  qint64 playhead_ = 0;
  double threshold_ = 8;

  DragKind kind_ = DragKind::None;
  QString dragItem_;
  Frame grabOffset_ = 0;
  Frame origStart_ = 0;
  int origTrack_ = 0;
  Frame trimFrame_ = 0;
  bool trimMoved_ = false;
  std::vector<Ghost> ghosts_;
  std::vector<QString> group_;
  std::vector<Frame> groupStarts_;
  std::optional<double> guide_;
  bool hoverBlocked_ = false;
  QString collapseTo_; // a plain click inside a multi-selection narrows it to the clicked clip
  QStringList marqueeBase_;
  bool marqueeAdditive_ = false;
};

} // namespace sf::editor
