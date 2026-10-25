#include "engine/engine.h"

#include "core/error.h"
#include "engine/cmd_util.h"
#include "engine/cmd_comfy.h"
#include "engine/cmd_storyboard.h"
#include "jobs/builtin_analyzers.h"
#include "comfy/comfy_generate.h"

#include <QCoreApplication>
#include <QJsonDocument>

#include <algorithm>
#include <exception>

namespace sf::engine {

namespace {

QString normalise(QString name) { return name.replace(QLatin1Char('_'), QLatin1Char('.')); }

} // namespace

Engine::Engine(QObject* parent) : QObject(parent) {
  qRegisterMetaType<sf::xport::ExportProgress>();
  qRegisterMetaType<sf::xport::ExportResult>();
  registerSessionCommands(*this);
  registerReadCommands(*this);
  registerEditCommands(*this);
  registerMediaCommands(*this);
  registerRenderCommands(*this);
  registerColourCommands(*this);
  registerAudioCommands(*this);
  registerTimelineCommands(*this);
  registerSafetyCommands(*this);
  registerStyleCommands(*this);
  registerSystemCommands(*this);
  registerExportCommands(*this);
  registerFxCommands(*this);
  registerJobsCommands(*this);
  registerComfyCommands(*this);
  registerVariationsCommands(*this);
  registerAssembleCommands(*this);
  registerRecipesCommands(*this);
  registerTitleCommands(*this);
  registerStoryboardCommands(*this);

  add(spec("engine.ping", "", "engine", false, QStringLiteral("Liveness and version: engine version, sessions, command count."), js::obj({}),
           [](CallContext& c, const QJsonObject&) {
             Engine& e = *c.engine;
             return Result::success(QJsonObject{{q("engine"), QStringLiteral("splitframe")},
                                                {q("version"), QCoreApplication::applicationVersion()},
                                                {q("sessions"), QJsonArray::fromStringList(e.sessionIds())},
                                                {q("current"), e.currentSession()},
                                                {q("commands"), static_cast<int>(e.commands().size())}});
           },
           false));
  add(spec("engine.commands", "", "engine", false,
           QStringLiteral("List every command with its JSON schema, domain (editor | colour | audio | engine) and whether it mutates the project."),
           js::obj({{q("domain"), js::choice({q("editor"), q("colour"), q("audio"), q("engine"), q("copilot"), q("branch")})}}),
           [](CallContext& c, const QJsonObject& a) { return Result::success(QJsonObject{{q("commands"), c.engine->describe(a.value(q("domain")).toString())}}); },
           false));
}

Engine::~Engine() {
  for (auto& [id, rec] : exports_)
    if (rec.job) {
      rec.job->disconnect(this);
      rec.job->cancel();
    }
  exports_.clear();
  if (jobs_) {
    jobs_->disconnect(this);
    jobs_->setBusyProbe({});
  }
  jobs_.reset();
  comfy_.reset();
  QCoreApplication::removePostedEvents(this);
  renderer_.reset();
  sessions_.clear();
}

// ---------- sessions ----------

void Engine::wire(Session& s) {
  const QString id = s.id;
  editor::Project* p = s.project;
  editor::MediaPool* pool = s.pool;
  connect(p, &editor::Project::opsApplied, this, [this, id, p](const std::vector<Op>& ops, const QString& label) {
    QJsonArray arr, inv;
    for (const Op& o : ops) arr.append(toJson(o));
    for (const Op& o : p->lastInverse()) inv.append(toJson(o));
    emitEvent(q("ops.applied"), {{q("session"), id}, {q("label"), label}, {q("actor"), p->lastActor()}, {q("count"), static_cast<int>(ops.size())},
                                 {q("ops"), arr}, {q("inverse"), inv}, {q("created"), createdIds(ops)}, {q("canUndo"), p->canUndo()}, {q("canRedo"), p->canRedo()}});
  });
  connect(p, &editor::Project::docChanged, this, [this, id, p] {
    emitEvent(q("doc.changed"), {{q("session"), id}, {q("dirty"), p->isDirty()}, {q("canUndo"), p->canUndo()}, {q("canRedo"), p->canRedo()}});
  });
  if (!pool) return;
  connect(pool, &editor::MediaPool::imported, this, [this, id](const editor::ImportSummary& sum) {
    emitEvent(q("media.imported"), {{q("session"), id}, {q("added"), QJsonArray::fromStringList(sum.added)},
                                    {q("duplicates"), QJsonArray::fromStringList(sum.duplicates)}, {q("skipped"), QJsonArray::fromStringList(sum.skipped)}});
  });
  connect(pool, &editor::MediaPool::previewReady, this, [this, id](const QString& asset) { emitEvent(q("media.preview"), {{q("session"), id}, {q("assetId"), asset}}); });
  connect(pool, &editor::MediaPool::proxiesChanged, this, [this, id, p, pool] {
    QJsonObject states;
    for (const Asset& a : p->assets()) {
      const QString st = pool->proxyState(a.id);
      if (st != QLatin1String("none")) states.insert(a.id, QJsonObject{{q("state"), st}, {q("progress"), pool->proxyProgress(a.id)}});
    }
    emitEvent(q("media.proxy"), {{q("session"), id}, {q("proxies"), states}});
  });
}

QString Engine::createSession() {
  auto s = std::make_unique<Session>();
  s->id = QStringLiteral("s%1").arg(nextSession_++);
  s->ownedProject = std::make_unique<editor::Project>();
  s->project = s->ownedProject.get();
  s->project->setAutosaveInterval(0);
  s->project->newProject(QStringLiteral("Untitled"));
  s->ownedPool = std::make_unique<editor::MediaPool>(*s->project);
  s->pool = s->ownedPool.get();
  wire(*s);
  const QString id = s->id;
  sessions_.push_back(std::move(s));
  current_ = id;
  emitEvent(q("session.opened"), {{q("session"), id}});
  return id;
}

QString Engine::attach(editor::Project* project, editor::MediaPool* pool, const QString& id) {
  auto s = std::make_unique<Session>();
  s->id = id;
  s->project = project;
  s->pool = pool;
  wire(*s);
  sessions_.push_back(std::move(s));
  current_ = id;
  emitEvent(q("session.opened"), {{q("session"), id}});
  return id;
}

void Engine::detach(const QString& id) {
  closeSession(id);
}

Session* Engine::session(const QString& id) {
  const QString want = id.isEmpty() ? current_ : id;
  for (auto& s : sessions_)
    if (s->id == want) return s.get();
  return nullptr;
}

QStringList Engine::sessionIds() const {
  QStringList ids;
  for (const auto& s : sessions_) ids << s->id;
  return ids;
}

bool Engine::closeSession(QString id) {
  const auto it = std::find_if(sessions_.begin(), sessions_.end(), [&](const auto& s) { return s->id == id; });
  if (it == sessions_.end()) return false;
  if ((*it)->project) disconnect((*it)->project, nullptr, this, nullptr);
  if ((*it)->pool) disconnect((*it)->pool, nullptr, this, nullptr);
  for (auto& [eid, rec] : exports_)
    if (rec.sessionId == id && rec.job && rec.state == QLatin1String("running")) rec.job->cancel();
  sessions_.erase(it);
  if (current_ == id) current_ = sessions_.empty() ? QString() : sessions_.back()->id;
  emitEvent(q("session.closed"), {{q("session"), id}});
  return true;
}

// ---------- registry ----------

void Engine::add(CommandSpec s) {
  QJsonObject schema = s.inputSchema;
  QJsonObject props = schema.value(q("properties")).toObject();
  props.insert(q("session"), js::str(QStringLiteral("Session id (default: the current session).")));
  if (s.mutates) props.insert(q("origin"), js::str(QStringLiteral("Who is making this change, recorded in the history entry (e.g. \"agent:editor\").")));
  schema.insert(q("type"), q("object"));
  schema.insert(q("properties"), props);
  if (!schema.contains(q("additionalProperties"))) schema.insert(q("additionalProperties"), false);
  s.inputSchema = schema;
  commands_.push_back(std::make_unique<CommandSpec>(std::move(s)));
}

const CommandSpec* Engine::find(const QString& name) const {
  const QString n = normalise(name);
  for (const auto& c : commands_)
    if (c->name == n) return c.get();
  for (const auto& c : commands_)
    if (!c->alias.isEmpty() && c->alias == name) return c.get();
  return nullptr;
}

bool Engine::wrap(const QString& name, const std::function<Handler(Handler)>& wrapper) {
  const CommandSpec* found = find(name);
  if (!found) return false;
  for (auto& c : commands_)
    if (c.get() == found) {
      c->handler = wrapper(std::move(c->handler));
      return true;
    }
  return false;
}

QJsonArray Engine::describe(const QString& domain) const {
  QJsonArray out;
  for (const auto& c : commands_) {
    if (!domain.isEmpty() && c->domain != domain) continue;
    QJsonObject o{{q("name"), c->name}, {q("description"), c->description}, {q("domain"), c->domain}, {q("mutates"), c->mutates}, {q("inputSchema"), c->inputSchema}};
    if (!c->alias.isEmpty()) o.insert(q("alias"), c->alias);
    out.append(o);
  }
  return out;
}

// ---------- calls ----------

// The copilot lock: while an agent holds a session's project (an open "agent:*" undo group, or the GUI's run reservation),
// every other caller's mutating command is refused with `agent_busy`. The one place all transports (GUI, API, MCP, CLI) pass through.
std::optional<Result> Engine::agentBusyRefusal(const CommandSpec& cmd, const CallContext& ctx, const QJsonObject& args) {
  // mutates, plus the undo-group commands (they would close or roll back the agent's group) and the project replacements
  const bool groupCmd = cmd.name.startsWith(QLatin1String("session.group."));
  const bool replaces = cmd.name == QLatin1String("project.new") || cmd.name == QLatin1String("project.open") || cmd.name == QLatin1String("project.close");
  if (!cmd.mutates && !groupCmd && !replaces) return std::nullopt;
  Session* target = ctx.session;
  if (!target) {
    if (args.contains(q("session"))) target = session(args.value(q("session")).toString());
    else if (!replaces) target = session(); // agent.chat & co act on the current session
  }
  if (!target || !target->project) return std::nullopt;
  const editor::Project& p = *target->project;
  if (!p.agentLocked()) return std::nullopt;
  const QString actor = groupCmd && args.contains(q("actor")) ? args.value(q("actor")).toString() : ctx.actor();
  if (editor::Project::isAgentActor(actor)) return std::nullopt;
  return Result::failure(q("agent_busy"), p.agentBusyMessage(), QJsonObject{{q("agent"), p.agentLockOwner()}, {q("command"), cmd.name}});
}

Result Engine::call(const QString& name, const QJsonObject& args) {
  const CommandSpec* cmd = find(name);
  if (!cmd) {
    QStringList near;
    const QString n = normalise(name).toLower();
    for (const auto& c : commands_)
      if (c->name.contains(n) || n.contains(c->name) || (n.size() > 3 && c->name.startsWith(n.left(n.indexOf(QLatin1Char('.')) + 1)) && n.contains(QLatin1Char('.')))) near << c->name;
    near = near.mid(0, 5);
    return Result::failure(q("unknown_command"), QStringLiteral("Unknown command \"%1\". Call engine.commands to list them.").arg(name),
                           QJsonObject{{q("suggestions"), QJsonArray::fromStringList(near)}});
  }
  if (const auto err = js::validate(args, cmd->inputSchema)) return Result::failure(q("invalid_params"), *err, QJsonObject{{q("command"), cmd->name}});

  CallContext ctx;
  ctx.engine = this;
  ctx.origin = args.value(q("origin")).toString();
  if (cmd->needsSession) {
    const QString sid = args.value(q("session")).toString();
    ctx.session = session(sid);
    if (!ctx.session)
      return Result::failure(q("no_session"), sid.isEmpty() ? QStringLiteral("No project is open. Call project.new or project.open first.")
                                                              : QStringLiteral("No session \"%1\".").arg(sid));
  }
  if (const auto busy = agentBusyRefusal(*cmd, ctx, args)) return *busy;
  try {
    return cmd->handler(ctx, args);
  } catch (const SchemaError& e) {
    return Result::failure(q("invalid_params"), QString::fromUtf8(e.what()), QJsonObject{{q("command"), cmd->name}});
  } catch (const OpError& e) {
    return Result::failure(q("edit_rejected"), QString::fromUtf8(e.what()), QJsonObject{{q("hint"), e.hint()}});
  } catch (const std::exception& e) {
    return Result::failure(q("internal"), QString::fromUtf8(e.what()));
  } catch (...) {
    return Result::failure(q("internal"), QStringLiteral("Unknown failure."));
  }
}

// ---------- services ----------

jobs::JobScheduler& Engine::jobs() {
  if (!jobs_) {
    jobs_ = std::make_unique<jobs::JobScheduler>();
    jobs::registerBuiltinAnalyzers(*jobs_);
    jobs_->registerAnalyzer(std::make_unique<comfy::ComfyGenerateAnalyzer>(
      comfy::ServerStateProvider{
        [this]() { return comfy_ ? comfy_->serverState() : comfy::ComfyServerState::None; },
        [this]() { return comfy_ ? comfy_->lastExit() : QJsonObject{}; },
        [this](int maxLines) { return comfy_ ? comfy_->recentLogs(maxLines) : QStringList{}; }
      }
    ));
    jobs_->setBusyProbe([this] {
      for (const auto& [id, rec] : exports_)
        if (rec.state == QLatin1String("running")) return true;
      return false;
    });
    connect(jobs_.get(), &jobs::JobScheduler::jobChanged, this, [this](const QString& id) {
      if (!jobs_) return;
      if (const auto i = jobs_->info(id)) emitEvent(q("jobs.updated"), {{q("jobId"), id}, {q("analyzer"), i->analyzer}, {q("state"), jobs::jobStateName(i->state)}, {q("progress"), i->progress}});
    });
    connect(jobs_.get(), &jobs::JobScheduler::pauseChanged, this, [this](bool paused, const QStringList& reasons) {
      if (!jobs_) return;
      emitEvent(q("jobs.paused"), {{q("paused"), paused}, {q("reasons"), QJsonArray::fromStringList(reasons)}});
    });
  }
  return *jobs_;
}

render::OffscreenRenderer* Engine::renderer(QString* error) {
  if (!renderer_) renderer_ = render::OffscreenRenderer::create(error);
  return renderer_.get();
}

void Engine::emitEvent(const QString& name, const QJsonObject& data) { emit event(name, data, ++seq_); }

} // namespace sf::engine
