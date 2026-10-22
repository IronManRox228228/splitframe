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
    bus_->setDefaultHandler(kind, [ptr, this](const PeerRequest& req) {
      return ptr->handlePeerRequest(req, lastSessionId_);
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

QJsonArray AgentHarness::applyProposed(Specialist* sp, const QJsonArray& proposed, const QString& sessionId) {
  QJsonArray results;
  if (!sp) return results;
  for (const auto& v : proposed) {
    const QJsonObject p = v.toObject();
    const QString fnName = p.value(QStringLiteral("name")).toString();
    const QJsonObject fnArgs = p.value(QStringLiteral("arguments")).toObject();
    QJsonObject toolResult;
    if (fnName == QStringLiteral("peer_request")) {
      SpecialistKind target = parseSpecialistKind(fnArgs.value(QStringLiteral("to")).toString()).value_or(SpecialistKind::Editor);
      PeerResponse pr = sp->sendPeerRequest(
        target,
        fnArgs.value(QStringLiteral("kind")).toString(),
        fnArgs.value(QStringLiteral("args")).toObject(),
        fnArgs.value(QStringLiteral("constraints")).toObject(),
        fnArgs.value(QStringLiteral("reason")).toString(),
        fnArgs.value(QStringLiteral("await")).toBool(true)
      );
      toolResult = pr.toJson();
    } else {
      toolResult = sp->executeCommand(fnName, fnArgs, sessionId).toJson();
    }
    results.append(QJsonObject{
      {QStringLiteral("call_id"), p.value(QStringLiteral("call_id"))},
      {QStringLiteral("name"), fnName},
      {QStringLiteral("arguments"), fnArgs},
      {QStringLiteral("result"), toolResult}
    });
  }
  return results;
}

QJsonObject AgentHarness::skipPending() {
  pending_ = {};
  return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("skipped"), true}};
}

QJsonObject AgentHarness::runPending(const QString& sessionId) {
  QJsonObject out;
  out.insert(QStringLiteral("ok"), true);
  if (pending_.kind == PendingKind::None) {
    out.insert(QStringLiteral("ok"), false);
    out.insert(QStringLiteral("error"), QStringLiteral("no_pending_plan"));
    return out;
  }

  const QString sess = !sessionId.isEmpty() ? sessionId : pending_.sessionId;
  lastSessionId_ = sess;

  if (pending_.kind == PendingKind::Ask) {
    Specialist* sp = specialist(pending_.specialist);
    QJsonObject groupArgs;
    if (!sess.isEmpty()) groupArgs.insert(QStringLiteral("session"), sess);
    groupArgs.insert(QStringLiteral("label"), QStringLiteral("Agent: %1").arg(pending_.message));
    if (engine_) engine_->call(QStringLiteral("session.group.begin"), groupArgs);
    QJsonArray toolResults = applyProposed(sp, pending_.proposed, sess);
    if (engine_) engine_->call(QStringLiteral("session.group.end"), groupArgs);
    pending_ = {};
    out.insert(QStringLiteral("applied"), true);
    out.insert(QStringLiteral("toolResults"), toolResults);
    out.insert(QStringLiteral("teamThread"), bus_->teamThreadJson());
    emit turnCompleted(out);
    return out;
  }

  const QString message = pending_.message;
  pending_ = {};
  const AgentMode saved = mode_;
  mode_ = AgentMode::Auto;
  out = executeTurn(message, sess);
  mode_ = saved;
  return out;
}

