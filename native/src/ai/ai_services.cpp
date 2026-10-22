#include "ai/ai_services.h"

#include "agent/llm_config.h"
#include "agent/meterless_router.h"
#include "copilot/copilot_commands.h"
#include "index/index_commands.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace sf::ai {

namespace {

QString q(const char* s) { return QString::fromLatin1(s); }

} // namespace

AiServices::AiServices(engine::Engine& engine, const AiOptions& options, QObject* parent) : QObject(parent), engine_(engine), options_(options) {
  llm_ = std::make_shared<agent::RecordingLlmClient>(
    agent::makeLlmClient(options_.llm, &configError_, &meterless_));
  // The fallback event becomes the one-line agent-panel notice (Part A item 5).
  if (const auto fb = std::dynamic_pointer_cast<agent::FallbackLlmClient>(llm_->inner())) {
    fb->setNoticeSink([this](const QString& note) { emit providerNotice(note); });
  }
  harness_ = std::make_unique<agent::AgentHarness>(&engine_, llm_);
  index_.setLlmClient(llm_);

  harness_->registerCommands(engine_);
  copilot::registerCopilotCommands(&engine_, &lint_, &suggest_, &branches_, &review_, index_.catalog());
  index::registerIndexCommands(&engine_, &index_);

  registerProviderCommands();

  guardAgentTurn();
  lazyBindings();
}

AiServices::~AiServices() {
  index_.closeCatalog();
  if (catalogIsTemp_ && !openCatalog_.isEmpty()) QFile::remove(openCatalog_);
}

void AiServices::setLlmClient(std::shared_ptr<agent::LlmClient> client) {
  llm_ = std::make_shared<agent::RecordingLlmClient>(std::move(client));
  harness_->setLlmClient(llm_);
  index_.setLlmClient(llm_);
}

