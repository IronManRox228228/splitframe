#include "editor/project.h"

#include "core/apply.h"
#include "core/error.h"
#include "core/timeline_doc.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

namespace sf::editor {

namespace {

constexpr int kDefaultAutosaveMs = 20000;

ProjectBundle readBundle(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) throw std::runtime_error(QStringLiteral("Can't read %1: %2").arg(path, f.errorString()).toStdString());
  const QByteArray json = f.readAll();
  const QJsonDocument parsed = QJsonDocument::fromJson(json);
  if (!parsed.isObject()) throw std::runtime_error("not a project file (expected a JSON object)");
  if (parsed.object().contains(QStringLiteral("doc"))) return parseProjectBundleJson(json);
  ProjectBundle bundle; // a bare TimelineDoc: no asset table
  bundle.doc = parseTimelineDocJson(json);
  return bundle;
}

void resolveAssetPaths(ProjectBundle& bundle, const QString& projectPath) {
  if (projectPath.isEmpty()) return;
  const QDir base = QFileInfo(projectPath).absoluteDir();
  for (Asset& a : bundle.assets) {
    // the Electron app writes absolute paths; a relative one is relative to the project file
    if (!a.path.isEmpty() && !QFileInfo(a.path).isAbsolute()) a.path = QDir::cleanPath(base.absoluteFilePath(a.path));
  }
}

QString nowIso() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }

} // namespace

Project::Project(QObject* parent) : QObject(parent) {
  autosaveTimer_.setSingleShot(true);
  autosaveTimer_.setInterval(kDefaultAutosaveMs);
  connect(&autosaveTimer_, &QTimer::timeout, this, [this] { autosaveNow(); });
  newProject(QStringLiteral("Untitled video"));
}

const Asset* Project::asset(const QString& id) const {
  const auto it = std::find_if(bundle_.assets.begin(), bundle_.assets.end(), [&](const Asset& a) { return a.id == id; });
  return it == bundle_.assets.end() ? nullptr : &*it;
}

// ---------- editing ----------

bool Project::apply(const std::vector<Op>& ops, const QString& label) {
  if (ops.empty()) return true;
  try {
    ApplyResult r = applyOps(bundle_.doc, ops);
    bundle_.doc = std::move(r.doc);
    history_.push(ops, r.inverse, label.isEmpty() ? std::nullopt : std::optional<QString>(label), QStringLiteral("user"));
  } catch (const std::exception& e) {
    lastError_ = QString::fromUtf8(e.what());
    emit editRejected(lastError_);
    return false;
  }
  lastError_.clear();
  changed(true);
  emit opsApplied(ops, label);
  return true;
}

void Project::beginGroup(const QString& label) {
  history_.beginGroup(label.isEmpty() ? std::nullopt : std::optional<QString>(label), QStringLiteral("user"));
  groupOpen_ = true;
}

void Project::endGroup() {
  if (!groupOpen_) return;
  history_.commitGroup();
  groupOpen_ = false;
  emit historyChanged();
}

bool Project::undo() {
  groupOpen_ = false; // History::undo closes an open group itself
  try {
    // recorded inverses replay without lock checks: a track locked after the edit can still be undone
    const auto r = history_.undoWith([&](const UndoGroup& g) { return applyOps(bundle_.doc, g.inverses, {.enforceLocks = false}); });
    if (!r) return false;
    bundle_.doc = r->doc;
  } catch (const std::exception& e) {
    lastError_ = QString::fromUtf8(e.what());
    emit editRejected(lastError_);
    return false;
  }
  changed(true);
  return true;
}

bool Project::redo() {
  try {
    const auto r = history_.redoWith([&](const UndoGroup& g) { return applyOps(bundle_.doc, g.ops, {.enforceLocks = false}); });
    if (!r) return false;
    bundle_.doc = r->doc;
  } catch (const std::exception& e) {
    lastError_ = QString::fromUtf8(e.what());
    emit editRejected(lastError_);
    return false;
  }
  changed(true);
  return true;
}

