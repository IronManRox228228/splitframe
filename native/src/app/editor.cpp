#include "app/editor.h"

#include "core/timeline_doc.h"
#include "media/probe.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QStandardPaths>

#include <algorithm>

namespace sf::app {

using namespace sf::editor;
using editor::Project; // sf::Project (the schema struct) would be ambiguous

namespace {

QString defaultRecoveryDir() { return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).absoluteFilePath(QStringLiteral("recovery")); }

QString names(const QStringList& list) { return list.size() <= 2 ? list.join(QStringLiteral(", ")) : QStringLiteral("%1 +%2 more").arg(list.mid(0, 2).join(QStringLiteral(", "))).arg(list.size() - 2); }

} // namespace

Editor::Editor(Player& player, const Options& options, QObject* parent)
    : QObject(parent),
      player_(player),
      pool_(project_),
      timeline_(project_),
      tracks_(project_),
      controller_(project_),
      inspector_(project_),
      recent_(options.settingsFile),
      export_(project_) {
  project_.setRecoveryDir(options.recoveryDir.isEmpty() ? defaultRecoveryDir() : options.recoveryDir);
  project_.setAutosaveInterval(options.autosaveMs);
  controller_.setSnapEnabled(recent_.value(QStringLiteral("snap"), true).toBool());
  controller_.setRippleEnabled(recent_.value(QStringLiteral("ripple"), false).toBool());

  useProxies_ = recent_.value(QStringLiteral("useProxies"), false).toBool();
  player_.setUseProxies(useProxies_);
  pool_.setProxiesEnabled(useProxies_);
  // a proxy finishing (or going stale) changes which file preview decodes
  connect(&pool_, &MediaPool::proxiesChanged, this, [this] {
    if (!useProxies_) return;
    QStringList key; // progress ticks come through here too: only a different set of proxy files matters
    for (const Asset& a : project_.assets()) key << pool_.proxyPath(a.id);
    const QString joined = key.join(QLatin1Char('|'));
    if (joined == proxyKey_) return;
    proxyKey_ = joined;
    pushAssets();
  });

  connect(&controller_, &TimelineController::optionsChanged, this, [this] {
    recent_.setValue(QStringLiteral("snap"), controller_.snapEnabled());
    recent_.setValue(QStringLiteral("ripple"), controller_.rippleEnabled());
  });
  connect(&project_, &Project::loaded, this, &Editor::onLoaded);
  connect(&project_, &Project::docChanged, this, &Editor::pushDocument);
  connect(&project_, &Project::assetsChanged, this, &Editor::pushAssets);
  connect(&project_, &Project::historyChanged, this, &Editor::historyChanged);
  connect(&project_, &Project::selectionChanged, this, &Editor::selectionChanged);
  connect(&project_, &Project::dirtyChanged, this, &Editor::titleChanged);
  connect(&project_, &Project::pathChanged, this, &Editor::titleChanged);
  connect(&project_, &Project::editRejected, this, [this](const QString& m) { emit toastRequested(m, QStringLiteral("error")); });
  connect(&controller_, &TimelineController::message, this, &Editor::toastRequested);
  connect(&player_, &Player::transportChanged, this, [this] { controller_.setPlayhead(player_.frame()); });
  connect(&pool_, &MediaPool::imported, this, [this](const ImportSummary& s) {
    QStringList parts;
    if (!s.added.isEmpty()) parts << QStringLiteral("Importing %1 file%2").arg(s.added.size()).arg(s.added.size() == 1 ? QString() : QStringLiteral("s"));
    if (!s.duplicates.isEmpty()) parts << QStringLiteral("%1 already in the project").arg(s.duplicates.size());
    if (!s.skipped.isEmpty()) parts << QStringLiteral("%1 unsupported (%2)").arg(s.skipped.size()).arg(names(s.skipped));
    if (!parts.isEmpty()) emit toastRequested(parts.join(QStringLiteral(" · ")), s.added.isEmpty() && s.duplicates.isEmpty() ? QStringLiteral("error") : QStringLiteral("info"));
  });
  onLoaded();
}

QString Editor::windowTitle() const { return project_.displayName() + QStringLiteral(" - SplitFrame"); }

QString Editor::toLocalPath(const QString& pathOrUrl) {
  if (pathOrUrl.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) return QUrl(pathOrUrl).toLocalFile();
  return pathOrUrl;
}

render::AssetTable Editor::assetTable() const {
  render::AssetTable t;
  for (const Asset& a : project_.assets()) {
    if (a.status == AssetStatus::Failed) continue;
    t[a.id] = {a.path, a.kind, pool_.proxyPath(a.id)};
  }
  return t;
}

void Editor::onLoaded() {
  player_.loadProject(project_.doc(), assetTable());
  pool_.projectLoaded();
  controller_.setPlayhead(0);
  rememberRecent();
  emit titleChanged();
  emit historyChanged();
  emit selectionChanged();
}

void Editor::pushDocument() {
  player_.setDocument(project_.doc());
  emit titleChanged();
}

void Editor::pushAssets() { player_.setAssets(assetTable()); }

void Editor::setUseProxies(bool on) {
  if (on == useProxies_) return;
  useProxies_ = on;
  recent_.setValue(QStringLiteral("useProxies"), on);
  pool_.setProxiesEnabled(on);
  player_.setUseProxies(on);
  if (on) pushAssets();
  emit useProxiesChanged();
}

void Editor::rememberRecent() {
  if (!project_.path().isEmpty()) recent_.add(project_.path());
}

// ---------- lifecycle ----------

void Editor::newProject(const QString& name, int width, int height, int fps) {
  project_.newProject(name.trimmed().isEmpty() ? QStringLiteral("Untitled video") : name.trimmed(), width, height, fps);
}

