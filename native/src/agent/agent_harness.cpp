#include "agent/agent_harness.h"
#include "agent/trace.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QUuid>

#include <optional>

namespace sf::agent {

AgentHarness::AgentHarness(engine::Engine* engine,
                           std::shared_ptr<LlmClient> llm,
                           QObject* parent)
  : QObject(parent), engine_(engine), llm_(std::move(llm)) {
  bus_ = std::make_unique<PeerBus>(this);
  router_ = std::make_unique<Router>();

  // Create the four specialists (Editor slot 0, Colourist slot 1, Audio slot 2, Director slot 3)
  specialists_[SpecialistKind::Editor] = std::make_unique<Specialist>(SpecialistKind::Editor, engine_, bus_.get(), llm_, this);
  specialists_[SpecialistKind::Editor]->setSlotId(0);
  specialists_[SpecialistKind::Colourist] = std::make_unique<Specialist>(SpecialistKind::Colourist, engine_, bus_.get(), llm_, this);
  specialists_[SpecialistKind::Colourist]->setSlotId(1);
  specialists_[SpecialistKind::Audio] = std::make_unique<Specialist>(SpecialistKind::Audio, engine_, bus_.get(), llm_, this);
  specialists_[SpecialistKind::Audio]->setSlotId(2);
  specialists_[SpecialistKind::Director] = std::make_unique<Specialist>(SpecialistKind::Director, engine_, bus_.get(), llm_, this);
  specialists_[SpecialistKind::Director]->setSlotId(3);

  // Set default peer handlers on bus to delegate to recipient specialists, and forward agent events
  for (auto& [kind, sp] : specialists_) {
    Specialist* ptr = sp.get();
    bus_->setDefaultHandler(kind, [ptr, this](const PeerRequest& req) {
      QString sid = req.args.value(QStringLiteral("session")).toString();
      if (sid.isEmpty()) sid = lastSessionId_;
      return ptr->handlePeerRequest(req, sid);
    });
    connect(ptr, &Specialist::agentEvent, this, &AgentHarness::agentEvent);
    ptr->setCancelFlag(&cancel_);
  }
  if (llm_) llm_->setCancelFlag(&cancel_);
}

namespace {

// An agent turn's undo group, closed on every way out (return, exception, provider error, Stop): the destructor ends it, which
// commits what was done as one undo step and releases the project lock.
class AgentGroupScope {
public:
  AgentGroupScope(engine::Engine* engine, QJsonObject args) : engine_(engine), args_(std::move(args)) {
    if (!engine_) return;
    const engine::Result r = engine_->call(QStringLiteral("session.group.begin"), args_);
    opened_ = r.ok;
    if (!r.ok) refusal_ = r;
  }
  ~AgentGroupScope() {
    if (opened_) engine_->call(QStringLiteral("session.group.end"), args_);
  }
  AgentGroupScope(const AgentGroupScope&) = delete;
  AgentGroupScope& operator=(const AgentGroupScope&) = delete;
  // True when another agent holds the project (nothing may run), false for no engine / no session (runs ungrouped as before).
  bool busy() const { return refusal_ && refusal_->error.code == QLatin1String("agent_busy"); }
  QString busyMessage() const { return refusal_ ? refusal_->error.message : QString(); }

private:
  engine::Engine* engine_ = nullptr;
  QJsonObject args_;
  bool opened_ = false;
  std::optional<engine::Result> refusal_;
};

QJsonObject busyTurn(const AgentGroupScope& g) {
  return QJsonObject{{QStringLiteral("ok"), false},
                     {QStringLiteral("status"), QStringLiteral("error")},
                     {QStringLiteral("error"), QStringLiteral("agent_busy")},
                     {QStringLiteral("response"), g.busyMessage()}};
}

} // namespace

void AgentHarness::setLlmClient(std::shared_ptr<LlmClient> llm) {
  llm_ = std::move(llm);
  if (llm_) llm_->setCancelFlag(&cancel_);
  for (auto& [kind, sp] : specialists_) {
    sp->setLlmClient(llm_);
  }
}

Specialist* AgentHarness::specialist(SpecialistKind kind) const {
  auto it = specialists_.find(kind);
  return it != specialists_.end() ? it->second.get() : nullptr;
}

QString AgentHarness::currentProjectDir() const {
  if (!engine_) return QString();
  engine::Session* s = engine_->session(lastSessionId_);
  if (s && s->project && !s->project->path().isEmpty()) {
    return QFileInfo(s->project->path()).absolutePath();
  }
  return QString();
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
      const QString toStr = fnArgs.value(QStringLiteral("to")).toString();
      auto optTarget = parseSpecialistKind(toStr);
      if (!optTarget) {
        toolResult = QJsonObject{
          {QStringLiteral("ok"), false},
          {QStringLiteral("error"), QStringLiteral("invalid_params")},
          {QStringLiteral("message"), QStringLiteral("Invalid target '%1'. Valid targets are: director, editor, colourist, audio.").arg(toStr)}
        };
      } else {
        PeerResponse pr = sp->sendPeerRequest(
          *optTarget,
          fnArgs.value(QStringLiteral("kind")).toString(),
          fnArgs.value(QStringLiteral("args")).toObject(),
          fnArgs.value(QStringLiteral("constraints")).toObject(),
          fnArgs.value(QStringLiteral("reason")).toString(),
          fnArgs.value(QStringLiteral("await")).toBool(true)
        );
        toolResult = pr.toJson();
      }
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
  const QString projDir = currentProjectDir();
  logAgentTrace(AgentTraceRecord{
    .runId = QUuid::createUuid().toString(QUuid::WithoutBraces),
    .agentName = QStringLiteral("harness"),
    .role = QStringLiteral("coordinator"),
    .action = QStringLiteral("critique"),
    .inputSummary = pending_.message.isEmpty() ? QStringLiteral("skipPending") : pending_.message,
    .outputSummary = QStringLiteral("Pending plan skipped/discarded"),
    .model = llm_ ? llm_->lastObservedModel() : QString(),
    .status = QStringLiteral("needs_revision"),
    .retryCount = 0
  }, projDir);
  pending_ = {};
  return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("skipped"), true}};
}