// ---------- selection ----------

void Project::setSelection(const QStringList& ids) {
  if (ids == selection_) return;
  selection_ = ids;
  emit selectionChanged();
}

void Project::select(const QString& id, bool additive) {
  if (!additive) {
    setSelection({id});
    return;
  }
  QStringList next = selection_;
  if (!next.removeOne(id)) next.push_back(id);
  setSelection(next);
}

void Project::pruneSelection() {
  QStringList next;
  for (const QString& id : selection_) {
    if (getItem(bundle_.doc, id)) next.push_back(id);
  }
  setSelection(next);
}

// ---------- assets ----------

void Project::addAsset(const Asset& asset) {
  bundle_.assets.push_back(asset);
  emit assetsChanged();
  refreshDirty();
}

void Project::updateAsset(const Asset& asset) {
  const auto it = std::find_if(bundle_.assets.begin(), bundle_.assets.end(), [&](const Asset& a) { return a.id == asset.id; });
  if (it == bundle_.assets.end() || *it == asset) return;
  *it = asset;
  emit assetsChanged();
  refreshDirty();
}

bool Project::removeAsset(const QString& id) {
  for (const Item& i : bundle_.doc.items) {
    if (i.assetId == id) return false;
  }
  const auto n = std::erase_if(bundle_.assets, [&](const Asset& a) { return a.id == id; });
  if (n == 0) return false;
  history_.dropReferences(id);
  emit assetsChanged();
  refreshDirty();
  return true;
}

// ---------- lifecycle ----------

void Project::load(ProjectBundle bundle, const QString& path, bool dirty) {
  endGroup();
  bundle_ = std::move(bundle);
  saved_ = bundle_;
  forceDirty_ = dirty;
  history_.clear();
  selection_.clear();
  const bool pathDiffers = path_ != path;
  path_ = path;
  dirty_ = false;
  autosaveTimer_.stop();
  refreshDirty();
  if (pathDiffers) emit pathChanged();
  emit selectionChanged();
  emit assetsChanged();
  emit docChanged();
  emit historyChanged();
  emit loaded();
}

void Project::newProject(const QString& name, int width, int height, int fps) {
  ProjectBundle b;
  b.doc = createEmptyDoc({.id = newId(QStringLiteral("prj")), .name = name, .fps = fps, .width = width, .height = height});
  load(std::move(b), QString(), false);
}

bool Project::open(const QString& path, QString* error) {
  try {
    ProjectBundle b = readBundle(path);
    resolveAssetPaths(b, path);
    load(std::move(b), QFileInfo(path).absoluteFilePath(), false);
    return true;
  } catch (const std::exception& e) {
    if (error) *error = QStringLiteral("%1: %2").arg(path, QString::fromUtf8(e.what()));
    return false;
  }
}

bool Project::writeTo(const QString& path, QString* error) {
  try {
    bundle_.doc.project.updatedAt = nowIso();
    saveProjectBundle(path, bundle_, true);
    return true;
  } catch (const std::exception& e) {
    if (error) *error = QStringLiteral("Can't save %1: %2").arg(path, QString::fromUtf8(e.what()));
    return false;
  }
}

bool Project::save(QString* error) {
  if (path_.isEmpty()) {
    if (error) *error = QStringLiteral("The project has no file yet");
    return false;
  }
  return saveAs(path_, error);
}

bool Project::saveAs(const QString& path, QString* error) {
  const QString abs = QFileInfo(path).absoluteFilePath();
  if (!writeTo(abs, error)) return false;
  clearOwnRecovery();
  saved_ = bundle_;
  forceDirty_ = false;
  const bool pathDiffers = path_ != abs;
  path_ = abs;
  refreshDirty();
  if (pathDiffers) emit pathChanged();
  return true;
}

QString Project::displayName() const {
  const QString n = bundle_.doc.project.name.isEmpty() ? QFileInfo(path_).completeBaseName() : bundle_.doc.project.name;
  return dirty_ ? n + QStringLiteral(" *") : n;
}

