#include "editor/timeline_model.h"

#include "core/timeline_doc.h"
#include "editor/timeline_math.h"

#include <algorithm>
#include <map>

namespace sf::editor {

QColor colorForType(ItemType type) {
  switch (type) {
  case ItemType::Video: return QColor(0x1F, 0x2A, 0x27);
  case ItemType::Image: return QColor(0x2A, 0x28, 0x30);
  case ItemType::Audio: return QColor(0x1d, 0x3b, 0x37);
  case ItemType::Text:
  case ItemType::Caption: return QColor(0x27, 0x45, 0x40);
  case ItemType::Shape: return QColor(0x2E, 0x2B, 0x24);
  case ItemType::MotionGraphic: return QColor(0x25, 0x2B, 0x33);
  }
  return QColor(0x1F, 0x2A, 0x27);
}

QString itemDisplayName(const Item& item, const Asset* asset) {
  if (const auto* text = std::get_if<TextProps>(&item.props)) {
    const QString first = text->text.trimmed().section(QLatin1Char('\n'), 0, 0);
    if (!first.isEmpty()) return first;
  }
  if (asset) return asset->originalName;
  if (item.labels.name && !item.labels.name->isEmpty()) return *item.labels.name;
  return enumName(item.type());
}

// ---------- TimelineModel ----------

TimelineModel::TimelineModel(Project& project, QObject* parent) : QAbstractListModel(parent), project_(project) {
  connect(&project_, &Project::docChanged, this, &TimelineModel::refresh);
  connect(&project_, &Project::selectionChanged, this, &TimelineModel::refresh);
  connect(&project_, &Project::assetsChanged, this, &TimelineModel::refresh);
  refresh();
}

int TimelineModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : static_cast<int>(rows_.size()); }

QHash<int, QByteArray> TimelineModel::roleNames() const {
  return {{ItemIdRole, "itemId"},   {TrackIdRole, "trackId"}, {TrackIndexRole, "trackIndex"}, {StartRole, "start"},
          {DurationRole, "duration"}, {EndRole, "end"},        {TypeRole, "type"},             {NameRole, "name"},
          {ColorRole, "color"},     {AssetIdRole, "assetId"}, {SelectedRole, "selected"},     {LockedRole, "locked"},
          {KeyframeCountRole, "keyframeCount"}, {MissingRole, "missing"}};
}

QVariant TimelineModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
  const ItemRow& r = rows_[static_cast<size_t>(index.row())];
  switch (role) {
  case ItemIdRole: return r.id;
  case TrackIdRole: return r.trackId;
  case TrackIndexRole: return r.trackIndex;
  case StartRole: return static_cast<qlonglong>(r.start);
  case DurationRole: return static_cast<qlonglong>(r.duration);
  case EndRole: return static_cast<qlonglong>(r.end());
  case TypeRole: return enumName(r.type);
  case NameRole:
  case Qt::DisplayRole: return r.name;
  case ColorRole: return colorForType(r.type);
  case AssetIdRole: return r.assetId;
  case SelectedRole: return r.selected;
  case LockedRole: return r.trackLocked;
  case KeyframeCountRole: return r.keyframeCount;
  case MissingRole: return r.missing;
  default: return {};
  }
}

int TimelineModel::rowOf(const QString& itemId) const {
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (rows_[i].id == itemId) return static_cast<int>(i);
  }
  return -1;
}

const ItemRow* TimelineModel::find(const QString& itemId) const {
  const int r = rowOf(itemId);
  return r < 0 ? nullptr : &rows_[static_cast<size_t>(r)];
}