QJsonObject AgentHarness::runPending(const QString& sessionId) {
  QJsonObject out;
  out.insert(QStringLiteral("ok"), true);

  const QString sess = !sessionId.isEmpty() ? sessionId : pending_.sessionId;
  lastSessionId_ = sess;
  const QString runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const QString projDir = currentProjectDir();
  bus_->setCurrentRun(runId, projDir);
  for (auto& [k, sp] : specialists_) {
    sp->setCurrentRun(runId, projDir);
    sp->setPeerReviewEnabled(true);
  }

  if (pending_.kind == PendingKind::None) {
    out.insert(QStringLiteral("ok"), false);
    out.insert(QStringLiteral("error"), QStringLiteral("no_pending_plan"));
    logAgentTrace(AgentTraceRecord{
      .runId = runId,
      .agentName = QStringLiteral("harness"),
      .role = QStringLiteral("coordinator"),
      .action = QStringLiteral("error"),
      .inputSummary = QStringLiteral("runPending"),
      .outputSummary = QStringLiteral("no pending plan"),
      .model = llm_ ? llm_->lastObservedModel() : QString(),
      .status = QStringLiteral("error"),
      .retryCount = 0,
      .error = QStringLiteral("no_pending_plan")
    }, projDir);
    return out;
  }

  if (pending_.kind == PendingKind::Ask) {
    Specialist* sp = specialist(pending_.specialist);
    QJsonObject groupArgs;
    if (!sess.isEmpty()) groupArgs.insert(QStringLiteral("session"), sess);
    groupArgs.insert(QStringLiteral("label"), QStringLiteral("Agent: %1").arg(pending_.message));
    groupArgs.insert(QStringLiteral("actor"), QStringLiteral("agent:%1").arg(sp ? sp->name() : QStringLiteral("agent")));
    QJsonArray toolResults;
    {
      AgentGroupScope group(engine_, groupArgs);
      if (group.busy()) return busyTurn(group);
      cancel_.store(false);
      toolResults = applyProposed(sp, pending_.proposed, sess);
    }

    logAgentTrace(AgentTraceRecord{
      .runId = runId,
      .agentName = QStringLiteral("harness"),
      .role = QStringLiteral("coordinator"),
      .action = QStringLiteral("tool_call"),
      .inputSummary = pending_.message,
      .outputSummary = QStringLiteral("Applied %1 proposed change(s)").arg(toolResults.size()),
      .model = llm_ ? llm_->lastObservedModel() : QString(),
      .status = QStringLiteral("success"),
      .retryCount = 0
    }, projDir);

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
  const QString runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const QString projDir = currentProjectDir();
  bus_->setCurrentRun(runId, projDir);
  for (auto& [k, sp] : specialists_) sp->setCurrentRun(runId, projDir);

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
    if (res.contains(QStringLiteral("errorMessage"))) out.insert(QStringLiteral("errorMessage"), res.value(QStringLiteral("errorMessage")));
    out.insert(QStringLiteral("teamThread"), bus_->teamThreadJson());

    const QString resStatus = res.value(QStringLiteral("status")).toString();
    if (resStatus == QStringLiteral("error") || res.contains(QStringLiteral("error"))) {
      logAgentTrace(AgentTraceRecord{
        .runId = runId,
        .agentName = primary ? primary->name() : QStringLiteral("harness"),
        .role = primary ? primary->domain() : QStringLiteral("coordinator"),
        .action = QStringLiteral("error"),
        .inputSummary = userMessage,
        .outputSummary = res.value(QStringLiteral("content")).toString(),
        .model = llm_ ? llm_->lastObservedModel() : QString(),
        .status = QStringLiteral("error"),
        .retryCount = 0,
        .error = res.value(QStringLiteral("error")).toString()
      }, projDir);
    } else {
      logAgentTrace(AgentTraceRecord{
        .runId = runId,
        .agentName = primary ? primary->name() : QStringLiteral("harness"),
        .role = primary ? primary->domain() : QStringLiteral("coordinator"),
        .action = QStringLiteral("message"),
        .inputSummary = userMessage,
        .outputSummary = res.value(QStringLiteral("content")).toString(),
        .model = llm_ ? llm_->lastObservedModel() : QString(),
        .status = QStringLiteral("success"),
        .retryCount = 0
      }, projDir);
    }

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

    logAgentTrace(AgentTraceRecord{
      .runId = runId,
      .agentName = QStringLiteral("harness"),
      .role = QStringLiteral("coordinator"),
      .action = QStringLiteral("plan"),
      .inputSummary = userMessage,
      .outputSummary = QStringLiteral("Plan created with %1 steps").arg(steps.size()),
      .model = llm_ ? llm_->lastObservedModel() : QString(),
      .status = QStringLiteral("success"),
      .retryCount = 0
    }, projDir);

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
    if (turnRes.contains(QStringLiteral("error"))) out.insert(QStringLiteral("error"), turnRes.value(QStringLiteral("error")));
    if (turnRes.contains(QStringLiteral("errorMessage"))) out.insert(QStringLiteral("errorMessage"), turnRes.value(QStringLiteral("errorMessage")));
    out.insert(QStringLiteral("teamThread"), bus_->teamThreadJson());

    logAgentTrace(AgentTraceRecord{
      .runId = runId,
      .agentName = QStringLiteral("harness"),
      .role = QStringLiteral("coordinator"),
      .action = QStringLiteral("escalate"),
      .inputSummary = userMessage,
      .outputSummary = QStringLiteral("Proposed %1 change(s) awaiting approval").arg(pending_.proposed.size()),
      .model = llm_ ? llm_->lastObservedModel() : QString(),
      .status = QStringLiteral("needs_revision"),
      .retryCount = 0
    }, projDir);

    emit turnCompleted(out);
    return out;
  }

  // 4. Execution: wrap in undo group
  const QString agentName = primary ? primary->name() : QStringLiteral("agent");
  QString agentCap = agentName;
  if (!agentCap.isEmpty()) agentCap[0] = agentCap[0].toUpper();
  QJsonObject groupArgs;
  if (!sessionId.isEmpty()) groupArgs.insert(QStringLiteral("session"), sessionId);
  groupArgs.insert(QStringLiteral("label"), QStringLiteral("%1: %2").arg(agentCap, userMessage));
  groupArgs.insert(QStringLiteral("actor"), QStringLiteral("agent:%1").arg(agentName));

  QJsonObject turnRes;
  {
    AgentGroupScope group(engine_, groupArgs);
    if (group.busy()) return busyTurn(group);
    cancel_.store(false);
    turnRes = primary->processTurn(userMessage, sessionId, TurnPolicy::Execute);
  }

  out.insert(QStringLiteral("response"), turnRes.value(QStringLiteral("content")));
  out.insert(QStringLiteral("toolResults"), turnRes.value(QStringLiteral("tool_results")));
  out.insert(QStringLiteral("status"), turnRes.value(QStringLiteral("status")));
  if (turnRes.contains(QStringLiteral("error"))) out.insert(QStringLiteral("error"), turnRes.value(QStringLiteral("error")));
  if (turnRes.contains(QStringLiteral("errorMessage"))) out.insert(QStringLiteral("errorMessage"), turnRes.value(QStringLiteral("errorMessage")));
  out.insert(QStringLiteral("teamThread"), bus_->teamThreadJson());

  const QString turnStatus = turnRes.value(QStringLiteral("status")).toString();
  if (turnStatus == QStringLiteral("error") || turnRes.contains(QStringLiteral("error"))) {
    logAgentTrace(AgentTraceRecord{
      .runId = runId,
      .agentName = primary ? primary->name() : QStringLiteral("harness"),
      .role = primary ? primary->domain() : QStringLiteral("coordinator"),
      .action = QStringLiteral("error"),
      .inputSummary = userMessage,
      .outputSummary = turnRes.value(QStringLiteral("content")).toString(),
      .model = llm_ ? llm_->lastObservedModel() : QString(),
      .status = QStringLiteral("error"),
      .retryCount = 0,
      .error = turnRes.value(QStringLiteral("error")).toString()
    }, projDir);
  } else {
    logAgentTrace(AgentTraceRecord{
      .runId = runId,
      .agentName = primary ? primary->name() : QStringLiteral("harness"),
      .role = primary ? primary->domain() : QStringLiteral("coordinator"),
      .action = QStringLiteral("message"),
      .inputSummary = userMessage,
      .outputSummary = turnRes.value(QStringLiteral("content")).toString(),
      .model = llm_ ? llm_->lastObservedModel() : QString(),
      .status = QStringLiteral("success"),
      .retryCount = 0
    }, projDir);
  }

  emit turnCompleted(out);
  return out;
}

