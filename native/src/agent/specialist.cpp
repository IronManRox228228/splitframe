#include "agent/specialist.h"
#include "agent/trace.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

namespace sf::agent {

Specialist::Specialist(SpecialistKind kind,
                       engine::Engine* engine,
                       PeerBus* bus,
                       std::shared_ptr<LlmClient> llm,
                       QObject* parent)
  : QObject(parent), kind_(kind), engine_(engine), bus_(bus), llm_(std::move(llm)) {
  // Set default warm slot IDs per specialist (0=editor, 1=colourist, 2=audio)
  switch (kind_) {
    case SpecialistKind::Editor: slotId_ = 0; break;
    case SpecialistKind::Colourist: slotId_ = 1; break;
    case SpecialistKind::Audio: slotId_ = 2; break;
  }
}

QString Specialist::domain() const {
  switch (kind_) {
    case SpecialistKind::Editor: return QStringLiteral("editor");
    case SpecialistKind::Colourist: return QStringLiteral("colour");
    case SpecialistKind::Audio: return QStringLiteral("audio");
  }
  return QStringLiteral("engine");
}

QString Specialist::systemPrompt() const {
  switch (kind_) {
    case SpecialistKind::Editor:
      return QStringLiteral(
        "You are the SplitFrame Video Editor specialist.\n"
        "You own timeline structure: cuts, trims, clip ordering, ripple editing, captions, titles, canvas format, and export.\n"
        "You collaborate with the Colourist and Audio specialists via the Peer Bus.\n"
        "You DO NOT directly modify mixer nodes or grades; ask the Audio or Colourist specialist instead."
      );
    case SpecialistKind::Colourist:
      return QStringLiteral(
        "You are the SplitFrame Colourist specialist.\n"
        "You own colour grading, LUTs, exposure, saturation, lift/gamma/gain, and OpenColorIO colour spaces.\n"
        "You collaborate with the Video Editor and Audio specialists via the Peer Bus.\n"
        "You DO NOT cut or move timeline clips directly; ask the Video Editor instead."
      );
    case SpecialistKind::Audio:
      return QStringLiteral(
        "You are the SplitFrame Audio specialist.\n"
        "You own audio tracks, background music, ducking, volume/pan, audio effects, CLAP plugins, and beat synchronization.\n"
        "You collaborate with the Video Editor and Colourist specialists via the Peer Bus.\n"
        "You DO NOT change timeline clip edits directly; ask the Video Editor instead."
      );
  }
  return {};
}

QJsonObject Specialist::scopedViewJson(const QString& sessionId) const {
  if (!engine_) return {};

  QJsonObject view;
  QJsonObject sessArg;
  if (!sessionId.isEmpty()) sessArg.insert(QStringLiteral("session"), sessionId);

  switch (kind_) {
    case SpecialistKind::Editor: {
      QJsonObject tlArg = sessArg;
      tlArg.insert(QStringLiteral("mode"), QStringLiteral("summary"));
      engine::Result tlRes = engine_->call(QStringLiteral("timeline.get"), tlArg);
      if (tlRes.ok) view.insert(QStringLiteral("timeline"), tlRes.value);

      engine::Result projRes = engine_->call(QStringLiteral("project.info"), sessArg);
      if (projRes.ok) {
        QJsonObject po = projRes.value.toObject();
        view.insert(QStringLiteral("canvas"), QJsonObject{
          {QStringLiteral("width"), po.value(QStringLiteral("width"))},
          {QStringLiteral("height"), po.value(QStringLiteral("height"))},
          {QStringLiteral("fps"), po.value(QStringLiteral("fps"))},
          {QStringLiteral("durationFrames"), po.value(QStringLiteral("durationFrames"))}
        });
      }
      break;
    }
    case SpecialistKind::Colourist: {
      engine::Result projRes = engine_->call(QStringLiteral("project.info"), sessArg);
      if (projRes.ok) {
        QJsonObject po = projRes.value.toObject();
        QJsonObject cm = po.value(QStringLiteral("colorManagement")).toObject();
        if (cm.isEmpty()) {
          cm.insert(QStringLiteral("blendSpace"), QStringLiteral("linear"));
        }
        view.insert(QStringLiteral("colorManagement"), cm);
      }
      QJsonObject tlArg = sessArg;
      tlArg.insert(QStringLiteral("mode"), QStringLiteral("summary"));
      engine::Result tlRes = engine_->call(QStringLiteral("timeline.get"), tlArg);
      if (tlRes.ok) {
        QJsonArray gradedItems;
        for (const auto& trackVal : tlRes.value.toObject().value(QStringLiteral("tracks")).toArray()) {
          for (const auto& itemVal : trackVal.toObject().value(QStringLiteral("items")).toArray()) {
            QJsonObject itemObj = itemVal.toObject();
            if (itemObj.contains(QStringLiteral("color")) || itemObj.contains(QStringLiteral("props"))) {
              gradedItems.append(itemObj);
            }
          }
        }
        view.insert(QStringLiteral("items"), gradedItems);
      }
      break;
    }
    case SpecialistKind::Audio: {
      engine::Result mxRes = engine_->call(QStringLiteral("mixer.get"), sessArg);
      if (mxRes.ok) view.insert(QStringLiteral("mixer"), mxRes.value.toObject().value(QStringLiteral("mixer")));

      QJsonObject tlArg = sessArg;
      tlArg.insert(QStringLiteral("mode"), QStringLiteral("summary"));
      engine::Result tlRes = engine_->call(QStringLiteral("timeline.get"), tlArg);
      if (tlRes.ok) {
        QJsonArray audioTracks;
        for (const auto& trackVal : tlRes.value.toObject().value(QStringLiteral("tracks")).toArray()) {
          QJsonObject tObj = trackVal.toObject();
          if (tObj.value(QStringLiteral("kind")).toString() == QStringLiteral("audio")) {
            audioTracks.append(tObj);
          }
        }
        view.insert(QStringLiteral("audioTracks"), audioTracks);
      }
      break;
    }
  }

  return view;
}

bool Specialist::allowsCommand(const engine::CommandSpec& spec) const {
  if (spec.domain == domain() || spec.domain == QStringLiteral("engine")) return true;
  if (!spec.mutates) return true;
  if (spec.domain == QStringLiteral("comfy")) return true;
  if (spec.name.startsWith(QStringLiteral("generate.")) || spec.name.startsWith(QStringLiteral("comfy."))) return true;
  return false;
}

QJsonArray Specialist::availableTools() const {
  if (!engine_) return {};

  QJsonArray tools;
  for (const auto& cmd : engine_->commands()) {
    if (allowsCommand(*cmd)) {
      tools.append(QJsonObject{
        {QStringLiteral("name"), cmd->name},
        {QStringLiteral("description"), cmd->description},
        {QStringLiteral("domain"), cmd->domain},
        {QStringLiteral("mutates"), cmd->mutates},
        {QStringLiteral("parameters"), cmd->inputSchema}
      });
    }
  }

  // Peer Bus tool for cross-domain collaboration
  tools.append(QJsonObject{
    {QStringLiteral("name"), QStringLiteral("peer_request")},
    {QStringLiteral("description"), QStringLiteral("Send a typed request to another specialist (editor, colourist, audio) across the Peer Bus.")},
    {QStringLiteral("parameters"), QJsonObject{
      {QStringLiteral("type"), QStringLiteral("object")},
      {QStringLiteral("properties"), QJsonObject{
        {QStringLiteral("to"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("enum"), QJsonArray{QStringLiteral("editor"), QStringLiteral("colourist"), QStringLiteral("audio")}}}},
        {QStringLiteral("kind"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("args"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}},
        {QStringLiteral("constraints"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}},
        {QStringLiteral("reason"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("await"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}}
      }},
      {QStringLiteral("required"), QJsonArray{QStringLiteral("to"), QStringLiteral("kind"), QStringLiteral("reason")}}
    }}
  });

  return tools;
}

engine::Result Specialist::executeCommand(const QString& cmdName,
                                          QJsonObject args,
                                          const QString& sessionId) {
  if (!engine_) {
    return engine::Result::failure(QStringLiteral("no_engine"), QStringLiteral("Engine instance is null"));
  }

  const engine::CommandSpec* spec = engine_->find(cmdName);
  if (!spec) {
    return engine::Result::failure(QStringLiteral("unknown_command"), QStringLiteral("Command '%1' not found").arg(cmdName));
  }

  if (!allowsCommand(*spec)) {
    QString owner = (spec->domain == QStringLiteral("editor")) ? QStringLiteral("Video Editor")
                  : (spec->domain == QStringLiteral("colour")) ? QStringLiteral("Colourist")
                  : (spec->domain == QStringLiteral("audio"))  ? QStringLiteral("Audio")
                  : spec->domain;
    return engine::Result::failure(
      QStringLiteral("domain_forbidden"),
      QStringLiteral("Command '%1' belongs to domain '%2' owned by the %3 specialist. Request this change via the Peer Bus.")
        .arg(cmdName, spec->domain, owner)
    );
  }

  if (!sessionId.isEmpty() && !args.contains(QStringLiteral("session"))) {
    args.insert(QStringLiteral("session"), sessionId);
  }
  if (spec->mutates) {
    args.insert(QStringLiteral("origin"), QStringLiteral("agent:%1").arg(name()));
  }

  return engine_->call(cmdName, args);
}

PeerResponse Specialist::sendPeerRequest(SpecialistKind to,
                                         const QString& kind,
                                         const QJsonObject& args,
                                         const QJsonObject& constraints,
                                         const QString& reason,
                                         bool await) {
  if (!bus_) {
    return PeerResponse{
      .status = QStringLiteral("declined"),
      .note = QStringLiteral("Peer bus is null")
    };
  }

  PeerRequest req{
    .from = kind_,
    .to = to,
    .kind = kind,
    .args = args,
    .constraints = constraints,
    .reason = reason,
    .await = await
  };

  return bus_->send(req);
}

namespace {

QString peerKindToCommand(const QString& kind) {
  if (kind == QStringLiteral("duck_music")) return QStringLiteral("audio.duck");
  if (kind == QStringLiteral("fit_to_beats")) return QStringLiteral("music.fit");
  if (kind == QStringLiteral("grade_clip") || kind == QStringLiteral("set_lut")) return QStringLiteral("item.color.set");
  if (kind == QStringLiteral("match_shots")) return QStringLiteral("colour.match");
  if (kind == QStringLiteral("trim_item")) return QStringLiteral("item.trim");
  if (kind == QStringLiteral("cut_timeline")) return QStringLiteral("item.split");
  return kind;
}

QJsonObject parseToolArgs(const QJsonValue& arguments) {
  if (arguments.isObject()) return arguments.toObject();
  return QJsonDocument::fromJson(arguments.toString().toUtf8()).object();
}

} // namespace

PeerResponse Specialist::handlePeerRequest(const PeerRequest& req, const QString& sessionId) {
  PeerResponse res;
  res.requestId = req.id;

  const QString cmdName = peerKindToCommand(req.kind);
  const engine::CommandSpec* spec = engine_ ? engine_->find(cmdName) : nullptr;
  if (!spec) {
    res.status = QStringLiteral("unsupported");
    res.note = QStringLiteral("%1 has no command for peer kind '%2'").arg(name(), req.kind);
    return res;
  }

  engine::Result cmdRes = executeCommand(cmdName, req.args, sessionId);
  res.outputs = cmdRes.toJson();
  if (cmdRes.ok) {
    res.status = QStringLiteral("done");
    res.note = QStringLiteral("executed %1").arg(cmdName);
  } else if (cmdRes.error.code == QStringLiteral("unknown_command") || cmdRes.error.code == QStringLiteral("domain_forbidden")) {
    res.status = QStringLiteral("unsupported");
    res.note = cmdRes.error.message;
  } else {
    res.status = QStringLiteral("declined");
    res.note = cmdRes.error.message;
  }
  return res;
}

QJsonObject Specialist::processTurn(const QString& userMessage, const QString& sessionId, TurnPolicy policy) {
  QJsonObject result;
  result.insert(QStringLiteral("specialist"), name());

  const QString runId = !currentRunId_.isEmpty() ? currentRunId_ : QUuid::createUuid().toString(QUuid::WithoutBraces);

  addMessage(ChatMessage{
    .role = QStringLiteral("user"),
    .content = userMessage
  });

  if (!llm_) {
    result.insert(QStringLiteral("content"), QString());
    result.insert(QStringLiteral("status"), QStringLiteral("error"));
    result.insert(QStringLiteral("error"), QStringLiteral("no_llm_client"));
    logAgentTrace(AgentTraceRecord{
      .runId = runId,
      .agentName = name(),
      .role = domain(),
      .action = QStringLiteral("error"),
      .inputSummary = userMessage,
      .status = QStringLiteral("error"),
      .retryCount = 0,
      .error = QStringLiteral("no_llm_client")
    }, projectDir_);
    return result;
  }

  QJsonObject scopedView = scopedViewJson(sessionId);
  QString scopedViewStr = QString::fromUtf8(QJsonDocument(scopedView).toJson(QJsonDocument::Compact));
  QString enrichedPrompt = systemPrompt() + QStringLiteral("\n\nCurrent scoped state: ") + scopedViewStr;

  QJsonArray allToolResults;
  QJsonArray proposed;
  QString respContent;
  QString status = QStringLiteral("ok");
  bool hitCap = false;

  for (int round = 0; round < kMaxToolRounds; ++round) {
    if (round > 0) {
      logAgentTrace(AgentTraceRecord{
        .runId = runId,
        .agentName = name(),
        .role = domain(),
        .action = QStringLiteral("retry"),
        .inputSummary = QStringLiteral("Turn round %1").arg(round),
        .model = llm_->lastObservedModel(),
        .status = QStringLiteral("success"),
        .retryCount = round
      }, projectDir_);
    }

    QJsonObject llmResp = llm_->complete(enrichedPrompt, history_, availableTools(), slotId_);
    const QJsonValue errVal = llmResp.value(QStringLiteral("error"));
    QJsonArray toolCalls = llmResp.value(QStringLiteral("tool_calls")).toArray();
    respContent = llmResp.value(QStringLiteral("content")).toString();

    if (!errVal.isUndefined() && !errVal.isNull() && respContent.isEmpty() && toolCalls.isEmpty()) {
      status = QStringLiteral("error");
      const QString code = llmErrorCode(llmResp);
      const QString finalErr = !code.isEmpty() ? code : (errVal.isString() ? errVal.toString() : QStringLiteral("llm_error"));
      result.insert(QStringLiteral("error"), finalErr);
      logAgentTrace(AgentTraceRecord{
        .runId = runId,
        .agentName = name(),
        .role = domain(),
        .action = QStringLiteral("error"),
        .inputSummary = userMessage,
        .model = llm_->lastObservedModel(),
        .status = QStringLiteral("error"),
        .retryCount = round,
        .error = finalErr
      }, projectDir_);
      break;
    }

    addMessage(ChatMessage{
      .role = QStringLiteral("assistant"),
      .content = respContent,
      .toolCalls = toolCalls
    });

    if (toolCalls.isEmpty()) {
      if (!respContent.isEmpty()) {
        logAgentTrace(AgentTraceRecord{
          .runId = runId,
          .agentName = name(),
          .role = domain(),
          .action = QStringLiteral("message"),
          .inputSummary = userMessage,
          .outputSummary = respContent,
          .model = llm_->lastObservedModel(),
          .status = QStringLiteral("success"),
          .retryCount = round
        }, projectDir_);
      }
      break;
    }

    for (const auto& tcVal : toolCalls) {
      QJsonObject tc = tcVal.toObject();
      QString callId = tc.value(QStringLiteral("id")).toString();
      QJsonObject fn = tc.value(QStringLiteral("function")).toObject();
      QString fnName = fn.value(QStringLiteral("name")).toString();
      QJsonObject fnArgs = parseToolArgs(fn.value(QStringLiteral("arguments")));

      QJsonObject toolResult;
      bool mutating = false;
      if (fnName == QStringLiteral("peer_request")) {
        mutating = true;
      } else if (engine_) {
        const engine::CommandSpec* spec = engine_->find(fnName);
        mutating = spec && spec->mutates;
      }

      if (policy == TurnPolicy::Propose && mutating) {
        proposed.append(QJsonObject{
          {QStringLiteral("call_id"), callId},
          {QStringLiteral("name"), fnName},
          {QStringLiteral("arguments"), fnArgs}
        });
        toolResult = QJsonObject{
          {QStringLiteral("ok"), true},
          {QStringLiteral("deferred"), true},
          {QStringLiteral("reason"), QStringLiteral("ask mode: waiting for agent.plan.run")}
        };
      } else if (fnName == QStringLiteral("peer_request")) {
        SpecialistKind target = parseSpecialistKind(fnArgs.value(QStringLiteral("to")).toString()).value_or(SpecialistKind::Editor);
        PeerResponse pr = sendPeerRequest(
          target,
          fnArgs.value(QStringLiteral("kind")).toString(),
          fnArgs.value(QStringLiteral("args")).toObject(),
          fnArgs.value(QStringLiteral("constraints")).toObject(),
          fnArgs.value(QStringLiteral("reason")).toString(),
          fnArgs.value(QStringLiteral("await")).toBool(true)
        );
        toolResult = pr.toJson();
      } else {
        toolResult = executeCommand(fnName, fnArgs, sessionId).toJson();
      }

      const bool isPeer = (fnName == QStringLiteral("peer_request"));
      const QString targetAgent = isPeer ? fnArgs.value(QStringLiteral("to")).toString() : QString();
      const bool ok = toolResult.value(QStringLiteral("ok")).toBool(true);
      const QString toolStatus = (policy == TurnPolicy::Propose && mutating) ? QStringLiteral("needs_revision") : (ok ? QStringLiteral("success") : QStringLiteral("error"));
      logAgentTrace(AgentTraceRecord{
        .runId = runId,
        .agentName = name(),
        .role = domain(),
        .action = QStringLiteral("tool_call"),
        .inputSummary = QString::fromUtf8(QJsonDocument(fnArgs).toJson(QJsonDocument::Compact)),
        .outputSummary = QString::fromUtf8(QJsonDocument(toolResult).toJson(QJsonDocument::Compact)),
        .targetAgent = targetAgent,
        .tool = fnName,
        .model = llm_->lastObservedModel(),
        .status = toolStatus,
        .retryCount = round,
        .error = ok ? QString() : toolResult.value(QStringLiteral("error")).toString()
      }, projectDir_);

      QJsonObject row{
        {QStringLiteral("call_id"), callId},
        {QStringLiteral("name"), fnName},
        {QStringLiteral("arguments"), fnArgs},
        {QStringLiteral("result"), toolResult}
      };
      allToolResults.append(row);

      addMessage(ChatMessage{
        .role = QStringLiteral("tool"),
        .content = QString::fromUtf8(QJsonDocument(toolResult).toJson(QJsonDocument::Compact)),
        .name = fnName,
        .toolCallId = callId
      });
    }

    if (round + 1 == kMaxToolRounds) hitCap = true;
  }

  if (hitCap && status == QStringLiteral("ok")) {
    status = QStringLiteral("error");
    result.insert(QStringLiteral("error"), QStringLiteral("tool_loop_limit"));
    logAgentTrace(AgentTraceRecord{
      .runId = runId,
      .agentName = name(),
      .role = domain(),
      .action = QStringLiteral("error"),
      .inputSummary = userMessage,
      .model = llm_ ? llm_->lastObservedModel() : QString(),
      .status = QStringLiteral("error"),
      .retryCount = kMaxToolRounds,
      .error = QStringLiteral("tool_loop_limit")
    }, projectDir_);
  }

  if (!allToolResults.isEmpty()) result.insert(QStringLiteral("tool_results"), allToolResults);
  if (!proposed.isEmpty()) result.insert(QStringLiteral("proposed"), proposed);
  result.insert(QStringLiteral("content"), respContent);
  result.insert(QStringLiteral("status"), status);
  return result;
}

} // namespace sf::agent
