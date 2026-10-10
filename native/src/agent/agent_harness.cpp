#include "agent/agent_harness.h"

#include <QJsonArray>

namespace sf::agent {

AgentHarness::AgentHarness(engine::Engine* engine,
                           std::shared_ptr<LlmClient> llm,
                           QObject* parent)
  : QObject(parent), engine_(engine), llm_(std::move(llm)) {
  bus_ = std::make_unique<PeerBus>(this);
  router_ = std::make_unique<Router>();

  // Create the three specialists
  specialists_[SpecialistKind::Editor] = std::make_unique<Specialist>(SpecialistKind::Editor, engine_, bus_.get(), llm_, this);
  specialists_[SpecialistKind::Colourist] = std::make_unique<Specialist>(SpecialistKind::Colourist, engine_, bus_.get(), llm_, this);
  specialists_[SpecialistKind::Audio] = std::make_unique<Specialist>(SpecialistKind::Audio, engine_, bus_.get(), llm_, this);

  // Set default peer handlers on bus to delegate to recipient specialists
  for (auto& [kind, sp] : specialists_) {
    Specialist* ptr = sp.get();
    bus_->setDefaultHandler(kind, [ptr](const PeerRequest& req) {
      return ptr->handlePeerRequest(req);
    });
  }
}

void AgentHarness::setLlmClient(std::shared_ptr<LlmClient> llm) {
  llm_ = std::move(llm);
  for (auto& [kind, sp] : specialists_) {
    sp->setLlmClient(llm_);
  }
}

Specialist* AgentHarness::specialist(SpecialistKind kind) const {
  auto it = specialists_.find(kind);
  return it != specialists_.end() ? it->second.get() : nullptr;
}

QJsonObject AgentHarness::executeTurn(const QString& userMessage, const QString& sessionId) {
  RouteResult route = router_->route(userMessage);

  Specialist* primary = specialist(route.primary);
  if (!primary) primary = specialist(SpecialistKind::Editor);

  QJsonObject out;
  out.insert(QStringLiteral("ok"), true);
  out.insert(QStringLiteral("mode"), modeName(mode_));
  out.insert(QStringLiteral("route"), route.toJson());

  // 1. Question handling: pure inquiry, no mutations or undo groups
  if (route.isQuestion) {
    QJsonObject res = primary->processTurn(userMessage, sessionId);
    out.insert(QStringLiteral("response"), res.value(QStringLiteral("content")));
    out.insert(QStringLiteral("toolResults"), res.value(QStringLiteral("tool_results")));
    out.insert(QStringLiteral("teamThread"), bus_->teamThreadJson());
    emit turnCompleted(out);
    return out;
  }

  // 2. Plan mode or Default mode on compound jobs: generate plan card without mutating
  if (mode_ == AgentMode::Plan || (mode_ == AgentMode::Default && route.needsPlan)) {
    out.insert(QStringLiteral("planNeeded"), true);
    QJsonArray steps;
    for (const QString& intent : route.intents) {
      steps.append(QJsonObject{
        {QStringLiteral("intent"), intent},
        {QStringLiteral("specialist"), (intent == QStringLiteral("color")) ? QStringLiteral("colourist")
                                     : (intent == QStringLiteral("music") || intent == QStringLiteral("beats") || intent == QStringLiteral("audio")) ? QStringLiteral("audio")
                                     : QStringLiteral("editor")},
        {QStringLiteral("status"), QStringLiteral("pending")}
      });
    }
    out.insert(QStringLiteral("plan"), QJsonObject{
      {QStringLiteral("goal"), userMessage},
      {QStringLiteral("steps"), steps}
    });
    out.insert(QStringLiteral("response"), QStringLiteral("Plan created. Review and click Run to execute."));
    out.insert(QStringLiteral("teamThread"), bus_->teamThreadJson());
    emit turnCompleted(out);
    return out;
  }

  // 3. Execution: wrap in undo group
  QJsonObject groupArgs;
  if (!sessionId.isEmpty()) groupArgs.insert(QStringLiteral("session"), sessionId);
  groupArgs.insert(QStringLiteral("label"), QStringLiteral("Agent: %1").arg(userMessage));

  if (engine_) {
    engine_->call(QStringLiteral("session.group.begin"), groupArgs);
  }

  QJsonObject turnRes = primary->processTurn(userMessage, sessionId);

  if (engine_) {
    engine_->call(QStringLiteral("session.group.end"), groupArgs);
  }

  out.insert(QStringLiteral("response"), turnRes.value(QStringLiteral("content")));
  out.insert(QStringLiteral("toolResults"), turnRes.value(QStringLiteral("tool_results")));
  out.insert(QStringLiteral("teamThread"), bus_->teamThreadJson());

  emit turnCompleted(out);
  return out;
}

void AgentHarness::registerCommands(engine::Engine& engine) {
  // agent.turn: Execute a user turn
  engine.add(engine::CommandSpec{
    .name = QStringLiteral("agent.turn"),
    .alias = QStringLiteral("agentTurn"),
    .description = QStringLiteral("Send a request to the multi-agent system (router -> specialists -> peer bus)."),
    .domain = QStringLiteral("engine"),
    .mutates = true,
    .needsSession = false,
    .inputSchema = QJsonObject{
      {QStringLiteral("type"), QStringLiteral("object")},
      {QStringLiteral("properties"), QJsonObject{
        {QStringLiteral("message"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("mode"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("enum"), QJsonArray{QStringLiteral("plan"), QStringLiteral("ask"), QStringLiteral("default"), QStringLiteral("auto")}}}}
      }},
      {QStringLiteral("required"), QJsonArray{QStringLiteral("message")}}
    },
    .handler = [this](engine::CallContext& c, const QJsonObject& a) {
      if (a.contains(QStringLiteral("mode"))) {
        auto m = parseAgentMode(a.value(QStringLiteral("mode")).toString());
        if (m) setMode(*m);
      }
      QString sessId = c.session ? c.session->id : QString();
      QJsonObject res = executeTurn(a.value(QStringLiteral("message")).toString(), sessId);
      return engine::Result::success(res);
    }
  });

  // agent.mode.get: Check current agent mode
  engine.add(engine::CommandSpec{
    .name = QStringLiteral("agent.mode.get"),
    .alias = QStringLiteral("getAgentMode"),
    .description = QStringLiteral("Get the current agent mode (plan, ask, default, auto)."),
    .domain = QStringLiteral("engine"),
    .mutates = false,
    .needsSession = false,
    .inputSchema = QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}},
    .handler = [this](engine::CallContext&, const QJsonObject&) {
      return engine::Result::success(QJsonObject{{QStringLiteral("mode"), modeName(mode_)}});
    }
  });

  // agent.mode.set: Update agent mode
  engine.add(engine::CommandSpec{
    .name = QStringLiteral("agent.mode.set"),
    .alias = QStringLiteral("setAgentMode"),
    .description = QStringLiteral("Set the agent mode (plan, ask, default, auto)."),
    .domain = QStringLiteral("engine"),
    .mutates = false,
    .needsSession = false,
    .inputSchema = QJsonObject{
      {QStringLiteral("type"), QStringLiteral("object")},
      {QStringLiteral("properties"), QJsonObject{
        {QStringLiteral("mode"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("enum"), QJsonArray{QStringLiteral("plan"), QStringLiteral("ask"), QStringLiteral("default"), QStringLiteral("auto")}}}}
      }},
      {QStringLiteral("required"), QJsonArray{QStringLiteral("mode")}}
    },
    .handler = [this](engine::CallContext&, const QJsonObject& a) {
      auto m = parseAgentMode(a.value(QStringLiteral("mode")).toString());
      if (!m) {
        return engine::Result::failure(QStringLiteral("invalid_mode"), QStringLiteral("Mode must be plan, ask, default, or auto"));
      }
      setMode(*m);
      return engine::Result::success(QJsonObject{{QStringLiteral("mode"), modeName(mode_)}});
    }
  });

  // agent.team.thread: Query inter-agent messages
  engine.add(engine::CommandSpec{
    .name = QStringLiteral("agent.team.thread"),
    .alias = QStringLiteral("agent.team_thread"),
    .description = QStringLiteral("Retrieve the peer bus inter-agent message log (team thread)."),
    .domain = QStringLiteral("engine"),
    .mutates = false,
    .needsSession = false,
    .inputSchema = QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}},
    .handler = [this](engine::CallContext&, const QJsonObject&) {
      return engine::Result::success(QJsonObject{{QStringLiteral("messages"), bus_->teamThreadJson()}});
    }
  });

  // agent.specialists: List specialists and their metadata
  engine.add(engine::CommandSpec{
    .name = QStringLiteral("agent.specialists"),
    .alias = QStringLiteral("getSpecialists"),
    .description = QStringLiteral("List the three specialist agents (editor, colourist, audio) with their domain ownership and slot IDs."),
    .domain = QStringLiteral("engine"),
    .mutates = false,
    .needsSession = false,
    .inputSchema = QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}},
    .handler = [this](engine::CallContext&, const QJsonObject&) {
      QJsonArray list;
      for (const auto& [kind, sp] : specialists_) {
        list.append(QJsonObject{
          {QStringLiteral("name"), sp->name()},
          {QStringLiteral("domain"), sp->domain()},
          {QStringLiteral("slotId"), sp->slotId()},
          {QStringLiteral("toolCount"), sp->availableTools().size()}
        });
      }
      return engine::Result::success(QJsonObject{{QStringLiteral("specialists"), list}});
    }
  });

  // agent.route: Expose stateless router
  engine.add(engine::CommandSpec{
    .name = QStringLiteral("agent.route"),
    .alias = QStringLiteral("routeRequest"),
    .description = QStringLiteral("Classify a request into intents and primary/secondary specialists without executing it."),
    .domain = QStringLiteral("engine"),
    .mutates = false,
    .needsSession = false,
    .inputSchema = QJsonObject{
      {QStringLiteral("type"), QStringLiteral("object")},
      {QStringLiteral("properties"), QJsonObject{
        {QStringLiteral("message"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
      }},
      {QStringLiteral("required"), QJsonArray{QStringLiteral("message")}}
    },
    .handler = [this](engine::CallContext&, const QJsonObject& a) {
      RouteResult r = router_->route(a.value(QStringLiteral("message")).toString());
      return engine::Result::success(r.toJson());
    }
  });
}

} // namespace sf::agent