QJsonObject AgentHarness::chat(const QString& userMessage, const QString& sessionId) {
  lastSessionId_ = sessionId;
  const QString runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const QString projDir = currentProjectDir();
  bus_->setCurrentRun(runId, projDir);
  for (auto& [k, sp] : specialists_) {
    sp->setCurrentRun(runId, projDir);
    sp->setPeerReviewEnabled(true);
  }

  Specialist* director = specialist(SpecialistKind::Director);
  if (!director) director = specialist(SpecialistKind::Editor);

  const QString dirName = director ? director->name() : QStringLiteral("director");
  QString dirCap = dirName;
  if (!dirCap.isEmpty()) dirCap[0] = dirCap[0].toUpper();
  QJsonObject groupArgs;
  if (!sessionId.isEmpty()) groupArgs.insert(QStringLiteral("session"), sessionId);
  groupArgs.insert(QStringLiteral("label"), QStringLiteral("%1: %2").arg(dirCap, userMessage));
  groupArgs.insert(QStringLiteral("actor"), QStringLiteral("agent:%1").arg(dirName));

  QJsonObject turnRes;
  {
    AgentGroupScope group(engine_, groupArgs);
    if (group.busy()) return busyTurn(group);
    cancel_.store(false);
    turnRes = director->processTurn(userMessage, sessionId, TurnPolicy::Execute, Lane::Deep);
  }

  QJsonObject out;
  out.insert(QStringLiteral("ok"), turnRes.value(QStringLiteral("status")).toString() != QStringLiteral("error"));
  out.insert(QStringLiteral("response"), turnRes.value(QStringLiteral("content")));
  out.insert(QStringLiteral("toolResults"), turnRes.value(QStringLiteral("tool_results")));
  out.insert(QStringLiteral("status"), turnRes.value(QStringLiteral("status")));
  if (turnRes.contains(QStringLiteral("error"))) out.insert(QStringLiteral("error"), turnRes.value(QStringLiteral("error")));
  if (turnRes.contains(QStringLiteral("errorMessage"))) out.insert(QStringLiteral("errorMessage"), turnRes.value(QStringLiteral("errorMessage")));
  out.insert(QStringLiteral("teamThread"), bus_->teamThreadJson());

  emit turnCompleted(out);
  return out;
}

