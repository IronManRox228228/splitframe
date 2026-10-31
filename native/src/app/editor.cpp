#include "app/editor.h"

#include "agent/llm_config.h"
#include "core/schema.h"
#include "core/timeline_doc.h"
#include "media/probe.h"

#include <QCoreApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSettings>
#include <QTimer>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
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

engine::Engine* Editor::engine() {
  if (!engine_) {
    engine_ = std::make_unique<engine::Engine>();
    engine_->attach(&project_, &pool_);
    ai_ = std::make_unique<ai::AiServices>(*engine_, aiOptions_);
    connect(engine_.get(), &engine::Engine::event, this, [this](const QString& name, const QJsonObject& data, qint64) { onEngineEvent(name, data); });
    // The crew's live stream: the harness's Qt signal becomes the engine event "agent.event" (API clients see it too)
    if (auto* h = ai_->harness())
      connect(h, &agent::AgentHarness::agentEvent, this, [this](const QJsonObject& ev) { engine_->emitEvent(QStringLiteral("agent.event"), ev); });
  }
  return engine_.get();
}

void Editor::setApiEnabled(bool on) {
  if (on == apiEnabled()) return;
  if (!on) {
    api_.reset();
    toast(QStringLiteral("Local API stopped"));
    emit apiChanged();
    return;
  }
  engine::ApiServer::Options o;
  o.port = apiPort_;
  api_ = std::make_unique<engine::ApiServer>(*engine(), o);
  QString err;
  if (!api_->start(&err)) {
    api_.reset();
    toast(QStringLiteral("Local API failed: %1").arg(err), QStringLiteral("error"));
  } else {
    toast(QStringLiteral("Local API on %1").arg(apiStatus()));
  }
  emit apiChanged();
}

QString Editor::apiStatus() const {
  if (!apiEnabled()) return QStringLiteral("off");
  return QStringLiteral("http://127.0.0.1:%1 (token: %2)").arg(api_->port()).arg(QDir::toNativeSeparators(api_->tokenFile()));
}

