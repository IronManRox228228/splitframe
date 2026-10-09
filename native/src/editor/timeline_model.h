#pragma once

// List models over the project's timeline for the UI. TimelineModel has one row per item (ordered
// by track, then start frame) and TrackModel one per track, top to bottom. Both follow the Project:
// when only values changed the rows are updated in place (dataChanged), when items or tracks came
// or went the model resets. The painted timeline reads rows() directly (no QVariant per clip).

#include "editor/media_pool.h"
#include "editor/project.h"

#include <QAbstractListModel>
#include <QColor>

namespace sf::editor {

struct ItemRow {
  QString id;
  QString trackId;
  int trackIndex = 0;
  Frame start = 0;
  Frame duration = 1;
  ItemType type = ItemType::Video;
  QString name;
  QString assetId;
  Frame sourceIn = 0;
  double speed = 1;
  int keyframeCount = 0;
  bool selected = false;
  bool trackLocked = false;
  bool trackHidden = false;
  bool trackMuted = false;
  bool missing = false; // its media file is gone
  bool operator==(const ItemRow&) const = default;
  Frame end() const { return start + duration; }
};

// Clip colour per item type (the reference's TYPE_BG palette).
QColor colorForType(ItemType type);
// Reference label rules: a text item shows its first line, media its file name.
QString itemDisplayName(const Item& item, const Asset* asset);

class TimelineModel : public QAbstractListModel {
  Q_OBJECT

public:
  enum Role {
    ItemIdRole = Qt::UserRole + 1,
    TrackIdRole,
    TrackIndexRole,
    StartRole,
    DurationRole,
    EndRole,
    TypeRole,
    NameRole,
    ColorRole,
    AssetIdRole,
    SelectedRole,
    LockedRole,
    KeyframeCountRole,
    MissingRole,
  };

  explicit TimelineModel(Project& project, QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  QHash<int, QByteArray> roleNames() const override;

  const std::vector<ItemRow>& rows() const { return rows_; }
  int rowOf(const QString& itemId) const;
  const ItemRow* find(const QString& itemId) const;

private:
  void refresh();

  Project& project_;
  std::vector<ItemRow> rows_;
};

class TrackModel : public QAbstractListModel {
  Q_OBJECT

public:
  enum Role { TrackIdRole = Qt::UserRole + 1, NameRole, KindRole, LockedRole, MutedRole, HiddenRole, HeightRole, ItemCountRole };

  explicit TrackModel(Project& project, QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  QHash<int, QByteArray> roleNames() const override;

private:
  void refresh();

  Project& project_;
  std::vector<Track> tracks_;
  std::vector<int> counts_;
};

} // namespace sf::editor