void AgentHarness::registerCommands(engine::Engine& engine) {
  // agent.chat: Send chat message to Director (Part C item 7)
  engine.add(engine::CommandSpec{
    .name = QStringLiteral("agent.chat"),
    .alias = QStringLiteral("agentChat"),
    .description = QStringLiteral("Chat with the Director agent. Director plans, executes, or assigns work and streams progress events."),
    .domain = QStringLiteral("engine"),
    .mutates = true,
    .needsSession = false,
    .inputSchema = QJsonObject{
      {QStringLiteral("type"), QStringLiteral("object")},
      {QStringLiteral("properties"), QJsonObject{
        {QStringLiteral("message"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
      }},
      {QStringLiteral("required"), QJsonArray{QStringLiteral("message")}}
    },
    .handler = [this](engine::CallContext& c, const QJsonObject& a) {
      QString sessId = c.session ? c.session->id : QString();
      QJsonObject res = chat(a.value(QStringLiteral("message")).toString(), sessId);
      return engine::Result::success(res);
    }
  });

  // agent.assign: Assign work to a specialist (Part C item 3)
  engine.add(engine::CommandSpec{
    .name = QStringLiteral("agent.assign"),
    .alias = QStringLiteral("agentAssign"),
    .description = QStringLiteral("Assign a task to a specialist agent (director, editor, colourist, audio) via the peer bus."),
    .domain = QStringLiteral("engine"),
    .mutates = true,
    .needsSession = false,
    .inputSchema = QJsonObject{
      {QStringLiteral("type"), QStringLiteral("object")},
      {QStringLiteral("properties"), QJsonObject{
        {QStringLiteral("to"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("enum"), QJsonArray{QStringLiteral("director"), QStringLiteral("editor"), QStringLiteral("colourist"), QStringLiteral("audio")}}}},
        {QStringLiteral("task"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("sceneId"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
      }},
      {QStringLiteral("required"), QJsonArray{QStringLiteral("to"), QStringLiteral("task")}}
    },
    .handler = [this](engine::CallContext& c, const QJsonObject& a) {
      const QString toStr = a.value(QStringLiteral("to")).toString();
      auto optTarget = parseSpecialistKind(toStr);
      if (!optTarget) {
        return engine::Result::failure(
          QStringLiteral("invalid_params"),
          QStringLiteral("Invalid target '%1' for agent.assign. Valid targets are: director, editor, colourist, audio.").arg(toStr)
        );
      }
      SpecialistKind target = *optTarget;
      QString sessId = c.session ? c.session->id : QString();
      if (!sessId.isEmpty()) lastSessionId_ = sessId;

      QJsonObject requestArgs = a;
      if (!sessId.isEmpty() && !requestArgs.contains(QStringLiteral("session"))) {
        requestArgs.insert(QStringLiteral("session"), sessId);
      }

      const QString targetName = specialistName(target);
      QString targetCap = targetName;
      if (!targetCap.isEmpty()) targetCap[0] = targetCap[0].toUpper();
      const QString task = a.value(QStringLiteral("task")).toString();
      QJsonObject groupArgs;
      if (!sessId.isEmpty()) groupArgs.insert(QStringLiteral("session"), sessId);
      groupArgs.insert(QStringLiteral("label"), QStringLiteral("%1: %2").arg(targetCap, task));
      groupArgs.insert(QStringLiteral("actor"), QStringLiteral("agent:%1").arg(targetName));

      PeerResponse pr;
      {
        AgentGroupScope group(engine_, groupArgs);
        if (group.busy()) return engine::Result::failure(QStringLiteral("agent_busy"), group.busyMessage());
        Specialist* dir = specialist(SpecialistKind::Director);
        if (dir) {
          pr = dir->sendPeerRequest(target, QStringLiteral("assign"), requestArgs, {}, task, true);
        } else {
          pr = bus_->send(PeerRequest{
            .from = SpecialistKind::Director,
            .to = target,
            .kind = QStringLiteral("assign"),
            .args = requestArgs,
            .reason = task,
            .await = true
          });
        }
      }

      return engine::Result::success(pr.toJson());
    }
  });

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
    .description = QStringLiteral("List the specialist agents with their domain ownership and slot IDs."),
    .domain = QStringLiteral("engine"),
    .mutates = false,
    .needsSession = false,
    .inputSchema = QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}},
    .handler = [this](engine::CallContext&, const QJsonObject& a) {
      QJsonArray list;
      const bool includeDirector = a.value(QStringLiteral("all")).toBool(false);
      for (const auto& [kind, sp] : specialists_) {
        if (!includeDirector && kind == SpecialistKind::Director) continue;
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

  // agent.trace: Read back the last N lines from the agent trace log
  engine.add(engine::CommandSpec{
    .name = QStringLiteral("agent.trace"),
    .alias = QStringLiteral("agentTrace"),
    .description = QStringLiteral("Read back the last N lines from the agent trace log (SF_AGENT_TRACE or agent-trace.jsonl)."),
    .domain = QStringLiteral("engine"),
    .mutates = false,
    .needsSession = false,
    .inputSchema = QJsonObject{
      {QStringLiteral("type"), QStringLiteral("object")},
      {QStringLiteral("properties"), QJsonObject{
        {QStringLiteral("limit"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
      }}
    },
    .handler = [this](engine::CallContext& c, const QJsonObject& a) {
      int limit = a.contains(QStringLiteral("limit")) ? a.value(QStringLiteral("limit")).toInt() : 50;
      QString projectDir;
      if (c.session && c.session->project && !c.session->project->path().isEmpty()) {
        projectDir = QFileInfo(c.session->project->path()).absolutePath();
      } else {
        projectDir = currentProjectDir();
      }
      return engine::Result::success(readAgentTrace(limit, projectDir));
    }
  });
}

} // namespace sf::agent
