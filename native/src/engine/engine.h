#pragma once

// The headless engine: owns open project sessions (Project + MediaPool) and exposes ONE typed command surface.
// Everything outside (local JSON-RPC API, MCP server, CLI, the GUI's API toggle) is a thin transport over
// Engine::call(name, args) -> Result.
//
// - Every command has a JSON-schema'd argument object, a domain tag (editor | colour | audio | engine: "engine" is
//   shared infrastructure that every agent may use: sessions, reads, media, render, export) and a `mutates` flag.
//   The registry is introspectable (commands(), describe()), so a consumer can scope commands per domain.
// - Mutating commands accept `origin` (e.g. "agent:editor"), recorded as the history entry's actor.
// - Errors are values (Result: code + message + details); nothing throws across call().
// - Threading: the engine lives on one thread (the one with the event loop). Long jobs (export, proxies, imports)
//   run on their own threads and report through events.

#include "editor/media_pool.h"
#include "editor/project.h"
#include "engine/result.h"
#include "export/exporter.h"
#include "jobs/job_scheduler.h"
#include "render/offscreen.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace sf::engine {

class Engine;
class ComfyService;

} // namespace sf::engine

template <>
struct std::default_delete<sf::engine::ComfyService> {
  void operator()(sf::engine::ComfyService* p) const;
};

namespace sf::engine {

struct Session {
  QString id;
  editor::Project* project = nullptr;
  editor::MediaPool* pool = nullptr;
  std::unique_ptr<editor::Project> ownedProject; // empty for a session attached to the GUI's project
  std::unique_ptr<editor::MediaPool> ownedPool;
  bool external() const { return !ownedProject; }
};

struct CallContext {
  Engine* engine = nullptr;
  Session* session = nullptr; // null for commands that don't need one
  QString origin;             // "" for none
  QString actor() const { return origin.isEmpty() ? QStringLiteral("api") : origin; }
};

using Handler = std::function<Result(CallContext&, const QJsonObject& args)>;

struct CommandSpec {
  QString name;        // "timeline.get"
  QString alias;       // the Electron tool name this ports ("getTimeline"), may be empty
  QString description;
  QString domain;      // editor | colour | audio | engine
  bool mutates = false;
  bool needsSession = true;
  QJsonObject inputSchema; // JSON schema of the arguments (`session` / `origin` are added automatically)
  Handler handler;
  // Declared last so the 8-arg braced initializations (agent/index/copilot commands) keep
  // binding positionally. False only for the mutations core has no op for (project.color.set):
  // not undoable via history, and MCP's destructiveHint reflects it.
  bool undoable = true;
};

struct ExportRecord {
  QString id;
  QString sessionId;
  QString state = QStringLiteral("running"); // running | done | failed | cancelled
  QString outputPath;
  qint64 frame = 0, totalFrames = 0;
  double fraction = 0, fps = 0, etaSec = 0;
  QString stage;
  QString error;
  QJsonObject result;
  QJsonArray warnings;
  std::unique_ptr<xport::ExportJob> job;
};

class Engine : public QObject {
  Q_OBJECT

public:
  explicit Engine(QObject* parent = nullptr);
  ~Engine() override;

  // ---- sessions ----
  // A new empty project (not saved anywhere); becomes the current session.
  QString createSession();
  // Wraps a project the caller owns (the GUI's) so commands act on it live. The caller keeps ownership.
  QString attach(editor::Project* project, editor::MediaPool* pool, const QString& id = QStringLiteral("main"));
  void detach(const QString& id);
  Session* session(const QString& id = {}); // "" = the current one
  QStringList sessionIds() const;
  const QString& currentSession() const { return current_; }
  bool closeSession(QString id); // by value: callers often pass Session::id of the session being destroyed

  // ---- registry ----
  void add(CommandSpec spec);
  const CommandSpec* find(const QString& name) const; // name, Electron alias, or the name with "_" for "."
  // Replaces a command's handler with wrapper(old handler) (guards, lazy setup). False when there is no such command.
  bool wrap(const QString& name, const std::function<Handler(Handler)>& wrapper);
  const std::vector<std::unique_ptr<CommandSpec>>& commands() const { return commands_; }
  // [{name, alias, description, domain, mutates, inputSchema}], optionally one domain only.
  QJsonArray describe(const QString& domain = {}) const;

  // ---- calls ----
  Result call(const QString& name, const QJsonObject& args = {});

  // ---- shared services used by command handlers ----
  render::OffscreenRenderer* renderer(QString* error); // created on first use
  std::map<QString, ExportRecord>& exports() { return exports_; }
  // The background analysis scheduler (created on first use with the built-in analyzers; pauses itself while an export runs).
  jobs::JobScheduler& jobs();
  // ComfyUI server connection, schema caching, and in-memory workflow documents.
  ComfyService& comfy();
  void emitEvent(const QString& name, const QJsonObject& data);
  qint64 eventSeq() const { return seq_; }

signals:
  // Events: "ops.applied", "doc.changed", "session.opened", "session.closed", "export.progress", "export.finished",
  // "media.imported", "media.preview", "media.proxy", "comfy.state". Data always carries "session" where one applies.
  void event(const QString& name, const QJsonObject& data, qint64 seq);

private:
  void wire(Session& s);
  std::optional<Result> agentBusyRefusal(const CommandSpec& cmd, const CallContext& ctx, const QJsonObject& args);

  std::vector<std::unique_ptr<Session>> sessions_;
  QString current_;
  int nextSession_ = 1;
  std::vector<std::unique_ptr<CommandSpec>> commands_;
  std::map<QString, ExportRecord> exports_;
  std::unique_ptr<render::OffscreenRenderer> renderer_;
  std::unique_ptr<jobs::JobScheduler> jobs_;
  std::unique_ptr<ComfyService> comfy_;
  qint64 seq_ = 0;
};

// Registered by engine.cpp's constructor (one function per file).
void registerSessionCommands(Engine& e);
void registerReadCommands(Engine& e);
void registerEditCommands(Engine& e);
void registerMediaCommands(Engine& e);
void registerRenderCommands(Engine& e);
void registerColourCommands(Engine& e);
void registerAudioCommands(Engine& e);
void registerTimelineCommands(Engine& e);
void registerSafetyCommands(Engine& e);
void registerStyleCommands(Engine& e);
void registerSystemCommands(Engine& e);
void registerExportCommands(Engine& e);
void registerFxCommands(Engine& e);
void registerJobsCommands(Engine& e);
void registerComfyCommands(Engine& e);
void registerVariationsCommands(Engine& e);
void registerAssembleCommands(Engine& e);
void registerRecipesCommands(Engine& e);
void registerTitleCommands(Engine& e);

} // namespace sf::engine