QJsonObject AgentHarness::executeTurn(const QString& userMessage, const QString& sessionId) {
  lastSessionId_ = sessionId;
  RouteResult route = router_->route(userMessage);

  Specialist* primary = specialist(route.primary);
  if (!primary) primary = specialist(SpecialistKind::Editor);

  QJsonObject out;
  out.insert(QStringLiteral("ok"), true);
  out.insert(QStringLiteral("mode"), modeName(mode_));
  out.insert(QStringLiteral("route"), route.toJson());

  // 1. Question handling: pure inquiry, no mutations or undo groups
  if (route.isQuestion) {
    QJsonObject res = primary->processTurn(userMessage, sessionId, TurnPolicy::Execute);
    out.insert(QStringLiteral("response"), res.value(QStringLiteral("content")));
    out.insert(QStringLiteral("toolResults"), res.value(QStringLiteral("tool_results")));
    out.insert(QStringLiteral("status"), res.value(QStringLiteral("status")));
    if (res.contains(QStringLiteral("error"))) out.insert(QStringLiteral("error"), res.value(QStringLiteral("error")));
    out.insert(QStringLiteral("teamThread"), bus_->teamThreadJson());
    emit turnCompleted(out);
    return out;
  }

  // 2. Plan mode or Default mode on compound jobs: generate plan card without mutating
  if (mode_ == AgentMode::Plan || (mode_ == AgentMode::Default && route.needsPlan)) {
    pending_ = PendingTurn{
      .kind = PendingKind::Plan,
      .message = userMessage,
      .sessionId = sessionId,
      .specialist = route.primary
    };
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
    out.insert(QStringLiteral("response"), QStringLiteral("Plan created. Call agent.plan.run to execute."));
    out.insert(QStringLiteral("teamThread"), bus_->teamThreadJson());
    emit turnCompleted(out);
    return out;
  }

  // 3. Ask mode: LLM may propose mutations but they are held until agent.plan.run
  if (mode_ == AgentMode::Ask) {
    QJsonObject turnRes = primary->processTurn(userMessage, sessionId, TurnPolicy::Propose);
    pending_ = PendingTurn{
      .kind = PendingKind::Ask,
      .message = userMessage,
      .sessionId = sessionId,
      .specialist = primary->kind(),
      .proposed = turnRes.value(QStringLiteral("proposed")).toArray()
    };
    out.insert(QStringLiteral("pendingApproval"), true);
    out.insert(QStringLiteral("proposed"), pending_.proposed);
    out.insert(QStringLiteral("response"), turnRes.value(QStringLiteral("content")).toString().isEmpty()
                 ? QStringLiteral("Proposed %1 change(s). Call agent.plan.run to apply, or agent.plan.skip to discard.").arg(pending_.proposed.size())
                 : turnRes.value(QStringLiteral("content")));
    out.insert(QStringLiteral("toolResults"), turnRes.value(QStringLiteral("tool_results")));
    out.insert(QStringLiteral("status"), turnRes.value(QStringLiteral("status")));
    out.insert(QStringLiteral("teamThread"), bus_->teamThreadJson());
    emit turnCompleted(out);
    return out;
  }

  // 4. Execution: wrap in undo group
  QJsonObject groupArgs;
  if (!sessionId.isEmpty()) groupArgs.insert(QStringLiteral("session"), sessionId);
  groupArgs.insert(QStringLiteral("label"), QStringLiteral("Agent: %1").arg(userMessage));

  if (engine_) {
    engine_->call(QStringLiteral("session.group.begin"), groupArgs);
  }

  QJsonObject turnRes = primary->processTurn(userMessage, sessionId, TurnPolicy::Execute);

  if (engine_) {
    engine_->call(QStringLiteral("session.group.end"), groupArgs);
  }

  out.insert(QStringLiteral("response"), turnRes.value(QStringLiteral("content")));
  out.insert(QStringLiteral("toolResults"), turnRes.value(QStringLiteral("tool_results")));
  out.insert(QStringLiteral("status"), turnRes.value(QStringLiteral("status")));
  if (turnRes.contains(QStringLiteral("error"))) out.insert(QStringLiteral("error"), turnRes.value(QStringLiteral("error")));
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

  engine.add(engine::CommandSpec{
    .name = QStringLiteral("agent.plan.run"),
    .alias = QStringLiteral("runAgentPlan"),
    .description = QStringLiteral("Execute the last held plan or Ask-mode proposals."),
    .domain = QStringLiteral("engine"),
    .mutates = true,
    .needsSession = false,
    .inputSchema = QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}},
    .handler = [this](engine::CallContext& c, const QJsonObject&) {
      QString sessId = c.session ? c.session->id : QString();
      QJsonObject res = runPending(sessId);
      if (!res.value(QStringLiteral("ok")).toBool()) {
        return engine::Result::failure(res.value(QStringLiteral("error")).toString(), QStringLiteral("No pending plan or Ask proposals to run."));
      }
      return engine::Result::success(res);
    }
  });

  engine.add(engine::CommandSpec{
    .name = QStringLiteral("agent.plan.skip"),
    .alias = QStringLiteral("skipAgentPlan"),
    .description = QStringLiteral("Discard the last held plan or Ask-mode proposals."),
    .domain = QStringLiteral("engine"),
    .mutates = false,
    .needsSession = false,
    .inputSchema = QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}},
    .handler = [this](engine::CallContext&, const QJsonObject&) {
      return engine::Result::success(skipPending());
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
