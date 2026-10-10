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
#include "render/offscreen.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>

#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace sf::engine {

class Engine;

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
  const std::vector<std::unique_ptr<CommandSpec>>& commands() const { return commands_; }
  // [{name, alias, description, domain, mutates, inputSchema}], optionally one domain only.
  QJsonArray describe(const QString& domain = {}) const;

  // ---- calls ----
  Result call(const QString& name, const QJsonObject& args = {});

  // ---- shared services used by command handlers ----
  render::OffscreenRenderer* renderer(QString* error); // created on first use
  std::map<QString, ExportRecord>& exports() { return exports_; }
  void emitEvent(const QString& name, const QJsonObject& data);
  qint64 eventSeq() const { return seq_; }

signals:
  // Events: "ops.applied", "doc.changed", "session.opened", "session.closed", "export.progress", "export.finished",
  // "media.imported", "media.preview", "media.proxy". Data always carries "session" where one applies.
  void event(const QString& name, const QJsonObject& data, qint64 seq);

private:
  void wire(Session& s);

  std::vector<std::unique_ptr<Session>> sessions_;
  QString current_;
  int nextSession_ = 1;
  std::vector<std::unique_ptr<CommandSpec>> commands_;
  std::map<QString, ExportRecord> exports_;
  std::unique_ptr<render::OffscreenRenderer> renderer_;
  qint64 seq_ = 0;
};

// Registered by engine.cpp's constructor (one function per file).
void registerSessionCommands(Engine& e);
void registerReadCommands(Engine& e);
void registerEditCommands(Engine& e);
void registerMediaCommands(Engine& e);
void registerRenderCommands(Engine& e);
void registerExportCommands(Engine& e);

} // namespace sf::engine
