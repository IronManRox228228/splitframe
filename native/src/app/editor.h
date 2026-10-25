#pragma once

// The editor facade QML talks to: owns the open project and everything built on it (media pool,
// timeline models, controller, inspector, recent files) and keeps the Player in step with the
// document. Anything that needs a dialog (save as, unsaved changes) is a signal the QML answers.

#include "ai/ai_services.h"
#include "app/colour_page.h"
#include "app/export_controller.h"
#include "app/mixer_model.h"
#include "app/player.h"
#include "editor/inspector.h"
#include "editor/media_pool.h"
#include "editor/project.h"
#include "editor/recent_projects.h"
#include "editor/timeline_controller.h"
#include "editor/timeline_model.h"

#include "engine/api_server.h"
#include "engine/engine.h"

#include <QElapsedTimer>
#include <QObject>
#include <QQmlEngine>
#include <QUrl>

namespace sf::app {

class Editor : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("created by the application")

  Q_PROPERTY(sf::editor::Project* project READ project CONSTANT)
  Q_PROPERTY(sf::editor::MediaPool* pool READ pool CONSTANT)
  Q_PROPERTY(sf::editor::TimelineModel* timeline READ timeline CONSTANT)
  Q_PROPERTY(sf::editor::TrackModel* tracks READ tracks CONSTANT)
  Q_PROPERTY(sf::editor::TimelineController* controller READ controller CONSTANT)
  Q_PROPERTY(sf::editor::Inspector* inspector READ inspector CONSTANT)
  Q_PROPERTY(sf::editor::RecentProjects* recent READ recent CONSTANT)
  Q_PROPERTY(sf::app::Player* player READ player CONSTANT)
  Q_PROPERTY(sf::app::ExportController* exporter READ exporter CONSTANT)
  Q_PROPERTY(sf::app::MixerModel* mixer READ mixer CONSTANT)
  Q_PROPERTY(sf::app::ColourPage* colour READ colour CONSTANT)
  Q_PROPERTY(QString windowTitle READ windowTitle NOTIFY titleChanged)
  Q_PROPERTY(QString projectName READ projectName NOTIFY titleChanged)
  Q_PROPERTY(QString projectPath READ projectPath NOTIFY titleChanged)
  Q_PROPERTY(bool dirty READ dirty NOTIFY titleChanged)
  Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
  Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
  Q_PROPERTY(QString undoLabel READ undoLabel NOTIFY historyChanged)
  Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
  Q_PROPERTY(int selectionCount READ selectionCount NOTIFY selectionChanged)
  // Preview from proxy files (made in the background for every video asset while on). Export always uses the originals.
  Q_PROPERTY(bool useProxies READ useProxies WRITE setUseProxies NOTIFY useProxiesChanged)
  // The local JSON-RPC / SSE API (engine/api_server.h) on 127.0.0.1, driving THIS project live. Off by default.
  Q_PROPERTY(bool apiEnabled READ apiEnabled WRITE setApiEnabled NOTIFY apiChanged)
  Q_PROPERTY(QString apiStatus READ apiStatus NOTIFY apiChanged)
  // The Assistant (Director crew). `agentMode` is the engine's mode: plan | ask | default | auto.
  // True from the moment a run is requested until it ends (and while any other agent holds the project's undo group): the
  // copilot lock. Edits by anyone but the agent are refused meanwhile (error agent_busy); play, scrub, zoom and selection work.
  Q_PROPERTY(bool agentBusy READ agentBusy NOTIFY agentBusyChanged)
  Q_PROPERTY(QString agentBusyName READ agentBusyName NOTIFY agentBusyChanged)
  Q_PROPERTY(QString agentMode READ agentMode WRITE setAgentMode NOTIFY agentModeChanged)
  // Demo mode hook: while on, the Assistant replaces its text input with one clickable block per entry of `demoBlocks`
  // ([{id, label, hint?}]) and emits demoBlockClicked(id). The demo task fills the list and does the replay.
  Q_PROPERTY(bool demoMode READ demoMode WRITE setDemoMode NOTIFY demoChanged)
  Q_PROPERTY(QVariantList demoBlocks READ demoBlocks WRITE setDemoBlocks NOTIFY demoChanged)

public:
  struct Options {
    QString settingsFile;  // ini file instead of the registry (tests, scripted runs)
    QString recoveryDir;   // autosave folder
    int autosaveMs = 20000;
    ai::AiOptions ai;      // LLM endpoint / model for the agent commands (SF_LLM_* by default)
  };