void TimelineModel::refresh() {
  const TimelineDoc& doc = project_.doc();
  std::map<QString, int> trackIndex;
  for (size_t i = 0; i < doc.tracks.size(); ++i) trackIndex[doc.tracks[i].id] = static_cast<int>(i);

  std::vector<ItemRow> next;
  next.reserve(doc.items.size());
  for (const Item& item : doc.items) {
    ItemRow r;
    r.id = item.id;
    r.trackId = item.trackId;
    const auto ti = trackIndex.find(item.trackId);
    r.trackIndex = ti == trackIndex.end() ? 0 : ti->second;
    r.start = item.startFrame;
    r.duration = item.durationFrames;
    r.type = item.type();
    const Asset* asset = item.assetId ? project_.asset(*item.assetId) : nullptr;
    r.name = itemDisplayName(item, asset);
    r.assetId = item.assetId.value_or(QString());
    r.sourceIn = item.sourceInFrame.value_or(0);
    r.speed = item.speed;
    r.keyframeCount = static_cast<int>(item.keyframes.size());
    r.selected = project_.isSelected(item.id);
    if (const Track* t = getTrack(doc, item.trackId)) {
      r.trackLocked = t->locked;
      r.trackHidden = t->hidden;
      r.trackMuted = t->muted;
    }
    r.missing = asset && asset->status == AssetStatus::Missing;
    next.push_back(std::move(r));
  }
  std::sort(next.begin(), next.end(), [](const ItemRow& a, const ItemRow& b) {
    if (a.trackIndex != b.trackIndex) return a.trackIndex < b.trackIndex;
    if (a.start != b.start) return a.start < b.start;
    return a.id < b.id;
  });

  const bool sameIds = next.size() == rows_.size() &&
                       std::equal(next.begin(), next.end(), rows_.begin(), [](const ItemRow& a, const ItemRow& b) { return a.id == b.id; });
  if (!sameIds) {
    beginResetModel();
    rows_ = std::move(next);
    endResetModel();
    return;
  }
  // same items in the same order: tell the views which rows changed
  for (size_t i = 0; i < next.size(); ++i) {
    if (next[i] == rows_[i]) continue;
    rows_[i] = std::move(next[i]);
    const QModelIndex idx = index(static_cast<int>(i));
    emit dataChanged(idx, idx);
  }
}

// ---------- TrackModel ----------

TrackModel::TrackModel(Project& project, QObject* parent) : QAbstractListModel(parent), project_(project) {
  connect(&project_, &Project::docChanged, this, &TrackModel::refresh);
  refresh();
}

int TrackModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : static_cast<int>(tracks_.size()); }

QHash<int, QByteArray> TrackModel::roleNames() const {
  return {{TrackIdRole, "trackId"}, {NameRole, "name"},     {KindRole, "kind"},          {LockedRole, "locked"},
          {MutedRole, "muted"},     {HiddenRole, "hidden"}, {HeightRole, "rowHeight"}, {ItemCountRole, "itemCount"}};
}

QVariant TrackModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
  const size_t i = static_cast<size_t>(index.row());
  const Track& t = tracks_[i];
  switch (role) {
  case TrackIdRole: return t.id;
  case NameRole:
  case Qt::DisplayRole: return t.name;
  case KindRole: return enumName(t.kind);
  case LockedRole: return t.locked;
  case MutedRole: return t.muted;
  case HiddenRole: return t.hidden;
  case HeightRole: return rowHeightForKind(t.kind);
  case ItemCountRole: return counts_[i];
  default: return {};
  }
}

void TrackModel::refresh() {
  const TimelineDoc& doc = project_.doc();
  std::vector<int> counts(doc.tracks.size(), 0);
  for (const Item& item : doc.items) {
    for (size_t i = 0; i < doc.tracks.size(); ++i) {
      if (doc.tracks[i].id == item.trackId) ++counts[i];
    }
  }
  const bool sameIds = doc.tracks.size() == tracks_.size() &&
                       std::equal(doc.tracks.begin(), doc.tracks.end(), tracks_.begin(), [](const Track& a, const Track& b) { return a.id == b.id; });
  if (!sameIds) {
    beginResetModel();
    tracks_ = doc.tracks;
    counts_ = std::move(counts);
    endResetModel();
    return;
  }
  for (size_t i = 0; i < tracks_.size(); ++i) {
    if (tracks_[i] == doc.tracks[i] && counts_[i] == counts[i]) continue;
    tracks_[i] = doc.tracks[i];
    counts_[i] = counts[i];
    const QModelIndex idx = index(static_cast<int>(i));
    emit dataChanged(idx, idx);
  }
}

} // namespace sf::editor