void AiServices::registerProviderCommands() {
  // agent.provider.status (Part A item 3): provider, base URL, discovery-based reachability,
  // models and the three lanes. "Reachable" requires a successful discovery call (item 9).
  engine_.add(engine::CommandSpec{
    .name = QStringLiteral("agent.provider.status"),
    .alias = QStringLiteral("agentProviderStatus"),
    .description = QStringLiteral("Report the active model provider: base URL, discovery outcome, models and lanes."),
    .domain = QStringLiteral("engine"),
    .mutates = false,
    .needsSession = false,
    .inputSchema = QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}},
    .handler = [this](engine::CallContext&, const QJsonObject&) {
      if (meterless_) return engine::Result::success(meterless_->statusJson());
      // Local provider: lanes mirror the single local model; reachability is a live probe.
      QString why;
      const bool reachable = llm_ ? llm_->inner()->reachable(&why) : false;
      QJsonObject out{
        {QStringLiteral("provider"), QStringLiteral("local")},
        {QStringLiteral("endpoint"), options_.llm.endpoint},
        {QStringLiteral("model"), options_.llm.model},
        {QStringLiteral("reachable"), reachable},
        {QStringLiteral("lanes"), QJsonObject{
          {QStringLiteral("fast"), options_.llm.model},
          {QStringLiteral("deep"), options_.llm.model},
          {QStringLiteral("judge"), options_.llm.model}}}};
      if (!why.isEmpty()) out.insert(QStringLiteral("error"), why);
      return engine::Result::success(out);
    }
  });

  // agent.provider.route (meterless-power-prompt.md): planRoute(task, attachments) as a command.
  engine_.add(engine::CommandSpec{
    .name = QStringLiteral("agent.provider.route"),
    .alias = QStringLiteral("agentProviderRoute"),
    .description = QStringLiteral("Plan the Meterless route for a task (class, disposition, accept header, 3d routing)."),
    .domain = QStringLiteral("engine"),
    .mutates = false,
    .needsSession = false,
    .inputSchema = QJsonObject{
      {QStringLiteral("type"), QStringLiteral("object")},
      {QStringLiteral("properties"), QJsonObject{
        {QStringLiteral("task"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("attachments"), QJsonObject{{QStringLiteral("type"), QStringLiteral("array")}, {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}}
      }},
      {QStringLiteral("required"), QJsonArray{QStringLiteral("task")}}
    },
    .handler = [](engine::CallContext&, const QJsonObject& a) {
      sf::meterless_router::Attachments attachments;
      for (const QJsonValue& v : a.value(QStringLiteral("attachments")).toArray())
        attachments << v.toString();
      const QJsonObject planned = sf::meterless_router::planRoute(
        a.value(QStringLiteral("task")).toString(), attachments);
      if (planned.contains(QStringLiteral("error")))
        return engine::Result::failure(planned.value(QStringLiteral("error")).toString(),
                                       QStringLiteral("no Meterless route for this task"));
      return engine::Result::success(planned);
    }
  });
}

QString AiServices::catalogPathFor(const QString& projectPath, const QString& sessionId) {
  if (!projectPath.isEmpty()) {
    const QFileInfo fi(projectPath);
    return fi.absoluteDir().absoluteFilePath(fi.completeBaseName() + QStringLiteral(".splitframe-index.sqlite"));
  }
  return QDir::temp().absoluteFilePath(QStringLiteral("splitframe-index-%1-%2.sqlite").arg(QCoreApplication::applicationPid()).arg(sessionId));
}

void AiServices::guardAgentTurn() {
  engine_.wrap(q("agent.turn"), [this](engine::Handler inner) -> engine::Handler {
    return [this, inner](engine::CallContext& c, const QJsonObject& a) -> engine::Result {
      const QJsonObject details{{q("endpoint"), options_.llm.endpoint}, {q("model"), options_.llm.model}};
      if (!configError_.isEmpty()) return engine::Result::failure(q("unavailable"), configError_, details);
      std::optional<agent::AgentMode> wanted;
      if (a.contains(q("mode"))) wanted = agent::parseAgentMode(a.value(q("mode")).toString());
      const agent::AgentMode mode = wanted.value_or(harness_->mode());
      // plan mode only plans (no model call), everything else needs the model: fail fast instead of waiting out a timeout
      QString why;
      if (mode != agent::AgentMode::Plan && !llm_->reachable(&why)) return engine::Result::failure(q("unavailable"), why, details);
      llm_->clearError();
      engine::Result r = inner(c, a);
      if (r.ok && !llm_->lastError().isEmpty())
        return engine::Result::failure(q("unavailable"), QStringLiteral("The model did not answer: %1").arg(llm_->lastError()), details);
      return r;
    };
  });
}

engine::Session* AiServices::sessionFor(engine::CallContext& ctx, const QJsonObject& args) {
  return ctx.session ? ctx.session : engine_.session(args.value(q("session")).toString());
}

void AiServices::bindBranches(engine::Session* s) {
  editor::Project* p = s ? s->project : nullptr;
  if (p == branchProject_) return;
  branches_.detachProject();
  branchProject_ = p;
  if (!p) return;
  branches_.attachProject(p);
  if (watched_.insert(p).second) {
    // a project opened into the same object replaces the document: the branches must be read again
    connect(p, &editor::Project::loaded, this, [this, p] {
      if (branchProject_ != p) return;
      branches_.detachProject();
      branchProject_ = nullptr;
    });
    connect(p, &QObject::destroyed, this, [this, p] {
      watched_.erase(p);
      if (branchProject_ == p) {
        branches_.detachProject();
        branchProject_ = nullptr;
      }
    });
  }
}

void AiServices::ensureCatalog(engine::Session* s, bool create) {
  const bool unsaved = !s || !s->project || s->project->path().isEmpty();
  const QString path = options_.catalogPath.isEmpty() ? catalogPathFor(unsaved ? QString() : s->project->path(), s ? s->id : QStringLiteral("none")) : options_.catalogPath;
  if (path == openCatalog_ && index_.catalog()->isOpen()) return;
  if (!create && !QFileInfo::exists(path)) return; // reads (copilot lint) use a catalog that is already there, they don't make one
  index_.closeCatalog();
  if (catalogIsTemp_ && !openCatalog_.isEmpty()) QFile::remove(openCatalog_);
  catalogIsTemp_ = options_.catalogPath.isEmpty() && unsaved;
  openCatalog_ = index_.openCatalog(path) ? path : QString();
}

void AiServices::lazyBindings() {
  QStringList names;
  for (const auto& c : engine_.commands())
    if (c->name.startsWith(q("index.")) || c->name.startsWith(q("copilot.")) || c->name.startsWith(q("branch."))) names << c->name;
  for (const QString& name : names) {
    const bool isIndex = name.startsWith(q("index."));
    const bool isCopilot = name.startsWith(q("copilot."));
    const bool isBranch = name.startsWith(q("branch."));
    engine_.wrap(name, [this, isIndex, isCopilot, isBranch](engine::Handler inner) -> engine::Handler {
      return [this, inner, isIndex, isCopilot, isBranch](engine::CallContext& ctx, const QJsonObject& args) -> engine::Result {
        engine::Session* s = sessionFor(ctx, args);
        if (isBranch || isCopilot) bindBranches(s);
        if (isIndex || isCopilot) ensureCatalog(s, isIndex);
        return inner(ctx, args);
      };
    });
  }
}

engine::EngineSetup engineSetup(const AiOptions& options) {
  return [options](engine::Engine& e) -> std::shared_ptr<void> { return std::make_shared<AiServices>(e, options); };
}

} // namespace sf::ai