bool Editor::wrapMedia(const QString& path) {
  const QFileInfo fi(path);
  if (!fi.isFile()) {
    emit toastRequested(QStringLiteral("No such file: %1").arg(path), QStringLiteral("error"));
    return false;
  }
  Asset a;
  a.id = newId(QStringLiteral("ast"));
  a.path = fi.absoluteFilePath();
  a.originalName = fi.fileName();
  a.status = AssetStatus::Analyzed;
  a.sizeBytes = fi.size();
  a.mtimeMs = static_cast<double>(fi.lastModified().toMSecsSinceEpoch());
  a.createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  int width = 1920, height = 1080, fps = 30;
  if (QImageReader::supportedImageFormats().contains(fi.suffix().toLower().toUtf8())) {
    QImageReader r(a.path);
    r.setAutoTransform(true);
    if (r.size().isValid()) {
      width = r.size().width();
      height = r.size().height();
    }
    a.kind = AssetKind::Image;
    a.width = width;
    a.height = height;
  } else {
    QString err;
    const auto info = probeMedia(a.path, &err);
    if (!info || !(info->hasVideo || info->hasAudio)) {
      emit toastRequested(info ? QStringLiteral("%1 has no audio or video").arg(path) : err, QStringLiteral("error"));
      return false;
    }
    a.kind = info->hasVideo ? AssetKind::Video : AssetKind::Audio;
    a.durationMs = info->durationMs;
    a.hasAudio = info->hasAudio;
    a.metadata.codec = info->hasVideo ? info->videoCodec : info->audioCodec;
    if (info->hasVideo) {
      const double rate = info->avgFps > 0 ? info->avgFps : (info->fps > 0 ? info->fps : 30.0);
      fps = static_cast<int>(std::clamp<std::int64_t>(jsRound(rate), 1, 240));
      width = info->displayWidth > 0 ? info->displayWidth : info->width;
      height = info->displayHeight > 0 ? info->displayHeight : info->height;
      a.width = width;
      a.height = height;
      a.fps = rate;
    }
  }
  project_.newProject(fi.completeBaseName(), width, height, fps);
  a.projectId = project_.doc().project.id;
  project_.addAsset(a);
  controller_.addAsset(a.id, 0, -1);
  pool_.projectLoaded(); // thumbnails / waveform for the new asset
  return true;
}

bool Editor::openPath(const QString& pathOrUrl) {
  const QString path = toLocalPath(pathOrUrl);
  if (QFileInfo(path).suffix().compare(QLatin1String("json"), Qt::CaseInsensitive) == 0) {
    QString err;
    if (!project_.open(path, &err)) {
      emit toastRequested(err, QStringLiteral("error"));
      recent_.remove(path);
      return false;
    }
    return true;
  }
  return wrapMedia(path);
}

bool Editor::save() {
  if (project_.path().isEmpty()) {
    emit saveAsRequested();
    return false;
  }
  QString err;
  if (!project_.save(&err)) {
    emit toastRequested(err, QStringLiteral("error"));
    return false;
  }
  rememberRecent();
  emit toastRequested(QStringLiteral("Saved"), QStringLiteral("info"));
  return true;
}

bool Editor::saveAs(const QString& pathOrUrl) {
  QString path = toLocalPath(pathOrUrl);
  if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".json");
  QString err;
  if (!project_.saveAs(path, &err)) {
    emit toastRequested(err, QStringLiteral("error"));
    return false;
  }
  rememberRecent();
  emit toastRequested(QStringLiteral("Saved"), QStringLiteral("info"));
  return true;
}

QVariantList Editor::recoveries() const {
  QVariantList out;
  for (const RecoveryEntry& e : project_.recoveries()) {
    out.push_back(QVariantMap{{QStringLiteral("autosavePath"), e.autosavePath},
                              {QStringLiteral("name"), e.name},
                              {QStringLiteral("originalPath"), e.originalPath},
                              {QStringLiteral("modified"), e.modified.toString(Qt::TextDate)}});
  }
  return out;
}

bool Editor::restoreRecovery(const QString& autosavePath) {
  for (const RecoveryEntry& e : project_.recoveries()) {
    if (e.autosavePath != autosavePath) continue;
    QString err;
    if (project_.openRecovery(e, &err)) return true;
    emit toastRequested(err, QStringLiteral("error"));
    return false;
  }
  return false;
}

void Editor::discardRecovery(const QString& autosavePath) {
  for (const RecoveryEntry& e : project_.recoveries()) {
    if (e.autosavePath == autosavePath) project_.discardRecovery(e);
  }
}

QString Editor::recoveryFor(const QString& path) const {
  const auto e = project_.recoveryFor(toLocalPath(path));
  return e ? e->autosavePath : QString();
}

// ---------- media and editing ----------

void Editor::importFiles(const QVariantList& pathsOrUrls) {
  QStringList paths;
  for (const QVariant& v : pathsOrUrls) paths << toLocalPath(v.toString());
  if (!paths.isEmpty()) pool_.import(paths);
}

void Editor::undo() {
  if (!project_.undo()) emit toastRequested(QStringLiteral("Nothing to undo"), QStringLiteral("info"));
}

void Editor::redo() {
  if (!project_.redo()) emit toastRequested(QStringLiteral("Nothing to redo"), QStringLiteral("info"));
}

void Editor::seek(qint64 frame) { player_.seek(std::max<qint64>(0, frame)); }

void Editor::step(qint64 frames) { player_.step(static_cast<int>(frames)); }

void Editor::gotoEdge(int dir) {
  const qint64 f = controller_.edgeFrom(player_.frame(), dir);
  if (f >= 0) player_.seek(f);
}

void Editor::toast(const QString& text, const QString& kind) { emit toastRequested(text, kind); }

} // namespace sf::app