Editor::Editor(Player& player, const Options& options, QObject* parent)
    : QObject(parent),
      player_(player),
      pool_(project_),
      timeline_(project_),
      tracks_(project_),
      controller_(project_),
      inspector_(project_),
      recent_(options.settingsFile),
      export_(project_),
      mixer_(project_),
      colour_(project_, &pool_, &player) {
  mixer_.setPlayer(&player_);
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
  aiOptions_ = options.ai;
  connect(&project_, &Project::loaded, this, &Editor::onLoaded);
  connect(&project_, &Project::docChanged, this, &Editor::pushDocument);
  connect(&project_, &Project::assetsChanged, this, &Editor::pushAssets);
  connect(&project_, &Project::historyChanged, this, &Editor::historyChanged);
  connect(&project_, &Project::agentLockChanged, this, &Editor::agentBusyChanged);
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

bool Editor::refusedWhileAgent() {
  if (!project_.agentLocked()) return false;
  emit toastRequested(project_.agentBusyMessage(), QStringLiteral("info"));
  return true;
}

void Editor::newProject(const QString& name, int width, int height, int fps) {
  if (refusedWhileAgent()) return;
  project_.newProject(name.trimmed().isEmpty() ? QStringLiteral("Untitled video") : name.trimmed(), width, height, fps);
}

bool Editor::wrapMedia(const QString& path) {
  if (refusedWhileAgent()) return false;
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
  if (refusedWhileAgent()) return false;
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
  if (refusedWhileAgent()) return false;
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
  if (refusedWhileAgent()) return;
  QStringList paths;
  for (const QVariant& v : pathsOrUrls) paths << toLocalPath(v.toString());
  if (!paths.isEmpty()) pool_.import(paths);
}

void Editor::undo() {
  if (refusedWhileAgent()) return;
  if (!project_.undo()) emit toastRequested(QStringLiteral("Nothing to undo"), QStringLiteral("info"));
}

void Editor::redo() {
  if (refusedWhileAgent()) return;
  if (!project_.redo()) emit toastRequested(QStringLiteral("Nothing to redo"), QStringLiteral("info"));
}

void Editor::seek(qint64 frame) { player_.seek(std::max<qint64>(0, frame)); }

void Editor::step(qint64 frames) { player_.step(static_cast<int>(frames)); }

void Editor::gotoEdge(int dir) {
  const qint64 f = controller_.edgeFrom(player_.frame(), dir);
  if (f >= 0) player_.seek(f);
}

void Editor::toast(const QString& text, const QString& kind) { emit toastRequested(text, kind); }

void Editor::copyText(const QString& text) {
  if (QClipboard* cb = QGuiApplication::clipboard()) cb->setText(text);
}

// ---------- the Assistant ----------

void Editor::setAgentMode(const QString& mode) {
  const QString m = mode.trimmed().toLower();
  if (m != QLatin1String("plan") && m != QLatin1String("ask") && m != QLatin1String("default") && m != QLatin1String("auto")) return;
  if (m == agentMode_) return;
  agentMode_ = m;
  emit agentModeChanged();
}

void Editor::setDemoMode(bool on) {
  if (on == demoMode_) return;
  demoMode_ = on;
  emit demoChanged();
}

void Editor::setDemoBlocks(const QVariantList& blocks) {
  demoBlocks_ = blocks;
  emit demoChanged();
}

// Engine events the GUI cares about. agent.event is the crew's live stream; the harness's own Qt signal is forwarded into the engine
// in engine() below, so the API server's SSE stream carries it too. If the harness already forwards it, the duplicate is dropped here.
void Editor::onEngineEvent(const QString& name, const QJsonObject& data) {
  if (name == QLatin1String("agent.event")) {
    const QByteArray key = QJsonDocument(data).toJson(QJsonDocument::Compact);
    if (key == lastAgentEvent_ && lastAgentEventClock_.isValid() && lastAgentEventClock_.elapsed() < 25) return;
    lastAgentEvent_ = key;
    lastAgentEventClock_.restart();
    emit agentEvent(data.toVariantMap());
  } else if (name == QLatin1String("doc.changed")) {
    emit storyboardChanged();
  } else if (name == QLatin1String("jobs.updated")) {
    const QString analyzer = data.value(QStringLiteral("analyzer")).toString();
    if (analyzer.startsWith(QLatin1String("comfy")) || analyzer.startsWith(QLatin1String("generate"))) emit generationJobChanged(data.toVariantMap());
  }
}

QString Editor::agentBusyName() const {
  const QString owner = project_.agentLockOwner();
  return owner.isEmpty() ? QString() : Project::agentDisplayName(owner);
}

void Editor::agentChat(const QString& message) {
  const QString text = message.trimmed();
  if (text.isEmpty() || agentBusy()) return;
  agentBusy_ = true;
  stopRequested_ = false;
  // the lock from this moment, not from when the run opens its undo group: no human edit slips into the gap
  project_.setAgentReservation(QStringLiteral("agent:director"));
  emit agentBusyChanged();
  QTimer::singleShot(0, this, [this, text] { runAgent(text); });
}

// Every end of a run (done, failed, stopped) comes through here: the lock always releases.
void Editor::finishAgentRun(const QVariantMap& out) {
  agentBusy_ = false;
  stopRequested_ = false;
  project_.setAgentReservation({});
  emit agentBusyChanged();
  emit agentTurnFinished(out);
  emit storyboardChanged();
}

void Editor::runAgent(const QString& message) {
  engine::Engine* e = engine();
  QVariantMap out;
  if (stopRequested_) { // Stop was pressed before the run began
    out = QVariantMap{{QStringLiteral("ok"), true}, {QStringLiteral("status"), QStringLiteral("cancelled")}};
    finishAgentRun(out);
    return;
  }
  engine::Result r;
  try {
    e->call(QStringLiteral("agent.mode.set"), QJsonObject{{QStringLiteral("mode"), agentMode_}});
    const bool held = agentMode_ == QLatin1String("plan") || agentMode_ == QLatin1String("ask");
    r = e->call(held ? QStringLiteral("agent.turn") : QStringLiteral("agent.chat"),
                QJsonObject{{QStringLiteral("message"), message}, {QStringLiteral("origin"), QStringLiteral("agent:director")}});
  } catch (...) {
    r = engine::Result::failure(QStringLiteral("internal"), QStringLiteral("The assistant stopped unexpectedly."));
  }
  if (r.ok) {
    out = r.value.toObject().toVariantMap();
  } else {
    out.insert(QStringLiteral("ok"), false);
    out.insert(QStringLiteral("error"), r.error.code == QLatin1String("unavailable") ? QStringLiteral("provider_unreachable") : r.error.code);
    out.insert(QStringLiteral("errorMessage"), r.error.message);
  }
  finishAgentRun(out);
}

void Editor::agentDecide(bool apply) {
  if (agentBusy()) return;
  agentBusy_ = true;
  stopRequested_ = false;
  project_.setAgentReservation(QStringLiteral("agent:director"));
  emit agentBusyChanged();
  QTimer::singleShot(0, this, [this, apply] {
    engine::Result r;
    try {
      r = engine()->call(apply ? QStringLiteral("agent.plan.run") : QStringLiteral("agent.plan.skip"),
                         apply ? QJsonObject{{QStringLiteral("origin"), QStringLiteral("agent:director")}} : QJsonObject{});
    } catch (...) {
      r = engine::Result::failure(QStringLiteral("internal"), QStringLiteral("The assistant stopped unexpectedly."));
    }
    QVariantMap out;
    if (r.ok) {
      out = r.value.toObject().toVariantMap();
    } else {
      out.insert(QStringLiteral("ok"), false);
      out.insert(QStringLiteral("error"), r.error.code);
      out.insert(QStringLiteral("errorMessage"), r.error.message);
    }
    out.insert(QStringLiteral("decision"), apply ? QStringLiteral("apply") : QStringLiteral("skip"));
    finishAgentRun(out);
  });
}

void Editor::agentStop() {
  if (agentBusy_) {
    stopRequested_ = true;
    if (auto* h = ai() ? ai()->harness() : nullptr) h->requestCancel();
  } else if (project_.agentLocked()) {
    // an agent outside the window (API, MCP) left its undo group open: the human can always take the project back
    project_.forceEndAgentGroup();
    emit toastRequested(QStringLiteral("Stopped. Its edits so far are one undo step."), QStringLiteral("info"));
  }
}

QVariantList Editor::storyboard() {
  const engine::Result r = engine()->call(QStringLiteral("storyboard.get"));
  QVariantList out;
  if (!r.ok) return out;
  for (const QJsonValue& v : r.value.toObject().value(QStringLiteral("scenes")).toArray()) out.push_back(v.toObject().toVariantMap());
  return out;
}

// ---------- AI settings ----------

namespace {

QString maskSecret(const QString& s) {
  if (s.isEmpty()) return QString();
  return s.size() <= 8 ? QString(s.size(), QChar(0x2022)) : QString(6, QChar(0x2022)) + s.right(4);
}

QStringList envOverridesOf(const QStringList& names) {
  QStringList out;
  for (const QString& n : names)
    if (!qEnvironmentVariable(n.toUtf8().constData()).trimmed().isEmpty()) out << n;
  return out;
}

} // namespace

QVariantMap Editor::aiConfig() const {
  const agent::LlmConfig& cfg = aiOptions_.llm; // what the agents actually run on: environment, saved settings, then --llm-* options
  QSettings s;
  QVariantMap m;
  m.insert(QStringLiteral("endpoint"), cfg.endpoint);
  m.insert(QStringLiteral("model"), cfg.model);
  m.insert(QStringLiteral("modelFast"), cfg.modelFast == cfg.model ? QString() : cfg.modelFast);
  m.insert(QStringLiteral("modelDeep"), cfg.modelDeep == cfg.model ? QString() : cfg.modelDeep);
  m.insert(QStringLiteral("modelJudge"), cfg.modelJudge == cfg.model ? QString() : cfg.modelJudge);
  m.insert(QStringLiteral("hasApiKey"), !cfg.apiKey.isEmpty());
  m.insert(QStringLiteral("apiKeyHint"), maskSecret(cfg.apiKey));
  m.insert(QStringLiteral("comfyUrl"), s.value(QStringLiteral("comfy/url")).toString());
  m.insert(QStringLiteral("comfyHome"), s.value(QStringLiteral("comfy/home")).toString());
  m.insert(QStringLiteral("comfyPython"), s.value(QStringLiteral("comfy/python")).toString());
  m.insert(QStringLiteral("comfyAutoStart"), s.value(QStringLiteral("comfy/autoStart"), false).toBool());
  m.insert(QStringLiteral("modelsDir"), s.value(QStringLiteral("jobs/modelsDir")).toString());
  const QString routerKey = s.value(QStringLiteral("comfyRouter/apiKey")).toString();
  const QString envRouter = qEnvironmentVariable("COMFY_API_KEY");
  m.insert(QStringLiteral("hasRouterKey"), !routerKey.isEmpty() || !envRouter.isEmpty());
  m.insert(QStringLiteral("routerKeyHint"), maskSecret(routerKey.isEmpty() ? envRouter : routerKey));
  m.insert(QStringLiteral("envOverrides"),
           envOverridesOf({QStringLiteral("SF_LLM_ENDPOINT"), QStringLiteral("SF_LLM_MODEL"), QStringLiteral("SF_LLM_MODEL_FAST"), QStringLiteral("SF_LLM_MODEL_DEEP"),
                           QStringLiteral("SF_LLM_MODEL_JUDGE"), QStringLiteral("SF_LLM_API_KEY"), QStringLiteral("SF_COMFY_URL"), QStringLiteral("SF_COMFY_HOME"),
                           QStringLiteral("SF_COMFY_PYTHON"), QStringLiteral("COMFY_API_KEY"), QStringLiteral("SF_MODELS_DIR")}));
  return m;
}

bool Editor::setAiConfig(const QVariantMap& c) {
  QSettings s;
  const auto put = [&](const QString& key, const QString& value) {
    const QString v = value.trimmed();
    if (v.isEmpty()) s.remove(key);
    else s.setValue(key, v);
  };
  put(QStringLiteral("SF_LLM_ENDPOINT"), c.value(QStringLiteral("endpoint")).toString());
  put(QStringLiteral("SF_LLM_MODEL"), c.value(QStringLiteral("model")).toString());
  put(QStringLiteral("SF_LLM_MODEL_FAST"), c.value(QStringLiteral("modelFast")).toString());
  put(QStringLiteral("SF_LLM_MODEL_DEEP"), c.value(QStringLiteral("modelDeep")).toString());
  put(QStringLiteral("SF_LLM_MODEL_JUDGE"), c.value(QStringLiteral("modelJudge")).toString());
  if (c.value(QStringLiteral("clearApiKey")).toBool()) s.remove(QStringLiteral("SF_LLM_API_KEY"));
  else if (!c.value(QStringLiteral("apiKey")).toString().trimmed().isEmpty()) s.setValue(QStringLiteral("SF_LLM_API_KEY"), c.value(QStringLiteral("apiKey")).toString().trimmed());
  put(QStringLiteral("comfy/url"), c.value(QStringLiteral("comfyUrl")).toString());
  put(QStringLiteral("comfy/home"), c.value(QStringLiteral("comfyHome")).toString());
  put(QStringLiteral("comfy/python"), c.value(QStringLiteral("comfyPython")).toString());
  s.setValue(QStringLiteral("comfy/autoStart"), c.value(QStringLiteral("comfyAutoStart")).toBool());
  put(QStringLiteral("jobs/modelsDir"), c.value(QStringLiteral("modelsDir")).toString());
  if (c.value(QStringLiteral("clearRouterKey")).toBool()) s.remove(QStringLiteral("comfyRouter/apiKey"));
  else if (!c.value(QStringLiteral("routerKey")).toString().trimmed().isEmpty()) s.setValue(QStringLiteral("comfyRouter/apiKey"), c.value(QStringLiteral("routerKey")).toString().trimmed());
  s.sync();
  if (s.status() != QSettings::NoError) {
    emit toastRequested(QStringLiteral("Could not save AI settings."), QStringLiteral("error"));
    return false;
  }
  agent::LlmConfig cfg = agent::LlmConfig::fromEnvironment();
  cfg.timeoutMs = aiOptions_.llm.timeoutMs;
  aiOptions_.llm = cfg;
  if (ai_) ai_->setLlmConfig(cfg); // before the first agent use there is nothing to rebuild: engine() reads aiOptions_
  emit toastRequested(QStringLiteral("AI settings saved."), QStringLiteral("info"));
  return true;
}

void Editor::testComfy(const QString& url) {
  const QString text = url.trimmed().isEmpty() ? qEnvironmentVariable("SF_COMFY_URL", QStringLiteral("http://127.0.0.1:8188")) : url.trimmed();
  const QUrl base(text);
  if (!base.isValid() || base.scheme().isEmpty()) {
    emit comfyTested(false, QStringLiteral("That is not a URL. Try http://127.0.0.1:8188."));
    return;
  }
  QUrl target = base;
  target.setPath(base.path().endsWith(QLatin1Char('/')) ? base.path() + QStringLiteral("system_stats") : base.path() + QStringLiteral("/system_stats"));
  auto* nam = new QNetworkAccessManager(this);
  QNetworkRequest req(target);
  req.setTransferTimeout(3000);
  QNetworkReply* reply = nam->get(req);
  connect(reply, &QNetworkReply::finished, this, [this, reply, nam, base] {
    const bool ok = reply->error() == QNetworkReply::NoError;
    emit comfyTested(ok, ok ? QStringLiteral("ComfyUI answered at %1.").arg(base.toString()) : QStringLiteral("Can't reach ComfyUI at %1. Is it running?").arg(base.toString()));
    reply->deleteLater();
    nam->deleteLater();
  });
}

// ---------- Home ----------

QVariantList Editor::homeProjects() const {
  QVariantList out;
  for (const QVariant& v : recent_.entries()) {
    const QVariantMap e = v.toMap();
    const QString path = e.value(QStringLiteral("path")).toString();
    QVariantMap m{{QStringLiteral("path"), path}, {QStringLiteral("name"), e.value(QStringLiteral("name"))}, {QStringLiteral("exists"), e.value(QStringLiteral("exists"))}};
    const QFileInfo fi(path);
    m.insert(QStringLiteral("modified"), fi.exists() ? fi.lastModified().toString(Qt::ISODate) : QString());
    m.insert(QStringLiteral("width"), 1920);
    m.insert(QStringLiteral("height"), 1080);
    m.insert(QStringLiteral("durationSec"), 0.0);
    if (fi.exists()) {
      try {
        const ProjectBundle b = loadProjectBundle(path);
        m.insert(QStringLiteral("name"), b.doc.project.name);
        m.insert(QStringLiteral("width"), static_cast<int>(b.doc.project.width));
        m.insert(QStringLiteral("height"), static_cast<int>(b.doc.project.height));
        Frame end = 0;
        for (const Item& it : b.doc.items) end = std::max(end, it.startFrame + it.durationFrames);
        m.insert(QStringLiteral("durationSec"), b.doc.project.fps > 0 ? static_cast<double>(end) / static_cast<double>(b.doc.project.fps) : 0.0);
      } catch (const std::exception&) {
      }
      m.insert(QStringLiteral("thumb"), QStringLiteral("image://projthumb/") + QString::fromLatin1(QUrl::toPercentEncoding(path)));
    }
    out.push_back(m);
  }
  return out;
}

bool Editor::renameProjectFile(const QString& path, const QString& name) {
  const QString n = name.trimmed();
  if (n.isEmpty()) return false;
  if (path == project_.path()) {
    emit toastRequested(QStringLiteral("Open projects are renamed from the editor."), QStringLiteral("info"));
    return false;
  }
  try {
    ProjectBundle b = loadProjectBundle(path);
    b.doc.project.name = n;
    saveProjectBundle(path, b, true);
  } catch (const std::exception& e) {
    emit toastRequested(QStringLiteral("Could not rename: %1").arg(QString::fromUtf8(e.what())), QStringLiteral("error"));
    return false;
  }
  emit recent_.changed();
  return true;
}

bool Editor::duplicateProjectFile(const QString& path) {
  try {
    ProjectBundle b = loadProjectBundle(path);
    const QFileInfo fi(path);
    const QString newName = b.doc.project.name + QStringLiteral(" copy");
    QString target = fi.absoluteDir().filePath(fi.completeBaseName() + QStringLiteral(" copy.json"));
    for (int i = 2; QFileInfo::exists(target); ++i) target = fi.absoluteDir().filePath(QStringLiteral("%1 copy %2.json").arg(fi.completeBaseName()).arg(i));
    b.doc.project.name = newName;
    b.doc.project.id = newId(QStringLiteral("prj"));
    saveProjectBundle(target, b, true);
    recent_.add(target);
    emit toastRequested(QStringLiteral("Created \"%1\"").arg(newName), QStringLiteral("info"));
    return true;
  } catch (const std::exception& e) {
    emit toastRequested(QStringLiteral("Could not duplicate: %1").arg(QString::fromUtf8(e.what())), QStringLiteral("error"));
    return false;
  }
}

void Editor::removeRecent(const QString& path) { recent_.remove(path); }

void Editor::revealInFolder(const QString& path) {
  const QFileInfo fi(path);
#ifdef Q_OS_WIN
  if (fi.exists()) {
    QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,"), QDir::toNativeSeparators(fi.absoluteFilePath())});
    return;
  }
#endif
  QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
}

QString Editor::appVersion() const { return QCoreApplication::applicationVersion(); }

} // namespace sf::app
