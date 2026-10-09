#include "editor/timeline_controller.h"

#include "core/snap.h"
#include "core/timeline_doc.h"

#include <algorithm>
#include <cmath>

namespace sf::editor {

namespace {

constexpr int kStillSeconds = 5; // default length of a still on the timeline

const Item* findItem(const TimelineDoc& doc, const QString& id) { return getItem(doc, id); }

int trackIndexOf(const TimelineDoc& doc, const QString& id) {
  for (size_t i = 0; i < doc.tracks.size(); ++i) {
    if (doc.tracks[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

bool onLockedTrack(const TimelineDoc& doc, const Item& item) {
  const Track* t = getTrack(doc, item.trackId);
  return t && t->locked;
}

QString trackKindLabel(TrackKind k) {
  switch (k) {
  case TrackKind::Video: return QStringLiteral("Video");
  case TrackKind::Audio: return QStringLiteral("Audio");
  case TrackKind::Overlay: return QStringLiteral("Overlay");
  case TrackKind::Text: return QStringLiteral("Text");
  }
  return QStringLiteral("Track");
}

} // namespace

TimelineController::TimelineController(Project& project, QObject* parent) : QObject(parent), project_(project) {
  // a new document invalidates anything half done
  connect(&project_, &Project::loaded, this, &TimelineController::cancelDrag);
}

void TimelineController::setSnapEnabled(bool on) {
  if (snap_ == on) return;
  snap_ = on;
  emit optionsChanged();
}

void TimelineController::setRippleEnabled(bool on) {
  if (ripple_ == on) return;
  ripple_ = on;
  emit optionsChanged();
}

void TimelineController::setPlayhead(qint64 frame) {
  frame = std::max<qint64>(0, frame);
  if (frame == playhead_) return;
  playhead_ = frame;
  emit playheadChanged();
}

bool TimelineController::fail(const QString& text) {
  emit message(text, QStringLiteral("error"));
  return false;
}

// ---------- drags ----------

// The selection side of pressing on a clip (Timeline.tsx startDrag). Returns whether a drag begins.
bool TimelineController::start(const QString& itemId, bool shift, DragKind kind, std::vector<QString>* group) {
  const TimelineDoc& doc = project_.doc();
  const Item* item = findItem(doc, itemId);
  if (!item) return false;
  cancelDrag();
  if (const Track* track = getTrack(doc, item->trackId); track && track->locked) {
    project_.select(itemId, shift);
    return fail(QStringLiteral("Track \"%1\" is locked. Unlock it to edit its clips.").arg(track->name));
  }
  const bool wasSelected = project_.isSelected(itemId);
  if (shift) {
    project_.select(itemId, true);
    if (wasSelected) return false; // shift-click on a selected clip only deselects it
  } else if (!wasSelected) {
    project_.select(itemId);
  }
  collapseTo_.clear();
  if (kind == DragKind::Move && project_.selection().size() > 1 && group) {
    for (const Item& i : doc.items) {
      if (project_.isSelected(i.id) && !onLockedTrack(doc, i)) group->push_back(i.id);
    }
    if (!shift) collapseTo_ = itemId; // a plain click without movement narrows the selection
  }
  return true;
}

bool TimelineController::beginMove(const QString& itemId, qint64 pointerFrame, bool shift) {
  std::vector<QString> group;
  if (!start(itemId, shift, DragKind::Move, &group)) return false;
  const Item* item = findItem(project_.doc(), itemId);
  kind_ = DragKind::Move;
  dragItem_ = itemId;
  origStart_ = item->startFrame;
  origTrack_ = trackIndexOf(project_.doc(), item->trackId);
  grabOffset_ = pointerFrame - item->startFrame;
  group_ = group;
  groupStarts_.clear();
  for (const QString& id : group_) groupStarts_.push_back(findItem(project_.doc(), id)->startFrame);
  ghosts_ = {{itemId, item->startFrame, item->durationFrames, origTrack_}};
  for (const QString& id : group_) {
    if (id == itemId) continue;
    const Item* o = findItem(project_.doc(), id);
    ghosts_.push_back({id, o->startFrame, o->durationFrames, trackIndexOf(project_.doc(), o->trackId)});
  }
  guide_.reset();
  hoverBlocked_ = false;
  emit dragChanged();
  return true;
}

bool TimelineController::beginTrim(const QString& itemId, bool inEdge, bool shift) {
  if (!start(itemId, shift, inEdge ? DragKind::TrimIn : DragKind::TrimOut, nullptr)) return false;
  const Item* item = findItem(project_.doc(), itemId);
  kind_ = inEdge ? DragKind::TrimIn : DragKind::TrimOut;
  dragItem_ = itemId;
  origStart_ = item->startFrame;
  origTrack_ = trackIndexOf(project_.doc(), item->trackId);
  trimMoved_ = false;
  trimFrame_ = inEdge ? item->startFrame : itemEnd(*item);
  group_.clear();
  groupStarts_.clear();
  ghosts_ = {{itemId, item->startFrame, item->durationFrames, origTrack_}};
  guide_.reset();
  hoverBlocked_ = false;
  emit dragChanged();
  return true;
}

void TimelineController::updateMove(qint64 pointerFrame, int pointerTrackIndex) {
  if (kind_ != DragKind::Move) return;
  const TimelineDoc& doc = project_.doc();
  const Item* item = findItem(doc, dragItem_);
  if (!item) return;

  double raw = static_cast<double>(std::max<Frame>(0, pointerFrame - grabOffset_));
  Frame minOrig = origStart_;
  for (const Frame s : groupStarts_) minOrig = std::min(minOrig, s);
  const bool grouped = group_.size() > 1;
  if (grouped) raw = std::max(raw, static_cast<double>(origStart_ - minOrig));

  double start = raw;
  std::optional<double> guide;
  if (snap_) {
    const std::vector<QString> exclude = group_.empty() ? std::vector<QString>{item->id} : group_;
    SnapOptions opts;
    opts.excludeItemIds = exclude;
    opts.includePlayhead = static_cast<double>(playhead_);
    const auto cands = getSnapCandidates(doc, opts);
    const double duration = static_cast<double>(item->durationFrames);
    const auto inSnap = snapFrame(raw, cands, threshold_);
    const auto outSnap = snapFrame(raw + duration, cands, threshold_);
    if (inSnap && (!outSnap || std::fabs(inSnap->delta) <= std::fabs(outSnap->delta))) {
      start = inSnap->frame;
      guide = inSnap->frame;
    } else if (outSnap) {
      start = std::max(0.0, outSnap->frame - duration);
      if (start == outSnap->frame - duration) guide = outSnap->frame;
    }
    if (grouped && start < static_cast<double>(origStart_ - minOrig)) {
      start = static_cast<double>(origStart_ - minOrig);
      guide.reset();
    }
  }
  const auto newStart = static_cast<Frame>(std::llround(start));

  const Track* hovered = pointerTrackIndex >= 0 && pointerTrackIndex < static_cast<int>(doc.tracks.size())
                             ? &doc.tracks[static_cast<size_t>(pointerTrackIndex)]
                             : nullptr;
  const bool single = group_.size() <= 1;
  const bool allowed = hovered && single && trackAllowsItem(hovered->kind, item->type()) && !hovered->locked;
  hoverBlocked_ = hovered && (!trackAllowsItem(hovered->kind, item->type()) || hovered->locked);

  ghosts_.front().start = newStart;
  ghosts_.front().trackIndex = allowed ? pointerTrackIndex : origTrack_;
  for (size_t i = 1; i < ghosts_.size(); ++i) {
    const auto at = std::find(group_.begin(), group_.end(), ghosts_[i].itemId) - group_.begin();
    ghosts_[i].start = std::max<Frame>(0, groupStarts_[static_cast<size_t>(at)] + newStart - origStart_);
  }
  guide_ = guide;
  emit dragChanged();
}

void TimelineController::updateTrim(qint64 pointerFrame) {
  if (kind_ != DragKind::TrimIn && kind_ != DragKind::TrimOut) return;
  const TimelineDoc& doc = project_.doc();
  const Item* item = findItem(doc, dragItem_);
  if (!item) return;
  double target = static_cast<double>(std::max<qint64>(0, pointerFrame));
  std::optional<double> guide;
  if (snap_) {
    SnapOptions opts;
    opts.excludeItemIds = {item->id};
    opts.includePlayhead = static_cast<double>(playhead_);
    if (const auto hit = snapFrame(target, getSnapCandidates(doc, opts), threshold_)) {
      target = hit->frame;
      guide = hit->frame;
    }
  }
  trimFrame_ = static_cast<Frame>(std::llround(target));
  trimMoved_ = true;
  const Frame end = itemEnd(*item);
  Ghost& g = ghosts_.front();
  if (kind_ == DragKind::TrimIn) {
    g.start = std::min(trimFrame_, end - 1);
    g.duration = end - g.start;
  } else {
    g.start = item->startFrame;
    g.duration = std::max<Frame>(1, trimFrame_ - item->startFrame);
  }
  guide_ = guide;
  emit dragChanged();
}

bool TimelineController::commitDrag() {
  if (kind_ == DragKind::None) return false;
  const DragKind kind = kind_;
  const QString itemId = dragItem_;
  const QString collapse = collapseTo_;
  const std::vector<Ghost> ghosts = ghosts_;
  const std::vector<QString> group = group_;
  const std::vector<Frame> groupStarts = groupStarts_;
  const Frame origStart = origStart_;
  const int origTrack = origTrack_;
  const Frame trimFrame = trimFrame_;
  const bool trimMoved = trimMoved_;
  clearDrag();

  const TimelineDoc& doc = project_.doc();
  std::vector<Op> ops;
  QString label;
  if (kind == DragKind::Move) {
    const Ghost& primary = ghosts.front();
    const bool trackChanged = primary.trackIndex != origTrack;
    const bool startChanged = primary.start != origStart;
    if (trackChanged || startChanged) {
      ItemMove m;
      m.itemId = itemId;
      if (trackChanged) m.trackId = doc.tracks[static_cast<size_t>(primary.trackIndex)].id;
      if (startChanged) m.startFrame = primary.start;
      ops.emplace_back(m);
      if (startChanged) {
        const Frame delta = primary.start - origStart;
        for (size_t i = 0; i < group.size(); ++i) {
          if (group[i] == itemId) continue;
          ops.emplace_back(ItemMove{group[i], std::nullopt, std::max<Frame>(0, groupStarts[i] + delta)});
        }
      }
      label = ops.size() > 1 ? QStringLiteral("Move clips") : QStringLiteral("Move clip");
    }
  } else if (trimMoved) {
    const Item* item = findItem(doc, itemId);
    const bool inEdge = kind == DragKind::TrimIn;
    if (item && trimFrame != (inEdge ? item->startFrame : itemEnd(*item))) {
      ops.emplace_back(ItemTrim{itemId, inEdge ? TrimEdge::In : TrimEdge::Out, trimFrame, ripple_});
      label = inEdge ? QStringLiteral("Trim in") : QStringLiteral("Trim out");
    }
  }
  if (ops.empty()) {
    if (!collapse.isEmpty()) project_.select(collapse);
    return false;
  }
  return project_.apply(ops, label);
}

void TimelineController::cancelDrag() {
  if (kind_ == DragKind::None) return;
  clearDrag();
}

void TimelineController::clearDrag() {
  kind_ = DragKind::None;
  ghosts_.clear();
  group_.clear();
  groupStarts_.clear();
  guide_.reset();
  hoverBlocked_ = false;
  collapseTo_.clear();
  emit dragChanged();
}

// ---------- marquee ----------

void TimelineController::beginMarquee(bool additive) {
  marqueeAdditive_ = additive;
  marqueeBase_ = additive ? project_.selection() : QStringList();
}

void TimelineController::updateMarquee(double frameA, double yA, double frameB, double yB) {
  const TimelineDoc& doc = project_.doc();
  const auto layout = trackLayout(doc.tracks);
  const double f0 = std::min(frameA, frameB), f1 = std::max(frameA, frameB);
  const double y0 = std::min(yA, yB), y1 = std::max(yA, yB);
  QStringList hits;
  for (const Item& item : doc.items) {
    const int row = trackIndexOf(doc, item.trackId);
    if (row < 0) continue;
    const RowLayout& r = layout[static_cast<size_t>(row)];
    if (r.top + r.height - kRowPad < y0 || r.top + kRowPad > y1) continue;
    if (static_cast<double>(itemEnd(item)) < f0 || static_cast<double>(item.startFrame) > f1) continue;
    hits << item.id;
  }
  QStringList next = marqueeAdditive_ ? marqueeBase_ : QStringList();
  for (const QString& id : hits) {
    if (!next.contains(id)) next << id;
  }
  project_.setSelection(next);
}

// ---------- commands ----------

bool TimelineController::splitAtPlayhead() {
  const TimelineDoc& doc = project_.doc();
  const Frame at = playhead_;
  std::vector<Op> ops;
  QStringList newIds;
  int lockedSkipped = 0;
  const bool restrict = !project_.selection().isEmpty();
  for (const Item& i : doc.items) {
    if (!(at > i.startFrame && at < itemEnd(i))) continue;
    if (restrict && !project_.isSelected(i.id)) continue;
    if (onLockedTrack(doc, i)) {
      ++lockedSkipped;
      continue;
    }
    const QString id = newId(QStringLiteral("itm"));
    ops.emplace_back(ItemSplit{i.id, at, id});
    newIds << id;
  }
  if (ops.empty()) {
    if (lockedSkipped > 0) return fail(QStringLiteral("That track is locked. Unlock it to split its clips."));
    emit message(QStringLiteral("Nothing to split at the playhead."), QStringLiteral("info"));
    return false;
  }
  if (!project_.apply(ops, QStringLiteral("Split"))) return false;
  project_.setSelection(newIds);
  return true;
}

bool TimelineController::deleteSelection() {
  const TimelineDoc& doc = project_.doc();
  std::vector<QString> ids;
  int locked = 0;
  for (const QString& id : project_.selection()) {
    const Item* i = findItem(doc, id);
    if (!i) continue;
    if (onLockedTrack(doc, *i)) ++locked;
    else ids.push_back(id);
  }
  if (ids.empty()) {
    if (locked > 0) return fail(QStringLiteral("That track is locked. Unlock it to delete its clips."));
    return false;
  }
  if (!project_.apply(ItemRemove{ids, ripple_}, ripple_ ? QStringLiteral("Ripple delete") : QStringLiteral("Delete"))) return false;
  emit message(QStringLiteral("Deleted %1 clip%2").arg(ids.size()).arg(ids.size() == 1 ? QString() : QStringLiteral("s")), QStringLiteral("undo"));
  return true;
}

bool TimelineController::cloneSelection() {
  const TimelineDoc& doc = project_.doc();
  std::vector<Op> ops;
  for (const Item& i : doc.items) {
    if (project_.isSelected(i.id) && !onLockedTrack(doc, i)) ops.emplace_back(ItemClone{i.id, newId(QStringLiteral("itm")), std::nullopt, std::nullopt});
  }
  if (ops.empty()) return false;
  return project_.apply(ops, QStringLiteral("Clone"));
}

void TimelineController::selectAll() {
  QStringList ids;
  for (const Item& i : project_.doc().items) ids << i.id;
  project_.setSelection(ids);
}

void TimelineController::deselect() { project_.clearSelection(); }

bool TimelineController::nudgeSelection(qint64 frames) {
  const TimelineDoc& doc = project_.doc();
  std::vector<const Item*> movers;
  for (const Item& i : doc.items) {
    if (project_.isSelected(i.id) && !onLockedTrack(doc, i)) movers.push_back(&i);
  }
  if (movers.empty() || frames == 0) return false;
  Frame minStart = movers.front()->startFrame;
  for (const Item* i : movers) minStart = std::min(minStart, i->startFrame);
  const Frame delta = std::max<Frame>(frames, -minStart);
  if (delta == 0) return false;
  std::vector<Op> ops;
  for (const Item* i : movers) ops.emplace_back(ItemMove{i->id, std::nullopt, i->startFrame + delta});
  return project_.apply(ops, QStringLiteral("Nudge"));
}

qint64 TimelineController::assetDurationFrames(const QString& assetId) const {
  const Asset* asset = project_.asset(assetId);
  if (!asset) return 0;
  const double fps = static_cast<double>(project_.doc().project.fps);
  if (asset->kind == AssetKind::Image) return jsRound(fps * kStillSeconds);
  return std::max<Frame>(1, msToFrames(asset->durationMs, fps));
}

qint64 TimelineController::snappedDropStart(const QString& assetId, qint64 frame) const {
  Frame start = std::max<Frame>(0, frame);
  if (!snap_) return start;
  const Frame duration = assetDurationFrames(assetId);
  if (duration <= 0) return start;
  const auto cands = getSnapCandidates(project_.doc());
  const auto inSnap = snapFrame(static_cast<double>(start), cands, threshold_);
  const auto outSnap = snapFrame(static_cast<double>(start + duration), cands, threshold_);
  if (inSnap && (!outSnap || std::fabs(inSnap->delta) <= std::fabs(outSnap->delta))) start = static_cast<Frame>(inSnap->frame);
  else if (outSnap) start = static_cast<Frame>(outSnap->frame) - duration;
  return std::max<Frame>(0, start);
}

bool TimelineController::addAsset(const QString& assetId, qint64 frame, int trackIndex) {
  const TimelineDoc& doc = project_.doc();
  const Asset* asset = project_.asset(assetId);
  if (!asset) return fail(QStringLiteral("That media is not in the project."));
  if (asset->status == AssetStatus::Failed) return fail(QStringLiteral("\"%1\" could not be read, so it can't be added.").arg(asset->originalName));
  const ItemType type = asset->kind == AssetKind::Audio ? ItemType::Audio : asset->kind == AssetKind::Image ? ItemType::Image : ItemType::Video;

  const Track* track = nullptr;
  if (trackIndex >= 0 && trackIndex < static_cast<int>(doc.tracks.size())) {
    const Track& t = doc.tracks[static_cast<size_t>(trackIndex)];
    if (trackAllowsItem(t.kind, type)) track = &t;
  }
  if (!track) {
    const TrackKind want = type == ItemType::Audio ? TrackKind::Audio : TrackKind::Video;
    for (const Track& t : doc.tracks) {
      if (t.kind == want && !t.locked) {
        track = &t;
        break;
      }
    }
  }
  if (!track) return fail(QStringLiteral("No compatible track. Add a %1 track first.").arg(type == ItemType::Audio ? QStringLiteral("audio") : QStringLiteral("video")));
  if (track->locked) return fail(QStringLiteral("Track \"%1\" is locked. Unlock it to add clips.").arg(track->name));

  ItemInit init;
  init.id = newId(QStringLiteral("itm"));
  init.trackId = track->id;
  init.startFrame = snappedDropStart(assetId, frame < 0 ? playhead_ : frame);
  init.durationFrames = assetDurationFrames(assetId);
  init.assetId = asset->id;
  if (isMediaType(type)) init.sourceInFrame = 0;
  Labels labels;
  labels.name = asset->originalName;
  init.labels = labels;
  Item item;
  try {
    item = createItem(type, init);
  } catch (const std::exception& e) {
    return fail(QString::fromUtf8(e.what()));
  }
  const QString id = item.id;
  if (!project_.apply(ItemAdd{item, false}, QStringLiteral("Add clip"))) return false;
  project_.setSelection({id});
  return true;
}

bool TimelineController::addText(const QString& text, qint64 frame, double seconds, int fontSize) {
  const TimelineDoc& doc = project_.doc();
  const Track* track = nullptr;
  for (const Track& t : doc.tracks) {
    if (t.kind == TrackKind::Text && !t.locked) {
      track = &t;
      break;
    }
  }
  if (!track) return fail(QStringLiteral("No unlocked text track to put the title on."));
  TextStyle style;
  style.fontFamily = QStringLiteral("Inter");
  style.fontSize = fontSize;
  style.fontWeight = 700;
  style.color = QStringLiteral("#ffffff");
  style.align = TextAlign::Center;
  ItemInit init;
  init.id = newId(QStringLiteral("itm"));
  init.trackId = track->id;
  init.startFrame = frame < 0 ? playhead_ : frame;
  init.durationFrames = std::max<Frame>(1, jsRound(seconds * static_cast<double>(doc.project.fps)));
  init.props = TextProps{text, style};
  Item item;
  try {
    item = createItem(ItemType::Text, init);
  } catch (const std::exception& e) {
    return fail(QString::fromUtf8(e.what()));
  }
  const QString id = item.id;
  if (!project_.apply(ItemAdd{item, false}, QStringLiteral("Add text"))) return false;
  project_.setSelection({id});
  return true;
}

bool TimelineController::addShape(qint64 frame, double seconds) {
  const TimelineDoc& doc = project_.doc();
  const Track* track = nullptr;
  for (const Track& t : doc.tracks) {
    if ((t.kind == TrackKind::Text || t.kind == TrackKind::Overlay) && !t.locked) {
      track = &t;
      break;
    }
  }
  if (!track) return fail(QStringLiteral("No unlocked overlay or text track to put the shape on."));
  ShapeProps shape;
  shape.shape = ShapeKind::Rect;
  shape.fill = QStringLiteral("#5FB7A1");
  shape.width = static_cast<double>(doc.project.width) / 4;
  shape.height = static_cast<double>(doc.project.height) / 4;
  ItemInit init;
  init.id = newId(QStringLiteral("itm"));
  init.trackId = track->id;
  init.startFrame = frame < 0 ? playhead_ : frame;
  init.durationFrames = std::max<Frame>(1, jsRound(seconds * static_cast<double>(doc.project.fps)));
  init.props = shape;
  Item item;
  try {
    item = createItem(ItemType::Shape, init);
  } catch (const std::exception& e) {
    return fail(QString::fromUtf8(e.what()));
  }
  const QString id = item.id;
  if (!project_.apply(ItemAdd{item, false}, QStringLiteral("Add shape"))) return false;
  project_.setSelection({id});
  return true;
}

// ---------- tracks ----------

bool TimelineController::addTrack(const QString& kindName) {
  TrackKind kind;
  if (kindName == QLatin1String("video")) kind = TrackKind::Video;
  else if (kindName == QLatin1String("audio")) kind = TrackKind::Audio;
  else if (kindName == QLatin1String("overlay")) kind = TrackKind::Overlay;
  else if (kindName == QLatin1String("text")) kind = TrackKind::Text;
  else return fail(QStringLiteral("Unknown track kind \"%1\".").arg(kindName));
  int same = 0;
  for (const Track& t : project_.doc().tracks) same += t.kind == kind;
  TrackAdd add;
  add.trackId = newId(QStringLiteral("trk"));
  add.kind = kind;
  add.name = QStringLiteral("%1 %2").arg(trackKindLabel(kind)).arg(same + 1);
  if (kind != TrackKind::Audio) add.index = 0; // picture layers stack above, audio goes below
  return project_.apply(add, QStringLiteral("Add track"));
}

bool TimelineController::removeTrack(int index) {
  const TimelineDoc& doc = project_.doc();
  if (index < 0 || index >= static_cast<int>(doc.tracks.size())) return false;
  const Track& t = doc.tracks[static_cast<size_t>(index)];
  if (t.locked) return fail(QStringLiteral("Track \"%1\" is locked. Unlock it to remove it.").arg(t.name));
  if (doc.tracks.size() == 1) return fail(QStringLiteral("A project needs at least one track."));
  return project_.apply(TrackRemove{t.id}, QStringLiteral("Remove track"));
}

bool TimelineController::moveTrack(int from, int to) {
  const TimelineDoc& doc = project_.doc();
  const int n = static_cast<int>(doc.tracks.size());
  if (from < 0 || from >= n || to < 0 || to >= n || from == to) return false;
  std::vector<QString> ids;
  for (const Track& t : doc.tracks) ids.push_back(t.id);
  const QString moved = ids[static_cast<size_t>(from)];
  ids.erase(ids.begin() + from);
  ids.insert(ids.begin() + to, moved);
  return project_.apply(TrackReorder{ids}, QStringLiteral("Reorder tracks"));
}

bool TimelineController::renameTrack(int index, const QString& name) {
  const TimelineDoc& doc = project_.doc();
  if (index < 0 || index >= static_cast<int>(doc.tracks.size()) || name.trimmed().isEmpty()) return false;
  TrackUpdate u;
  u.trackId = doc.tracks[static_cast<size_t>(index)].id;
  u.patch.name = name.trimmed();
  return project_.apply(u, QStringLiteral("Rename track"));
}

bool TimelineController::setTrackFlag(int index, const QString& flag, bool value) {
  const TimelineDoc& doc = project_.doc();
  if (index < 0 || index >= static_cast<int>(doc.tracks.size())) return false;
  TrackUpdate u;
  u.trackId = doc.tracks[static_cast<size_t>(index)].id;
  if (flag == QLatin1String("locked")) u.patch.locked = value;
  else if (flag == QLatin1String("muted")) u.patch.muted = value;
  else if (flag == QLatin1String("hidden")) u.patch.hidden = value;
  else return false;
  return project_.apply(u, flag == QLatin1String("locked") ? QStringLiteral("Toggle lock")
                           : flag == QLatin1String("muted") ? QStringLiteral("Toggle mute")
                                                            : QStringLiteral("Toggle visibility"));
}

// ---------- navigation ----------

qint64 TimelineController::edgeFrom(qint64 frame, int dir) const {
  return neighborEdge(clipEdges(project_.doc()), frame, dir).value_or(-1);
}

qint64 TimelineController::durationFrames() const { return docDurationFrames(project_.doc()); }

} // namespace sf::editor