  explicit Editor(Player& player, const Options& options, QObject* parent = nullptr);

  editor::Project* project() { return &project_; }
  editor::MediaPool* pool() { return &pool_; }
  editor::TimelineModel* timeline() { return &timeline_; }
  editor::TrackModel* tracks() { return &tracks_; }
  editor::TimelineController* controller() { return &controller_; }
  editor::Inspector* inspector() { return &inspector_; }
  editor::RecentProjects* recent() { return &recent_; }
  Player* player() { return &player_; }
  ExportController* exporter() { return &export_; }
  MixerModel* mixer() { return &mixer_; }
  ColourPage* colour() { return &colour_; }

  QString windowTitle() const;
  QString projectName() const { return project_.name(); }
  QString projectPath() const { return project_.path(); }
  bool dirty() const { return project_.isDirty(); }
  bool canUndo() const { return project_.canUndo(); }
  bool canRedo() const { return project_.canRedo(); }
  QString undoLabel() const { return project_.undoLabel(); }
  bool useProxies() const { return useProxies_; }
  void setUseProxies(bool on);
  bool apiEnabled() const { return api_ && api_->running(); }
  void setApiEnabled(bool on);
  void setApiPort(quint16 port) { apiPort_ = port; } // before enabling; 0 = any free port
  QString apiStatus() const;
  bool agentBusy() const { return agentBusy_ || project_.agentLocked(); }
  // "Director", "Editor"...: who holds the lock (empty when nobody)
  QString agentBusyName() const;
  QString agentMode() const { return agentMode_; }
  void setAgentMode(const QString& mode);
  bool demoMode() const { return demoMode_; }
  void setDemoMode(bool on);
  QVariantList demoBlocks() const { return demoBlocks_; }
  void setDemoBlocks(const QVariantList& blocks);
  // The engine acting on THIS project live (undo history, window), with the agent / copilot / branch / index commands registered.
  // Created on first use (the API server, a future agent panel); edits it makes carry their actor ("agent:editor", "api", ...).
  engine::Engine* engine();
  ai::AiServices* ai() { engine(); return ai_.get(); }
  bool hasSelection() const { return !project_.selection().isEmpty(); }
  int selectionCount() const { return static_cast<int>(project_.selection().size()); }

  // ---- project lifecycle (callers handle unsaved changes first: see dirty) ----
  Q_INVOKABLE void newProject(const QString& name = QString(), int width = 1920, int height = 1080, int fps = 30);
  // A project .json, or a media file wrapped as a one-clip project. False with a toast on failure.
  Q_INVOKABLE bool openPath(const QString& pathOrUrl);
  Q_INVOKABLE bool save();   // false when it needs a file name first (saveAsRequested is emitted)
  Q_INVOKABLE bool saveAs(const QString& pathOrUrl);
  // Autosaves left behind by a crash: [{autosavePath, name, originalPath, modified}]
  Q_INVOKABLE QVariantList recoveries() const;
  Q_INVOKABLE bool restoreRecovery(const QString& autosavePath);
  Q_INVOKABLE void discardRecovery(const QString& autosavePath);
  // The autosave next to `path`, if newer than the file ("" when none)
  Q_INVOKABLE QString recoveryFor(const QString& path) const;

  // ---- media ----
  Q_INVOKABLE void importFiles(const QVariantList& pathsOrUrls);

  // ---- editing shortcuts that span models ----
  Q_INVOKABLE void undo();
  Q_INVOKABLE void redo();
  Q_INVOKABLE void seek(qint64 frame);
  Q_INVOKABLE void step(qint64 frames);
  Q_INVOKABLE void gotoEdge(int dir);
  Q_INVOKABLE void toast(const QString& text, const QString& kind = QStringLiteral("info"));
  Q_INVOKABLE void copyText(const QString& text);   // system clipboard (the reply "Copy" button)

