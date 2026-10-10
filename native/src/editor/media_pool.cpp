#include "editor/media_pool.h"

#include "core/ops.h"
#include "export/proxy.h"
#include "media/probe.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QImageReader>
#include <QSet>
#include <QThread>

#include <algorithm>

namespace sf::editor {

namespace {

constexpr int kStripThumbnails = 12;
constexpr QSize kThumbSize{160, 90};

enum class Hint { Unsupported, Video, Audio, Image };

Hint classify(const QString& path) {
  static const QSet<QString> audio{QStringLiteral("mp3"), QStringLiteral("wav"), QStringLiteral("flac"), QStringLiteral("aac"), QStringLiteral("m4a"),
                                   QStringLiteral("ogg"), QStringLiteral("opus"), QStringLiteral("wma"), QStringLiteral("aif"), QStringLiteral("aiff")};
  static const QSet<QString> video{QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("mkv"), QStringLiteral("webm"), QStringLiteral("avi"),
                                   QStringLiteral("m4v"), QStringLiteral("mts"), QStringLiteral("m2ts"), QStringLiteral("mpg"), QStringLiteral("mpeg"),
                                   QStringLiteral("wmv"), QStringLiteral("flv"), QStringLiteral("ts"), QStringLiteral("3gp"), QStringLiteral("ogv")};
  const QString ext = QFileInfo(path).suffix().toLower();
  if (QImageReader::supportedImageFormats().contains(ext.toUtf8())) return Hint::Image;
  if (audio.contains(ext)) return Hint::Audio;
  if (video.contains(ext)) return Hint::Video;
  return Hint::Unsupported;
}

QString clock(qint64 ms) {
  const qint64 s = (ms + 500) / 1000;
  return s >= 3600 ? QStringLiteral("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'))
                   : QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

// Probe results, filled on a worker and read on the UI thread.
struct MediaPool::Probed {
  bool ok = false;
  QString error;
  MediaInfo info;
  QSize imageSize;
};

MediaPool::MediaPool(Project& project, QObject* parent) : QAbstractListModel(parent), project_(project), cancel_(std::make_shared<std::atomic<bool>>(false)) {
  threads_.setMaxThreadCount(2);
  proxyThreads_.setMaxThreadCount(1);
  proxyCancel_ = std::make_shared<std::atomic<bool>>(false);
  qRegisterMetaType<sf::editor::ImportSummary>();
  connect(&project_, &Project::assetsChanged, this, &MediaPool::onAssetsChanged);
  rebuild();
}

MediaPool::~MediaPool() {
  cancel_->store(true);
  proxyCancel_->store(true);
  threads_.waitForDone();
  proxyThreads_.waitForDone();
}

int MediaPool::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : count(); }

QHash<int, QByteArray> MediaPool::roleNames() const {
  return {{IdRole, "assetId"},     {NameRole, "name"},   {KindRole, "kind"},     {StatusRole, "status"}, {DurationRole, "duration"},
          {DurationMsRole, "durationMs"}, {DetailRole, "detail"}, {PathRole, "path"}, {MissingRole, "missing"}, {ErrorRole, "error"},
          {ThumbRole, "thumb"},   {UsesRole, "uses"},
          {ProxyStateRole, "proxyState"}, {ProxyProgressRole, "proxyProgress"}};
}

QVariant MediaPool::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= count()) return {};
  const QString& id = rows_[static_cast<size_t>(index.row())];
  const Asset* a = project_.asset(id);
  if (!a) return {};
  switch (role) {
  case IdRole: return a->id;
  case NameRole:
  case Qt::DisplayRole: return a->originalName;
  case KindRole: return enumName(a->kind);
  case StatusRole: return enumName(a->status);
  case DurationRole: return a->kind == AssetKind::Image ? QStringLiteral("image") : clock(a->durationMs);
  case DurationMsRole: return static_cast<qlonglong>(a->durationMs);
  case DetailRole: {
    if (a->status == AssetStatus::Importing) return QStringLiteral("Reading…");
    if (a->status == AssetStatus::Failed) return a->error.value_or(QStringLiteral("Unreadable"));
    if (a->status == AssetStatus::Missing) return QStringLiteral("File not found");
    if (a->kind == AssetKind::Audio) return a->metadata.codec.value_or(QStringLiteral("audio"));
    QString d = QStringLiteral("%1×%2").arg(a->width).arg(a->height);
    if (a->fps) d += QStringLiteral(" · %1 fps").arg(*a->fps, 0, 'g', 4);
    return d;
  }
  case PathRole: return a->path;
  case MissingRole: return a->status == AssetStatus::Missing;
  case ErrorRole: return a->error.value_or(QString());
  case ThumbRole: {
    const QMutexLocker lock(&mutex_);
    const auto it = previews_.find(id);
    if (it == previews_.end() || it->second->poster.isNull()) return QString();
    return QStringLiteral("image://pool/%1?%2").arg(id).arg(revisions_.at(id));
  }
  case UsesRole: return usesOf(id);
  case ProxyStateRole: return proxyState(id);
  case ProxyProgressRole: return proxyProgress(id);
  default: return {};
  }
}

bool MediaPool::matches(const Asset& a) const {
  if (!filterKind_.isEmpty() && filterKind_ != QLatin1String("all") && enumName(a.kind) != filterKind_) return false;
  return filterText_.isEmpty() || a.originalName.contains(filterText_, Qt::CaseInsensitive);
}

void MediaPool::rebuild() {
  std::vector<QString> next;
  for (const Asset& a : project_.assets()) {
    if (matches(a)) next.push_back(a.id);
  }
  if (next == rows_) {
    if (!rows_.empty()) emit dataChanged(index(0), index(count() - 1));
    return;
  }
  const int before = count();
  beginResetModel();
  rows_ = std::move(next);
  endResetModel();
  if (before != count()) emit countChanged();
}

void MediaPool::onAssetsChanged() { rebuild(); }

void MediaPool::setFilterText(const QString& t) {
  if (t == filterText_) return;
  filterText_ = t;
  rebuild();
  emit filterChanged();
}

void MediaPool::setFilterKind(const QString& k) {
  if (k == filterKind_) return;
  filterKind_ = k;
  rebuild();
  emit filterChanged();
}

void MediaPool::bump(int delta) {
  pending_ += delta;
  emit pendingChanged();
}

int MediaPool::usesOf(const QString& assetId) const {
  int n = 0;
  for (const Item& i : project_.doc().items) {
    if (i.assetId == assetId) ++n;
  }
  return n;
}

std::shared_ptr<const AssetPreview> MediaPool::preview(const QString& assetId) const {
  const QMutexLocker lock(&mutex_);
  const auto it = previews_.find(assetId);
  return it == previews_.end() ? nullptr : it->second;
}

// ---------- import ----------

ImportSummary MediaPool::import(const QStringList& paths) {
  ImportSummary summary;
  for (const QString& raw : paths) {
    const QFileInfo fi(raw);
    const QString path = fi.absoluteFilePath();
    const Hint hint = classify(path);
    if (!fi.isFile() || hint == Hint::Unsupported) {
      summary.skipped << fi.fileName();
      continue;
    }
    const auto same = std::find_if(project_.assets().begin(), project_.assets().end(),
                                   [&](const Asset& a) { return QFileInfo(a.path) == fi; });
    if (same != project_.assets().end()) {
      summary.duplicates << same->id;
      continue;
    }
    Asset a;
    a.id = newId(QStringLiteral("ast"));
    a.projectId = project_.doc().project.id;
    a.kind = hint == Hint::Audio ? AssetKind::Audio : hint == Hint::Image ? AssetKind::Image : AssetKind::Video;
    a.path = path;
    a.originalName = fi.fileName();
    a.status = AssetStatus::Importing;
    a.sizeBytes = fi.size();
    a.mtimeMs = static_cast<double>(fi.lastModified().toMSecsSinceEpoch());
    a.createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    project_.addAsset(a);
    summary.added << a.id;
    startProbe(a.id, path, a.kind);
  }
  emit imported(summary);
  return summary;
}

void MediaPool::startProbe(const QString& assetId, const QString& path, AssetKind hint) {
  bump(1);
  const auto cancel = cancel_;
  threads_.start([this, assetId, path, hint, cancel] {
    auto out = std::make_shared<Probed>();
    if (!cancel->load()) {
      if (hint == AssetKind::Image) {
        QImageReader r(path);
        r.setAutoTransform(true);
        out->imageSize = r.size();
        out->ok = out->imageSize.isValid();
        if (!out->ok) out->error = r.errorString().isEmpty() ? QStringLiteral("Not a readable image") : r.errorString();
      } else {
        QString err;
        // the VFR scan is not needed to list a clip: a short one keeps import snappy on long files
        if (const auto info = probeMedia(path, &err, ProbeOptions{.vfrScanLimit = 200})) {
          out->info = *info;
          out->ok = info->hasVideo || info->hasAudio;
          if (!out->ok) out->error = QStringLiteral("No audio or video streams");
        } else {
          out->error = err.isEmpty() ? QStringLiteral("Can't open the file") : err;
        }
      }
    }
    QMetaObject::invokeMethod(this, [this, assetId, out] { probeDone(assetId, out); }, Qt::QueuedConnection);
  });
}

void MediaPool::probeDone(const QString& assetId, const std::shared_ptr<Probed>& r) {
  bump(-1);
  const Asset* current = project_.asset(assetId);
  if (!current) return; // removed while probing
  Asset a = *current;
  if (!r->ok) {
    a.status = AssetStatus::Failed;
    a.error = r->error;
    project_.updateAsset(a);
    return;
  }
  a.error.reset();
  if (a.kind == AssetKind::Image) {
    a.width = r->imageSize.width();
    a.height = r->imageSize.height();
  } else {
    const MediaInfo& info = r->info;
    // a "video" file with no picture is audio (and an "audio" file with cover art stays audio)
    if (a.kind == AssetKind::Video && !info.hasVideo) a.kind = AssetKind::Audio;
    a.durationMs = info.durationMs;
    a.hasAudio = info.hasAudio;
    a.width = info.displayWidth > 0 ? info.displayWidth : info.width;
    a.height = info.displayHeight > 0 ? info.displayHeight : info.height;
    const double rate = info.avgFps > 0 ? info.avgFps : info.fps;
    if (a.kind == AssetKind::Video && rate > 0) a.fps = rate;
    a.metadata.codec = a.kind == AssetKind::Audio ? info.audioCodec : info.videoCodec;
  }
  a.status = AssetStatus::Processing;
  project_.updateAsset(a);
  startPreviews(a.id, a.path, a.kind);
}

void MediaPool::startPreviews(const QString& assetId, const QString& path, AssetKind kind) {
  bump(1);
  const auto cancel = cancel_;
  threads_.start([this, assetId, path, kind, cancel] {
    auto out = std::make_shared<AssetPreview>();
    bool ok = false;
    const auto stop = [cancel] { return cancel->load(); };
    if (kind == AssetKind::Image) {
      QImageReader r(path);
      r.setAutoTransform(true);
      const QSize s = r.size();
      if (s.isValid()) {
        r.setScaledSize(s.scaled(kThumbSize, Qt::KeepAspectRatio));
        Thumbnail t;
        t.image = r.read().convertToFormat(QImage::Format_RGBA8888);
        if (!t.image.isNull()) {
          out->strip.push_back(std::move(t));
          ok = true;
        }
      }
    } else if (kind == AssetKind::Video) {
      out->strip = extractThumbnailStrip(path, kStripThumbnails, kThumbSize, stop);
      ok = !out->strip.empty();
    } else {
      out->peaks = extractWaveformPeaks(path, 480, stop);
      ok = out->peaks.bucketCount() > 0;
      if (ok) out->durationSec = static_cast<double>(out->peaks.totalSamples) / out->peaks.sampleRate;
    }
    if (!out->strip.empty()) out->poster = out->strip.front().image;
    if (cancel->load()) ok = false;
    QMetaObject::invokeMethod(this, [this, assetId, out, ok] { previewsDone(assetId, out, ok); }, Qt::QueuedConnection);
  });
}

void MediaPool::previewsDone(const QString& assetId, const std::shared_ptr<AssetPreview>& preview, bool ok) {
  bump(-1);
  const Asset* current = project_.asset(assetId);
  if (!current) return;
  if (ok) {
    const QMutexLocker lock(&mutex_);
    previews_[assetId] = preview;
    revisions_[assetId] += 1;
  }
  if (current->status == AssetStatus::Processing) {
    Asset a = *current;
    a.status = AssetStatus::Analyzed;
    project_.updateAsset(a);
    if (proxiesEnabled_) requestProxy(assetId);
  }
  if (ok) {
    const auto it = std::find(rows_.begin(), rows_.end(), assetId);
    if (it != rows_.end()) {
      const QModelIndex i = index(static_cast<int>(it - rows_.begin()));
      emit dataChanged(i, i);
    }
    emit previewReady(assetId);
  }
}

// ---------- availability, removal ----------

void MediaPool::refreshAvailability() {
  refreshProxyStates();
  for (const Asset& a : std::vector<Asset>(project_.assets())) {
    const bool exists = QFileInfo::exists(a.path);
    if (!exists && (a.status == AssetStatus::Analyzed || a.status == AssetStatus::Processing)) {
      Asset m = a;
      m.status = AssetStatus::Missing;
      project_.updateAsset(m);
    } else if (exists && a.status == AssetStatus::Missing) {
      Asset back = a;
      back.status = AssetStatus::Importing;
      project_.updateAsset(back);
      startProbe(back.id, back.path, back.kind); // the file may have been replaced: look again
    }
  }
}

void MediaPool::projectLoaded() {
  {
    const QMutexLocker lock(&mutex_);
    previews_.clear();
  }
  // jobs of the previous project stop; their results are dropped (they carry the old flag)
  proxyCancel_->store(true);
  proxyCancel_ = std::make_shared<std::atomic<bool>>(false);
  proxies_.clear();
  proxyPending_ = 0;
  ++proxyGen_;
  refreshProxyStates();
  for (const Asset& a : std::vector<Asset>(project_.assets())) {
    if (!QFileInfo::exists(a.path)) {
      if (a.status != AssetStatus::Missing) {
        Asset m = a;
        m.status = AssetStatus::Missing;
        project_.updateAsset(m);
      }
    } else if (a.status == AssetStatus::Analyzed || a.status == AssetStatus::Missing) {
      startPreviews(a.id, a.path, a.kind);
    } else {
      startProbe(a.id, a.path, a.kind); // importing/processing/failed when it was saved: start over
    }
  }
}

bool MediaPool::removeAsset(const QString& assetId, bool removeClips) {
  std::vector<QString> ids;
  for (const Item& i : project_.doc().items) {
    if (i.assetId == assetId) ids.push_back(i.id);
  }
  if (!ids.empty()) {
    if (!removeClips) return false;
    if (!project_.apply(ItemRemove{ids, false}, QStringLiteral("Remove media from timeline"))) return false;
  }
  if (!project_.removeAsset(assetId)) return false;
  proxies_.erase(assetId);
  const QMutexLocker lock(&mutex_);
  previews_.erase(assetId);
  return true;
}

// ---------- proxies ----------

QString MediaPool::proxyDir() const { return proxyDirOverride_.isEmpty() ? xport::proxyCacheDir(project_.path()) : proxyDirOverride_; }

QString MediaPool::proxyState(const QString& assetId) const {
  const auto it = proxies_.find(assetId);
  return it == proxies_.end() ? QStringLiteral("none") : it->second.state;
}

double MediaPool::proxyProgress(const QString& assetId) const {
  const auto it = proxies_.find(assetId);
  return it == proxies_.end() ? 0.0 : it->second.progress;
}

QString MediaPool::proxyPath(const QString& assetId) const {
  if (proxyState(assetId) != QLatin1String("ready")) return {};
  const Asset* a = project_.asset(assetId);
  return a ? xport::existingProxy(a->path, proxyDir()) : QString();
}

void MediaPool::notifyProxyRow(const QString& assetId) {
  const auto it = std::find(rows_.begin(), rows_.end(), assetId);
  if (it != rows_.end()) {
    const QModelIndex i = index(static_cast<int>(it - rows_.begin()));
    emit dataChanged(i, i, {ProxyStateRole, ProxyProgressRole});
  }
  emit proxiesChanged();
}

void MediaPool::refreshProxyStates() {
  const QString dir = proxyDir();
  for (const Asset& a : project_.assets()) {
    if (a.kind != AssetKind::Video) continue;
    ProxyInfo& info = proxies_[a.id];
    if (info.state == QLatin1String("queued") || info.state == QLatin1String("running")) continue;
    const QString before = info.state;
    // a proxy is keyed by the source's path, size and mtime: a replaced source has no ready proxy any more
    // (and "unneeded" is re-evaluated, the new file may be bigger)
    info.state = xport::existingProxy(a.path, dir).isEmpty() ? QStringLiteral("none") : QStringLiteral("ready");
    if (info.state != before) notifyProxyRow(a.id);
  }
  if (proxiesEnabled_) requestAllProxies();
}

void MediaPool::setProxiesEnabled(bool on) {
  if (on == proxiesEnabled_) return;
  proxiesEnabled_ = on;
  if (on) requestAllProxies();
  emit proxiesChanged();
}

void MediaPool::requestAllProxies() {
  for (const Asset& a : std::vector<Asset>(project_.assets())) {
    if (a.kind == AssetKind::Video) requestProxy(a.id);
  }
}

void MediaPool::requestProxy(const QString& assetId) {
  const Asset* a = project_.asset(assetId);
  if (!a || a->kind != AssetKind::Video || a->status != AssetStatus::Analyzed) return;
  ProxyInfo& info = proxies_[assetId];
  if (info.state == QLatin1String("queued") || info.state == QLatin1String("running")) return;
  const QString dir = proxyDir();
  if (!xport::existingProxy(a->path, dir).isEmpty()) {
    if (info.state != QLatin1String("ready")) {
      info.state = QStringLiteral("ready");
      notifyProxyRow(assetId);
    }
    return;
  }
  info.state = QStringLiteral("queued");
  info.progress = 0;
  info.error.clear();
  ++proxyPending_;
  notifyProxyRow(assetId);
  const auto cancel = proxyCancel_;
  const int gen = proxyGen_;
  const QString path = a->path;
  const xport::ProxyOptions options = proxyOptions_;
  proxyThreads_.start([this, assetId, path, dir, options, cancel, gen] {
    xport::ProxyResult result;
    if (cancel->load()) {
      result.cancelled = true;
    } else {
      auto last = std::make_shared<std::atomic<qint64>>(0);
      result = xport::makeProxy(path, dir, options,
                                [this, assetId, last](double f) {
                                  const qint64 now = QDateTime::currentMSecsSinceEpoch();
                                  if (now - last->load() < 200 && f < 1.0) return;
                                  last->store(now);
                                  QMetaObject::invokeMethod(this, [this, assetId, f] { proxyProgressed(assetId, f); }, Qt::QueuedConnection);
                                },
                                cancel.get());
    }
    QMetaObject::invokeMethod(this, [this, assetId, result, gen] {
      if (gen == proxyGen_) proxyDone(assetId, result); // else: a project that is gone
    }, Qt::QueuedConnection);
  });
}

void MediaPool::proxyProgressed(const QString& assetId, double fraction) {
  const auto it = proxies_.find(assetId);
  if (it == proxies_.end() || (it->second.state != QLatin1String("queued") && it->second.state != QLatin1String("running"))) return;
  it->second.state = QStringLiteral("running");
  it->second.progress = fraction;
  notifyProxyRow(assetId);
}

void MediaPool::proxyDone(const QString& assetId, const xport::ProxyResult& r) {
  proxyPending_ = std::max(0, proxyPending_ - 1);
  const auto it = proxies_.find(assetId);
  if (it == proxies_.end()) {
    emit proxiesChanged();
    return;
  }
  if (r.cancelled) it->second.state = QStringLiteral("none");
  else if (!r.ok) it->second.state = QStringLiteral("failed");
  else it->second.state = r.unneeded ? QStringLiteral("unneeded") : QStringLiteral("ready");
  it->second.progress = r.ok ? 1.0 : 0.0;
  it->second.error = r.error;
  notifyProxyRow(assetId);
}

void MediaPool::cancelProxies() {
  // queued jobs never start (they see the old flag set); the running one stops at its next frame; each reports "cancelled"
  proxyCancel_->store(true);
  proxyCancel_ = std::make_shared<std::atomic<bool>>(false);
}

bool MediaPool::waitForProxies(int timeoutMs) {
  QElapsedTimer t;
  t.start();
  while (proxyPending_ > 0 && t.elapsed() < timeoutMs) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  QCoreApplication::processEvents();
  return proxyPending_ == 0;
}

bool MediaPool::waitForIdle(int timeoutMs) {
  QElapsedTimer t;
  t.start();
  while (pending_ > 0 && t.elapsed() < timeoutMs) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  QCoreApplication::processEvents();
  return pending_ == 0;
}

} // namespace sf::editor
