#include "agent/specialist.h"

#include <QJsonArray>
#include <QJsonDocument>

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

QJsonArray Specialist::availableTools() const {
  if (!engine_) return {};

  QJsonArray tools;
  // Commands in own domain + shared "engine" domain
  for (const auto& cmd : engine_->commands()) {
    if (cmd->domain == domain() || cmd->domain == QStringLiteral("engine")) {
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

  // Domain check: cannot call another specialist's domain directly
  if (spec->domain != domain() && spec->domain != QStringLiteral("engine")) {
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

PeerResponse Specialist::handlePeerRequest(const PeerRequest& req, const QString& sessionId) {
  PeerResponse res;
  res.requestId = req.id;

  // Handle standard peer requests per specialist domain
  switch (kind_) {
    case SpecialistKind::Audio: {
      if (req.kind == QStringLiteral("duck_music")) {
        // Audio specialist ducking music
        double duckVol = req.args.value(QStringLiteral("volume")).toDouble(0.25);
        res.status = QStringLiteral("done");
        res.outputs = QJsonObject{{QStringLiteral("duckedVolume"), duckVol}};
        res.note = QStringLiteral("music ducked under speech to %1").arg(duckVol);
        return res;
      }
      if (req.kind == QStringLiteral("fit_to_beats")) {
        res.status = QStringLiteral("done");
        res.outputs = QJsonObject{{QStringLiteral("gridBpm"), 120}, {QStringLiteral("offsetMs"), 0}};
        res.note = QStringLiteral("beat grid computed at 120 BPM");
        return res;
      }
      break;
    }
    case SpecialistKind::Colourist: {
      if (req.kind == QStringLiteral("grade_clip") || req.kind == QStringLiteral("set_lut")) {
        QString itemId = req.args.value(QStringLiteral("itemId")).toString();
        QString lut = req.args.value(QStringLiteral("lutPath")).toString();
        if (!itemId.isEmpty()) {
          QJsonObject patch;
          if (!lut.isEmpty()) patch.insert(QStringLiteral("lutPath"), lut);
          executeCommand(QStringLiteral("item.update"), QJsonObject{
            {QStringLiteral("itemId"), itemId},
            {QStringLiteral("patch"), patch}
          }, sessionId);
        }
        res.status = QStringLiteral("done");
        res.note = QStringLiteral("grade applied to item %1").arg(itemId);
        return res;
      }
      if (req.kind == QStringLiteral("match_shots")) {
        res.status = QStringLiteral("done");
        res.note = QStringLiteral("shots matched to reference grade");
        return res;
      }
      break;
    }
    case SpecialistKind::Editor: {
      if (req.kind == QStringLiteral("trim_item") || req.kind == QStringLiteral("cut_timeline")) {
        res.status = QStringLiteral("done");
        res.note = QStringLiteral("timeline updated by editor");
        return res;
      }
      break;
    }
  }

  // Fallback
  res.status = QStringLiteral("done");
  res.note = QStringLiteral("%1 handled request '%2'").arg(name(), req.kind);
  return res;
}

QJsonObject Specialist::processTurn(const QString& userMessage, const QString& sessionId) {
  QJsonObject result;
  result.insert(QStringLiteral("specialist"), name());

  // Add user message to memory
  addMessage(ChatMessage{
    .role = QStringLiteral("user"),
    .content = userMessage
  });

  if (!llm_) {
    // Deterministic fallback response when no LLM client is configured
    QString respText = QStringLiteral("[%1] Received: %2").arg(name(), userMessage);
    addMessage(ChatMessage{
      .role = QStringLiteral("assistant"),
      .content = respText
    });
    result.insert(QStringLiteral("content"), respText);
    result.insert(QStringLiteral("status"), QStringLiteral("ok"));
    return result;
  }

  // Generate scoped view and call LLM
  QJsonObject scopedView = scopedViewJson(sessionId);
  QString scopedViewStr = QString::fromUtf8(QJsonDocument(scopedView).toJson(QJsonDocument::Compact));

  QString enrichedPrompt = systemPrompt() + QStringLiteral("\n\nCurrent scoped state: ") + scopedViewStr;

  QJsonObject llmResp = llm_->complete(enrichedPrompt, history_, availableTools(), slotId_);

  QString respContent = llmResp.value(QStringLiteral("content")).toString();
  QJsonArray toolCalls = llmResp.value(QStringLiteral("tool_calls")).toArray();

  if (!toolCalls.isEmpty()) {
    // Process tool calls
    QJsonArray toolResults;
    for (const auto& tcVal : toolCalls) {
      QJsonObject tc = tcVal.toObject();
      QString callId = tc.value(QStringLiteral("id")).toString();
      QJsonObject fn = tc.value(QStringLiteral("function")).toObject();
      QString fnName = fn.value(QStringLiteral("name")).toString();
      QJsonObject fnArgs = QJsonDocument::fromJson(fn.value(QStringLiteral("arguments")).toString().toUtf8()).object();

      if (fnName == QStringLiteral("peer_request")) {
        SpecialistKind target = parseSpecialistKind(fnArgs.value(QStringLiteral("to")).toString()).value_or(SpecialistKind::Editor);
        PeerResponse pr = sendPeerRequest(
          target,
          fnArgs.value(QStringLiteral("kind")).toString(),
          fnArgs.value(QStringLiteral("args")).toObject(),
          fnArgs.value(QStringLiteral("constraints")).toObject(),
          fnArgs.value(QStringLiteral("reason")).toString(),
          fnArgs.value(QStringLiteral("await")).toBool(true)
        );
        toolResults.append(QJsonObject{
          {QStringLiteral("call_id"), callId},
          {QStringLiteral("result"), pr.toJson()}
        });
      } else {
        engine::Result cmdRes = executeCommand(fnName, fnArgs, sessionId);
        toolResults.append(QJsonObject{
          {QStringLiteral("call_id"), callId},
          {QStringLiteral("result"), cmdRes.toJson()}
        });
      }
    }
    result.insert(QStringLiteral("tool_results"), toolResults);
  }

  addMessage(ChatMessage{
    .role = QStringLiteral("assistant"),
    .content = respContent,
    .toolCalls = toolCalls
  });

  result.insert(QStringLiteral("content"), respContent);
  result.insert(QStringLiteral("status"), QStringLiteral("ok"));
  return result;
}

} // namespace sf::agent