  // ---- the Assistant ----
  // Sends `message` to the crew: agent.chat (the Director) in default / auto mode, agent.turn in plan / ask mode (those hold
  // their work for agentDecide). Returns at once; the run happens on the next event-loop turn and the model client waits on
  // the network in a nested event loop, so the window keeps painting and the user keeps editing. Progress arrives as agentEvent,
  // the end as agentTurnFinished. Edits the agents make are normal undoable edits ("agent:*" actor), grouped per message.
  Q_INVOKABLE void agentChat(const QString& message);
  // Run (apply = true) or discard the plan / proposed edits a plan or ask turn is holding (agent.plan.run / agent.plan.skip).
  Q_INVOKABLE void agentDecide(bool apply);
  // Stop: the run ends after the tool call it is on, its undo group is committed (one undo step, what was done stays) and the lock
  // releases. A group an outside agent left open is closed the same way.
  Q_INVOKABLE void agentStop();
  // storyboard.get as a list of {id, title, intent, durationSec, status, shotPrompt, voText, ...}
  Q_INVOKABLE QVariantList storyboard();
  // ---- AI settings (QSettings; the same keys agent/llm_config.cpp and the Comfy client read) ----
  // {endpoint, model, modelFast, modelDeep, modelJudge, apiKeyHint, hasApiKey, comfyUrl, comfyHome, comfyPython, routerKeyHint,
  //  hasRouterKey, envOverrides: [names set in the environment, which win over the saved values]}. Secrets are masked.
  Q_INVOKABLE QVariantMap aiConfig() const;
  // Saves a map with the same keys (an absent / empty `apiKey` / `routerKey` keeps the stored secret, `clearApiKey` removes it)
  // and applies the model settings to the running agents. The Comfy URL applies from the next start.
  Q_INVOKABLE bool setAiConfig(const QVariantMap& config);
  // GET <url>/system_stats; the answer arrives as comfyTested.
  Q_INVOKABLE void testComfy(const QString& url);
  // ---- Home ----
  // Recent projects for the Home cards: [{path, name, exists, width, height, durationSec, modified (ISO), thumb (image URL)}]
  Q_INVOKABLE QVariantList homeProjects() const;
  Q_INVOKABLE bool renameProjectFile(const QString& path, const QString& name);
  Q_INVOKABLE bool duplicateProjectFile(const QString& path);
  Q_INVOKABLE void removeRecent(const QString& path);
  Q_INVOKABLE void revealInFolder(const QString& path);
  Q_INVOKABLE QString appVersion() const;

  static QString toLocalPath(const QString& pathOrUrl);

signals:
  void titleChanged();
  void historyChanged();
  void selectionChanged();
  void saveAsRequested();
  void useProxiesChanged();
  void apiChanged();
  void agentBusyChanged();
  void agentModeChanged();
  void demoChanged();
  // One agent.event: {agent: director|editor|colourist|audio, lane, kind: think|tool|result|review|message, text, tool?, args?, ok?}
  void agentEvent(const QVariantMap& event);
  // The end of an agentChat run: {ok, response, status, error?, errorMessage?, planNeeded?, plan?, pendingApproval?, proposed?}
  void agentTurnFinished(const QVariantMap& result);
  void storyboardChanged();
  // A generation job (generate.*, the Comfy analyzer) changed: {jobId, analyzer, state, progress}
  void generationJobChanged(const QVariantMap& job);
  void comfyTested(bool ok, const QString& message);
  void demoBlockClicked(const QString& id);
  // kind: info, error, undo (the toast offers Undo)
  void toastRequested(const QString& text, const QString& kind);

private:
  void onLoaded();
  void pushDocument();
  void pushAssets();
  void rememberRecent();
  void runAgent(const QString& message);
  void finishAgentRun(const QVariantMap& out);
  // True (with the busy toast) while an agent holds the project: for the Editor's own edits (lifecycle, import, undo)
  bool refusedWhileAgent();
  void onEngineEvent(const QString& name, const QJsonObject& data);
  render::AssetTable assetTable() const;
  bool wrapMedia(const QString& path);

  Player& player_;
  editor::Project project_;
  editor::MediaPool pool_;
  editor::TimelineModel timeline_;
  editor::TrackModel tracks_;
  editor::TimelineController controller_;
  editor::Inspector inspector_;
  editor::RecentProjects recent_;
  ExportController export_;
  MixerModel mixer_;
  ColourPage colour_;
  bool adoptingPlayhead_ = false;
  bool useProxies_ = false;
  QString proxyKey_;
  std::unique_ptr<engine::Engine> engine_;
  std::unique_ptr<ai::AiServices> ai_;
  std::unique_ptr<engine::ApiServer> api_;
  quint16 apiPort_ = 0;
  ai::AiOptions aiOptions_;
  bool agentBusy_ = false;
  bool stopRequested_ = false;
  QString agentMode_ = QStringLiteral("default");
  bool demoMode_ = false;
  QVariantList demoBlocks_;
  QByteArray lastAgentEvent_;
  QElapsedTimer lastAgentEventClock_;
};

} // namespace sf::app
