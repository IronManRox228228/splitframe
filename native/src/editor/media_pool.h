#pragma once

// The media pool: the project's assets as a list model for the UI, plus the background work that
// turns a dropped file into one (probe, then thumbnails or waveform). Assets live in the Project's
// bundle (that is what gets saved); previews are derived data, kept in memory and rebuilt on open.
//
// Everything that touches the disk or decodes runs on the pool's own two worker threads; results
// come back as queued calls, so the model and the project are only ever touched on the UI thread.
// Decoding is software on purpose: a thumbnail strip is a handful of keyframes, not worth a GPU
// session (and the GPU is shared with other work).

#include "editor/project.h"
#include "media/thumbnails.h"
#include "media/waveform.h"

#include <QAbstractListModel>
#include <QImage>
#include <QMutex>
#include <QThreadPool>

#include <atomic>
#include <memory>

namespace sf::editor {

// What the timeline and pool cards draw for an asset. Immutable once published.
struct AssetPreview {
  QImage poster;                  // first thumbnail: the pool card
  std::vector<Thumbnail> strip;   // evenly spaced over the clip (video, image)
  WaveformPeaks peaks;            // audio
  double durationSec = 0;
};

struct ImportSummary {
  QStringList added;      // asset ids
  QStringList duplicates; // already in the project (asset ids of the existing ones)
  QStringList skipped;    // file names that are not media
};

class MediaPool : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(int count READ count NOTIFY countChanged)
  Q_PROPERTY(int pending READ pending NOTIFY pendingChanged)
  Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterChanged)
  Q_PROPERTY(QString filterKind READ filterKind WRITE setFilterKind NOTIFY filterChanged)

public:
  enum Role {
    IdRole = Qt::UserRole + 1,
    NameRole,
    KindRole,
    StatusRole,
    DurationRole, // text, "0:12"
    DurationMsRole,
    DetailRole,   // "1920×1080 · 30 fps"
    PathRole,
    MissingRole,
    ErrorRole,
    ThumbRole,    // image://pool/<id>?<revision>
    UsesRole,     // clips on the timeline
  };

  explicit MediaPool(Project& project, QObject* parent = nullptr);
  ~MediaPool() override;

  int rowCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  QHash<int, QByteArray> roleNames() const override;

  int count() const { return static_cast<int>(rows_.size()); }
  int pending() const { return pending_; }
  QString filterText() const { return filterText_; }
  QString filterKind() const { return filterKind_; }
  void setFilterText(const QString& t);
  void setFilterKind(const QString& k); // "" or "all", "video", "audio", "image"

  // Adds each readable media file as an asset (status importing) and starts probing it. Files
  // already in the project are not added twice.
  ImportSummary import(const QStringList& paths);

  std::shared_ptr<const AssetPreview> preview(const QString& assetId) const;
  QString assetIdAt(int row) const { return row >= 0 && row < count() ? rows_[static_cast<size_t>(row)] : QString(); }
  Q_INVOKABLE QString idAt(int row) const { return assetIdAt(row); }

  // Re-checks every file: a vanished original becomes missing, a returned one analysed again.
  Q_INVOKABLE void refreshAvailability();
  // Starts previews for assets that don't have them (after a project was opened) and flags missing files.
  void projectLoaded();
  // Removes the asset; with removeClips the timeline items using it go first, as one undo group.
  Q_INVOKABLE bool removeAsset(const QString& assetId, bool removeClips);
  Q_INVOKABLE int usesOf(const QString& assetId) const;

  // Blocks (pumping the event loop) until no background work is left. For tests and screenshots.
  bool waitForIdle(int timeoutMs = 30000);

signals:
  void countChanged();
  void pendingChanged();
  void filterChanged();
  void previewReady(const QString& assetId);
  void imported(const sf::editor::ImportSummary& summary);

private:
  struct Probed;
  void rebuild();
  void onAssetsChanged();
  void startProbe(const QString& assetId, const QString& path, AssetKind hint);
  void startPreviews(const QString& assetId, const QString& path, AssetKind kind);
  void probeDone(const QString& assetId, const std::shared_ptr<Probed>& result);
  void previewsDone(const QString& assetId, const std::shared_ptr<AssetPreview>& preview, bool ok);
  void bump(int delta);
  bool matches(const Asset& a) const;

  Project& project_;
  QThreadPool threads_;
  std::shared_ptr<std::atomic<bool>> cancel_;
  std::vector<QString> rows_; // visible asset ids
  QString filterText_;
  QString filterKind_;
  int pending_ = 0;
  mutable QMutex mutex_; // guards previews_ and revisions_, read by the image provider's thread
  std::map<QString, std::shared_ptr<const AssetPreview>> previews_;
  std::map<QString, int> revisions_;
};

} // namespace sf::editor

Q_DECLARE_METATYPE(sf::editor::ImportSummary)