void Project::changed(bool docTouched) {
  if (docTouched) {
    pruneSelection();
    emit docChanged();
  }
  refreshDirty();
  emit historyChanged();
}

void Project::refreshDirty() {
  const bool now = forceDirty_ || !(bundle_ == saved_);
  if (now != dirty_) {
    dirty_ = now;
    if (!dirty_) autosaveTimer_.stop();
    emit dirtyChanged();
  }
  if (dirty_ && autosaveTimer_.interval() > 0 && !autosaveTimer_.isActive()) autosaveTimer_.start();
}

// ---------- autosave and recovery ----------

void Project::setAutosaveInterval(int ms) {
  autosaveTimer_.stop();
  autosaveTimer_.setInterval(std::max(0, ms));
  if (ms > 0 && dirty_) autosaveTimer_.start();
}

QString Project::recoveryPathFor(const QString& projectId) const { return QDir(recoveryDir_).absoluteFilePath(projectId + QStringLiteral(".autosave.json")); }

bool Project::autosaveNow() {
  if (recoveryDir_.isEmpty() || !dirty_) return false;
  if (!QDir().mkpath(recoveryDir_)) return false;
  try {
    const QString file = recoveryPathFor(bundle_.doc.project.id);
    saveProjectBundle(file, bundle_, false);
    QJsonObject meta;
    meta.insert(QStringLiteral("path"), path_);
    meta.insert(QStringLiteral("name"), bundle_.doc.project.name);
    QSaveFile m(file + QStringLiteral(".meta"));
    if (!m.open(QIODevice::WriteOnly)) return false;
    m.write(QJsonDocument(meta).toJson(QJsonDocument::Compact));
    return m.commit();
  } catch (const std::exception&) {
    return false;
  }
}

void Project::clearOwnRecovery() {
  if (recoveryDir_.isEmpty()) return;
  const QString file = recoveryPathFor(bundle_.doc.project.id);
  QFile::remove(file);
  QFile::remove(file + QStringLiteral(".meta"));
}

QList<RecoveryEntry> Project::recoveries() const {
  QList<RecoveryEntry> out;
  if (recoveryDir_.isEmpty()) return out;
  const QFileInfoList files = QDir(recoveryDir_).entryInfoList({QStringLiteral("*.autosave.json")}, QDir::Files, QDir::Time);
  for (const QFileInfo& fi : files) {
    RecoveryEntry e;
    e.autosavePath = fi.absoluteFilePath();
    e.modified = fi.lastModified();
    QFile m(e.autosavePath + QStringLiteral(".meta"));
    if (m.open(QIODevice::ReadOnly)) {
      const QJsonObject meta = QJsonDocument::fromJson(m.readAll()).object();
      e.originalPath = meta.value(QStringLiteral("path")).toString();
      e.name = meta.value(QStringLiteral("name")).toString();
    }
    if (e.name.isEmpty()) e.name = fi.completeBaseName();
    out.push_back(e);
  }
  return out;
}

std::optional<RecoveryEntry> Project::recoveryFor(const QString& path) const {
  const QFileInfo target(path);
  for (const RecoveryEntry& e : recoveries()) {
    if (e.originalPath.isEmpty()) continue;
    if (QFileInfo(e.originalPath) == target && (!target.exists() || e.modified > target.lastModified())) return e;
  }
  return std::nullopt;
}

bool Project::openRecovery(const RecoveryEntry& entry, QString* error) {
  try {
    ProjectBundle b = readBundle(entry.autosavePath);
    resolveAssetPaths(b, entry.originalPath);
    load(std::move(b), entry.originalPath, true);
    return true;
  } catch (const std::exception& e) {
    if (error) *error = QStringLiteral("%1: %2").arg(entry.autosavePath, QString::fromUtf8(e.what()));
    return false;
  }
}

void Project::discardRecovery(const RecoveryEntry& entry) const {
  QFile::remove(entry.autosavePath);
  QFile::remove(entry.autosavePath + QStringLiteral(".meta"));
}

} // namespace sf::editor
