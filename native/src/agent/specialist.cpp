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
  // Set default warm slot IDs per specialist (0=editor, 1=colourist, 2=audio, 3=director)
  switch (kind_) {
    case SpecialistKind::Editor: slotId_ = 0; break;
    case SpecialistKind::Colourist: slotId_ = 1; break;
    case SpecialistKind::Audio: slotId_ = 2; break;
    case SpecialistKind::Director: slotId_ = 3; break;
  }
}

QString Specialist::domain() const {
  switch (kind_) {
    case SpecialistKind::Director: return QStringLiteral("director");
    case SpecialistKind::Editor: return QStringLiteral("editor");
    case SpecialistKind::Colourist: return QStringLiteral("colour");
    case SpecialistKind::Audio: return QStringLiteral("audio");
  }
  return QStringLiteral("engine");
}

QString Specialist::systemPrompt() const {
  switch (kind_) {
    case SpecialistKind::Director:
      return QStringLiteral(
        "You are the SplitFrame Director.\n"
        "You talk with the user, write storyboards and timeline plans, write prompts for video/image/speech/music generation, and assign work to specialists via agent.assign.\n"
        "A request to make a film, cut, edit or video from the project's media is only done when the clips are on the timeline. A storyboard alone is not a film.\n"
        "Recipe for making a film from imported clips and music:\n"
        "1. Read the project summary below (call assets.list only if you need paths). Call music.structure on the music asset for its beats and sections.\n"
        "2. Write the whole storyboard in ONE storyboard.set call. Every scene names at least one video clip in assetIds. Music is not a scene.\n"
        "3. Put the film on the timeline. Fast path: audio.add the music (returns the music item id), then edit.assemble with assetIds = the clips in storyboard order, "
        "musicItemId = that item, pacing = one of music-video, trailer, documentary, interview, social-short, and apply = true. "
        "Or call agent.assign to the editor with a task that lists the clip ids in order and the music asset.\n"
        "4. Only after the timeline holds the film, reply to the user with a short summary.\n"
        "Prompt craft: Write generation prompts focused on scene intent, narrative arc, mood, emotional beats, and visual metaphors on Lane deep.\n"
        "Before calling any generate.* tool, you MUST call generate.ready. If generate.ready reports not_ready, do NOT retry blindly; inform the user what is missing."
      );
    case SpecialistKind::Editor:
      return QStringLiteral(
        "You are the SplitFrame Video Editor specialist.\n"
        "You own timeline structure: cuts, trims, clip ordering, ripple editing, captions, titles, canvas format, and export.\n"
        "You collaborate with the Director, Colourist and Audio specialists via the Peer Bus.\n"
        "You DO NOT directly modify mixer nodes or grades; ask the Audio or Colourist specialist instead.\n"
        "Prompt craft: Write generation prompts using precise shot language (camera angles, movement, composition, pacing, B-roll framing) on Lane deep.\n"
        "Before calling any generate.* tool, you MUST call generate.ready. If generate.ready reports not_ready, do NOT retry blindly; inform the user what is missing.\n"
        "Workflow building: Build workflows via comfy.graph.fromTemplate or comfy.graph.new, apply ops with comfy.graph.op, validate with comfy.graph.validate, and run with generate.fromGraph. On validation failure, inspect errors, fix with ops, and retry at most 3 times."
      );
    case SpecialistKind::Colourist:
      return QStringLiteral(
        "You are the SplitFrame Colourist specialist.\n"
        "You own colour grading, LUTs, exposure, saturation, lift/gamma/gain, and OpenColorIO colour spaces.\n"
        "You collaborate with the Director, Video Editor and Audio specialists via the Peer Bus.\n"
        "You DO NOT cut or move timeline clips directly; ask the Video Editor instead.\n"
        "Prompt craft: Write generation prompts specifying look, lighting ratios, color temperature, palette, film stock, and atmosphere on Lane deep.\n"
        "Before calling any generate.* tool, you MUST call generate.ready. If generate.ready reports not_ready, do NOT retry blindly; inform the user what is missing.\n"
        "You review edits and generated clips for flash, safety, continuity, and legibility."
      );
    case SpecialistKind::Audio:
      return QStringLiteral(
        "You are the SplitFrame Audio specialist.\n"
        "You own audio tracks, background music, ducking, volume/pan, audio effects, CLAP plugins, and beat synchronization.\n"
        "You collaborate with the Director, Video Editor and Colourist specialists via the Peer Bus.\n"
        "You DO NOT change timeline clip edits directly; ask the Video Editor instead.\n"
        "Prompt craft: Write generation prompts describing voice character, cadence, style, acoustic environment, instrumentation, BPM, and mood on Lane deep.\n"
        "Before calling any generate.* tool, you MUST call generate.ready. If generate.ready reports not_ready, do NOT retry blindly; inform the user what is missing.\n"
        "You review cuts and mixes for clipped words and loudness compliance."
      );
  }
  return {};
}

namespace {

// Deterministic "what is in the project" text, built from the same read commands the model can call
// (assets.list, timeline.get, storyboard.get), so the model never has to guess or ask first.
QString projectSummaryText(engine::Engine* engine, const QString& sessionId) {
  QJsonObject sessArg;
  if (!sessionId.isEmpty()) sessArg.insert(QStringLiteral("session"), sessionId);

  const engine::Result assetsRes = engine->call(QStringLiteral("assets.list"), sessArg);
  if (!assetsRes.ok) return QStringLiteral("Project summary unavailable (no open project).");
  const QJsonArray assets = assetsRes.value.toObject().value(QStringLiteral("assets")).toArray();

  QString out = QStringLiteral("Project summary (live; call assets.list for paths and details):\n");
  if (assets.isEmpty()) {
    out += QStringLiteral("Project has no media yet.\n");
  } else {
    out += QStringLiteral("Assets (%1):\n").arg(assets.size());
    for (const auto& v : assets) {
      const QJsonObject a = v.toObject();
      QString line = QStringLiteral("- %1 | %2 | %3 | %4s")
        .arg(a.value(QStringLiteral("id")).toString(), a.value(QStringLiteral("name")).toString(),
             a.value(QStringLiteral("kind")).toString(),
             QString::number(a.value(QStringLiteral("durationMs")).toDouble() / 1000.0, 'f', 1));
      if (a.value(QStringLiteral("kind")).toString() == QLatin1String("video") && a.contains(QStringLiteral("width")))
        line += QStringLiteral(" | %1x%2").arg(a.value(QStringLiteral("width")).toInt()).arg(a.value(QStringLiteral("height")).toInt());
      line += QStringLiteral(" | hasAudio=%1").arg(a.value(QStringLiteral("hasAudio")).toBool() ? QStringLiteral("yes") : QStringLiteral("no"));
      const QString status = a.value(QStringLiteral("status")).toString();
      if (!status.isEmpty() && status != QLatin1String("ready")) line += QStringLiteral(" | status=%1").arg(status);
      out += line + QLatin1Char('\n');
    }
  }

  QJsonObject tlArg = sessArg;
  tlArg.insert(QStringLiteral("mode"), QStringLiteral("summary"));
  tlArg.insert(QStringLiteral("limit"), 1);   // only the per-track itemCount is read
  tlArg.insert(QStringLiteral("includeMarkers"), false);
  const engine::Result tlRes = engine->call(QStringLiteral("timeline.get"), tlArg);
  if (tlRes.ok) {
    const QJsonObject tl = tlRes.value.toObject();
    int items = 0;
    const QJsonArray tracks = tl.value(QStringLiteral("tracks")).toArray();
    for (const auto& t : tracks) items += t.toObject().value(QStringLiteral("itemCount")).toInt();
    const QJsonObject proj = tl.value(QStringLiteral("project")).toObject();
    const double fps = proj.value(QStringLiteral("fps")).toDouble();
    const double secs = fps > 0 ? proj.value(QStringLiteral("durationFrames")).toDouble() / fps : 0.0;
    out += QStringLiteral("Timeline: %1 tracks, %2 items, %3s total.\n").arg(tracks.size()).arg(items).arg(QString::number(secs, 'f', 1));
  }

  const engine::Result sbRes = engine->call(QStringLiteral("storyboard.get"), sessArg);
  if (sbRes.ok) out += QStringLiteral("Storyboard: %1 scenes.").arg(sbRes.value.toObject().value(QStringLiteral("count")).toInt());
  return out.trimmed();
}

} // namespace

QJsonObject Specialist::scopedViewJson(const QString& sessionId) const {
  if (!engine_) return {};

  QJsonObject view;
  QJsonObject sessArg;
  if (!sessionId.isEmpty()) sessArg.insert(QStringLiteral("session"), sessionId);

  switch (kind_) {
    case SpecialistKind::Director: {
      engine::Result sbRes = engine_->call(QStringLiteral("storyboard.get"), sessArg);
      if (sbRes.ok) view.insert(QStringLiteral("storyboard"), sbRes.value.toObject().value(QStringLiteral("scenes")));

      QJsonObject tlArg = sessArg;
      tlArg.insert(QStringLiteral("mode"), QStringLiteral("summary"));
      engine::Result tlRes = engine_->call(QStringLiteral("timeline.get"), tlArg);
      if (tlRes.ok) view.insert(QStringLiteral("timeline"), tlRes.value);

      engine::Result projRes = engine_->call(QStringLiteral("project.info"), sessArg);
      if (projRes.ok) view.insert(QStringLiteral("project"), projRes.value);
      break;
    }
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
  // Project files and undo grouping belong to the user and the harness: the harness already wraps an
  // agent run in one undo group, and saving / opening / closing files is never an agent's call.
  static const QStringList kNeverForAgents{
    QStringLiteral("project.new"), QStringLiteral("project.open"), QStringLiteral("project.save"),
    QStringLiteral("project.saveAs"), QStringLiteral("project.close")};
  if (kNeverForAgents.contains(spec.name) || spec.name.startsWith(QStringLiteral("session.group."))) return false;
  if (kind_ == SpecialistKind::Director) {
    if (spec.name == QStringLiteral("edit.assemble") || spec.name == QStringLiteral("audio.add")) return true;
    if (spec.domain == QStringLiteral("director")) return true;
    if (spec.name == QStringLiteral("agent.assign") || spec.name == QStringLiteral("agent.chat")) return true;
    if (spec.name.startsWith(QStringLiteral("storyboard."))) return true;
  }
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
    // the Director's agent.assign is the peer-bus tool appended below; listing the engine command too
    // sends a duplicate function name, which hosted APIs (Gemini) reject with HTTP 400
    if (kind_ == SpecialistKind::Director && cmd->name == QStringLiteral("agent.assign")) continue;
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
  if (kind_ == SpecialistKind::Director) {
    tools.append(QJsonObject{
      {QStringLiteral("name"), QStringLiteral("agent.assign")},
      {QStringLiteral("description"), QStringLiteral("Assign a task to a specialist (editor, colourist, audio) via the peer bus, running their real tool loop.")},
      {QStringLiteral("parameters"), QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
          {QStringLiteral("to"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("enum"), QJsonArray{QStringLiteral("editor"), QStringLiteral("colourist"), QStringLiteral("audio")}}}},
          {QStringLiteral("task"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
          {QStringLiteral("sceneId"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
        }},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("to"), QStringLiteral("task")}}
      }}
    });
  }

  // The Director hands off with agent.assign only; two near-identical hand-off tools confuse small models.
  if (kind_ != SpecialistKind::Director) tools.append(QJsonObject{
    {QStringLiteral("name"), QStringLiteral("peer_request")},
    {QStringLiteral("description"), QStringLiteral("Send a typed request to another specialist (editor, colourist, audio) across the Peer Bus.")},
    {QStringLiteral("parameters"), QJsonObject{
      {QStringLiteral("type"), QStringLiteral("object")},
      {QStringLiteral("properties"), QJsonObject{
        {QStringLiteral("to"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("enum"), QJsonArray{QStringLiteral("director"), QStringLiteral("editor"), QStringLiteral("colourist"), QStringLiteral("audio")}}}},
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

struct ReviewVerdict {
  bool ok = false;
  QString verdict; // "approve" or "revise"
  QString reason;
  QString suggestedFix;
  QString error;
};

static ReviewVerdict parseReviewVerdict(const QString& content) {
  ReviewVerdict v;
  QString text = content.trimmed();
  if (text.isEmpty()) {
    v.error = QStringLiteral("empty_response");
    return v;
  }

  // 1. Try parsing JSON object directly or embedded in code block / brackets
  QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
  if (!doc.isObject()) {
    int start = text.indexOf(u'{');
    int end = text.lastIndexOf(u'}');
    if (start >= 0 && end > start) {
      doc = QJsonDocument::fromJson(text.mid(start, end - start + 1).toUtf8());
    }
  }

  if (doc.isObject()) {
    const QJsonObject obj = doc.object();
    if (!obj.contains(QStringLiteral("verdict"))) {
      v.error = QStringLiteral("missing_verdict_field");
      return v;
    }
    const QString verd = obj.value(QStringLiteral("verdict")).toString().trimmed().toLower();
    if (verd != QLatin1String("approve") && verd != QLatin1String("revise")) {
      v.error = QStringLiteral("invalid_verdict_enum");
      return v;
    }
    v.ok = true;
    v.verdict = verd;
    v.reason = obj.value(QStringLiteral("reason")).toString();
    v.suggestedFix = obj.value(QStringLiteral("suggestedFix")).toString();
    return v;
  }

  // 2. Structured text starting with approve or revise (e.g. "approve, no flash risk", "approve: ...", "revise: ...")
  // Enforces NO substring matching: only prefix enum check!
  const QString lower = text.toLower();
  if (lower.startsWith(QLatin1String("approve"))) {
    v.ok = true;
    v.verdict = QStringLiteral("approve");
    QString rest = text.mid(7).trimmed();
    while (!rest.isEmpty() && (rest[0] == u':' || rest[0] == u',' || rest[0] == u'-')) {
      rest = rest.mid(1).trimmed();
    }
    v.reason = rest.isEmpty() ? text : rest;
    return v;
  }
  if (lower.startsWith(QLatin1String("revise"))) {
    v.ok = true;
    v.verdict = QStringLiteral("revise");
    QString rest = text.mid(6).trimmed();
    while (!rest.isEmpty() && (rest[0] == u':' || rest[0] == u',' || rest[0] == u'-')) {
      rest = rest.mid(1).trimmed();
    }
    v.reason = rest.isEmpty() ? text : rest;
    return v;
  }

  v.error = QStringLiteral("unparseable_verdict");
  return v;
}

} // namespace

PeerResponse Specialist::handlePeerRequest(const PeerRequest& req, const QString& sessionId) {
  PeerResponse res;
  res.requestId = req.id;

  if (req.kind == QStringLiteral("assign") || req.kind == QStringLiteral("task")) {
    setPeerReviewEnabled(true);
    QString task = req.args.value(QStringLiteral("task")).toString();
    if (task.isEmpty()) task = req.reason;

    const QString sceneId = req.args.value(QStringLiteral("sceneId")).toString();
    if (!sceneId.isEmpty() && engine_) {
      engine::Session* s = engine_->session(sessionId);
      if (s && s->project) {
        for (const auto& scn : s->project->doc().storyboard) {
          if (scn.id == sceneId) {
            QString sceneContext;
            if (scn.shotPrompt && !scn.shotPrompt->isEmpty()) {
              sceneContext += QStringLiteral("\nShot prompt: %1").arg(*scn.shotPrompt);
            }
            if (scn.voText && !scn.voText->isEmpty()) {
              sceneContext += QStringLiteral("\nVoiceover text: %1").arg(*scn.voText);
            }
            if (scn.musicCue && !scn.musicCue->isEmpty()) {
              sceneContext += QStringLiteral("\nMusic cue: %1").arg(*scn.musicCue);
            }
            if (scn.colourNote && !scn.colourNote->isEmpty()) {
              sceneContext += QStringLiteral("\nColour note: %1").arg(*scn.colourNote);
            }
            if (!sceneContext.isEmpty()) {
              task += QStringLiteral("\nScene context (sceneId: %1):%2").arg(sceneId, sceneContext);
            }
            break;
          }
        }
      }
    }

    QJsonObject turnRes = processTurn(task, sessionId, TurnPolicy::Execute, Lane::Fast);
    res.outputs = turnRes;
    const bool ok = (turnRes.value(QStringLiteral("status")).toString() == QStringLiteral("ok"));
    res.status = ok ? QStringLiteral("done") : QStringLiteral("declined");
    res.ops = turnRes.value(QStringLiteral("tool_results")).toArray();
    res.note = turnRes.value(QStringLiteral("content")).toString();
    if (res.note.isEmpty() && !ok) res.note = turnRes.value(QStringLiteral("error")).toString();

    // Link generated assets to sceneId if applicable
    if (ok && !sceneId.isEmpty() && engine_) {
      engine::Session* s = engine_->session(sessionId);
      if (s && s->project) {
        QJsonArray newAssetIds;
        for (const auto& trVal : res.ops) {
          const QJsonObject tr = trVal.toObject();
          const QString name = tr.value(QStringLiteral("name")).toString();
          if (name.startsWith(QStringLiteral("generate.")) || name == QStringLiteral("comfy.run")) {
            const QJsonObject resultObj = tr.value(QStringLiteral("result")).toObject();
            const QJsonObject resBody = resultObj.contains(QStringLiteral("result")) && resultObj.value(QStringLiteral("result")).isObject()
                                      ? resultObj.value(QStringLiteral("result")).toObject() : resultObj;
            const QJsonArray filesArr = resBody.value(QStringLiteral("files")).toArray();
            for (const auto& fVal : filesArr) {
              const QString fPath = fVal.toString();
              for (const auto& ast : s->project->assets()) {
                if (ast.path == fPath || ast.path.endsWith(fPath) || fPath.endsWith(ast.path)) {
                  if (!newAssetIds.contains(ast.id)) newAssetIds.append(ast.id);
                }
              }
            }
          }
        }
        if (!newAssetIds.isEmpty()) {
          QJsonObject updateArgs{
            {QStringLiteral("id"), sceneId},
            {QStringLiteral("patch"), QJsonObject{
              {QStringLiteral("assetIds"), newAssetIds},
              {QStringLiteral("status"), QStringLiteral("ready")}
            }}
          };
          if (!sessionId.isEmpty()) updateArgs.insert(QStringLiteral("session"), sessionId);
          engine_->call(QStringLiteral("storyboard.scene.update"), updateArgs);
        }
      }
    }

    return res;
  }

  if (req.kind == QStringLiteral("review") || req.kind == QStringLiteral("critique") || req.kind == QStringLiteral("peer_review")) {
    if (!llm_) {
      const QString reason = QStringLiteral("review unavailable: no llm client");
      logAgentTrace(AgentTraceRecord{
        .runId = currentRunId_.isEmpty() ? req.id : currentRunId_,
        .agentName = name(),
        .role = domain(),
        .lane = QStringLiteral("judge"),
        .action = QStringLiteral("critique"),
        .inputSummary = req.reason,
        .status = QStringLiteral("error"),
        .error = QStringLiteral("no_llm_client")
      }, projectDir_);
      res.status = QStringLiteral("error");
      res.note = reason;
      res.outputs = QJsonObject{
        {QStringLiteral("error"), QStringLiteral("no_llm_client")},
        {QStringLiteral("reason"), reason}
      };
      return res;
    }

    QJsonObject measurements;
    if (kind_ == SpecialistKind::Colourist && engine_) {
      QJsonObject safetyArgs{{QStringLiteral("session"), sessionId}};
      engine::Result rSafety = engine_->call(QStringLiteral("safety.check"), safetyArgs);
      if (rSafety.ok) {
        measurements.insert(QStringLiteral("safety.check"), rSafety.value.toObject());
      }
    } else if (kind_ == SpecialistKind::Audio && engine_) {
      QJsonObject audioArgs{{QStringLiteral("session"), sessionId}};
      engine::Result rAudio = engine_->call(QStringLiteral("audio.analyze"), audioArgs);
      if (rAudio.ok) {
        measurements.insert(QStringLiteral("audio.analyze"), rAudio.value.toObject());
      }
    }

    if (kind_ != SpecialistKind::Colourist && kind_ != SpecialistKind::Audio) {
      // Only the Colourist (safety.check) and Audio (audio.analyze) have measurements to review against.
      // Any other reviewer has nothing to ground a revision in, so it approves without asking the model
      // (a model asked anyway invents rules).
      res.status = QStringLiteral("done");
      res.note = QStringLiteral("approved: no measurements to check in the %1 domain").arg(domain());
      res.outputs = QJsonObject{
        {QStringLiteral("verdict"), QStringLiteral("approve")},
        {QStringLiteral("reason"), res.note},
        {QStringLiteral("lane"), QStringLiteral("judge")}
      };
      logAgentTrace(AgentTraceRecord{
        .runId = currentRunId_.isEmpty() ? req.id : currentRunId_,
        .agentName = name(),
        .role = domain(),
        .lane = QStringLiteral("judge"),
        .action = QStringLiteral("critique"),
        .inputSummary = req.reason,
        .outputSummary = res.note,
        .status = QStringLiteral("success")
      }, projectDir_);
      return res;
    }

    QString reviewPrompt = QStringLiteral("You are performing a peer critique/review of changes made by another specialist on Lane judge.\n");
    reviewPrompt += QStringLiteral("Request reason: ") + req.reason + QStringLiteral("\nDetails: ") + QString::fromUtf8(QJsonDocument(req.args).toJson(QJsonDocument::Compact));
    if (!measurements.isEmpty()) {
      reviewPrompt += QStringLiteral("\nDeterministic measurements for your domain:\n") + QString::fromUtf8(QJsonDocument(measurements).toJson(QJsonDocument::Compact));
    }
    reviewPrompt += QStringLiteral("\nProvide your verdict as a JSON object: {\"verdict\": \"approve\" | \"revise\", \"reason\": \"<explanation citing measurements>\", \"suggestedFix\": \"<fix>\"}.\n"
                                   "Ground your decision strictly in the deterministic measurements. Revise ONLY when a measurement shows a flash safety, clipping or loudness problem; "
                                   "otherwise approve. Never invent style or ordering rules.");

    std::vector<ChatMessage> msgs;
    msgs.push_back(ChatMessage{.role = QStringLiteral("user"), .content = reviewPrompt});

    QJsonObject resp = llm_->complete(systemPrompt(), msgs, {}, slotId_, Lane::Judge);
    const QJsonValue errVal = resp.value(QStringLiteral("error"));
    QString content = resp.value(QStringLiteral("content")).toString().trimmed();

    if ((!errVal.isUndefined() && !errVal.isNull()) || content.isEmpty()) {
      QString errCode = llmErrorCode(resp);
      if (errCode.isEmpty() && errVal.isString()) errCode = errVal.toString();
      if (errCode.isEmpty()) errCode = QStringLiteral("empty_response");
      const QString reason = QStringLiteral("review unavailable: %1").arg(errCode);
      logAgentTrace(AgentTraceRecord{
        .runId = currentRunId_.isEmpty() ? req.id : currentRunId_,
        .agentName = name(),
        .role = domain(),
        .lane = QStringLiteral("judge"),
        .action = QStringLiteral("critique"),
        .inputSummary = req.reason,
        .model = llm_->lastObservedModel(),
        .status = QStringLiteral("error"),
        .error = errCode
      }, projectDir_);
      res.status = QStringLiteral("error");
      res.note = reason;
      res.outputs = QJsonObject{
        {QStringLiteral("error"), errCode},
        {QStringLiteral("reason"), reason},
        {QStringLiteral("model"), llm_->lastObservedModel()},
        {QStringLiteral("lane"), QStringLiteral("judge")}
      };
      return res;
    }

    ReviewVerdict v = parseReviewVerdict(content);
    if (!v.ok) {
      // Re-ask once with the error
      msgs.push_back(ChatMessage{.role = QStringLiteral("assistant"), .content = content});
      msgs.push_back(ChatMessage{
        .role = QStringLiteral("user"),
        .content = QStringLiteral("Verdict format error: %1. You must respond with a JSON object: {\"verdict\": \"approve\"|\"revise\", \"reason\": \"...\", \"suggestedFix\": \"...\"}").arg(v.error)
      });
      resp = llm_->complete(systemPrompt(), msgs, {}, slotId_, Lane::Judge);
      const QJsonValue reErr = resp.value(QStringLiteral("error"));
      content = resp.value(QStringLiteral("content")).toString().trimmed();
      if ((!reErr.isUndefined() && !reErr.isNull()) || content.isEmpty()) {
        v.ok = false;
        v.error = QStringLiteral("re_ask_failed");
      } else {
        v = parseReviewVerdict(content);
      }
    }

    if (!v.ok) {
      const QString reason = QStringLiteral("review unavailable: parse error (%1)").arg(v.error);
      logAgentTrace(AgentTraceRecord{
        .runId = currentRunId_.isEmpty() ? req.id : currentRunId_,
        .agentName = name(),
        .role = domain(),
        .lane = QStringLiteral("judge"),
        .action = QStringLiteral("critique"),
        .inputSummary = req.reason,
        .model = llm_->lastObservedModel(),
        .status = QStringLiteral("error"),
        .error = v.error
      }, projectDir_);
      res.status = QStringLiteral("error");
      res.note = reason;
      res.outputs = QJsonObject{
        {QStringLiteral("error"), v.error},
        {QStringLiteral("reason"), reason},
        {QStringLiteral("model"), llm_->lastObservedModel()},
        {QStringLiteral("lane"), QStringLiteral("judge")}
      };
      return res;
    }

    res.outputs = QJsonObject{
      {QStringLiteral("content"), content},
      {QStringLiteral("verdict"), v.verdict},
      {QStringLiteral("model"), llm_->lastObservedModel()},
      {QStringLiteral("lane"), QStringLiteral("judge")}
    };
    if (!v.reason.isEmpty()) res.outputs.insert(QStringLiteral("reason"), v.reason);
    if (!v.suggestedFix.isEmpty()) res.outputs.insert(QStringLiteral("suggestedFix"), v.suggestedFix);

    if (v.verdict == QLatin1String("revise")) {
      res.status = QStringLiteral("needs_revision");
      res.note = v.reason.isEmpty() ? content : v.reason;
      logAgentTrace(AgentTraceRecord{
        .runId = currentRunId_.isEmpty() ? req.id : currentRunId_,
        .agentName = name(),
        .role = domain(),
        .lane = QStringLiteral("judge"),
        .action = QStringLiteral("critique"),
        .inputSummary = req.reason,
        .model = llm_->lastObservedModel(),
        .status = QStringLiteral("needs_revision"),
        .error = QString()
      }, projectDir_);
    } else {
      res.status = QStringLiteral("done");
      res.note = v.reason.isEmpty() ? QStringLiteral("approved") : v.reason;
      logAgentTrace(AgentTraceRecord{
        .runId = currentRunId_.isEmpty() ? req.id : currentRunId_,
        .agentName = name(),
        .role = domain(),
        .lane = QStringLiteral("judge"),
        .action = QStringLiteral("critique"),
        .inputSummary = req.reason,
        .model = llm_->lastObservedModel(),
        .status = QStringLiteral("success"),
        .error = QString()
      }, projectDir_);
    }
    return res;
  }

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

QJsonObject Specialist::processTurn(const QString& userMessage, const QString& sessionId, TurnPolicy policy, Lane lane) {
  QJsonObject result;
  result.insert(QStringLiteral("specialist"), name());

  const QString runId = !currentRunId_.isEmpty() ? currentRunId_ : QUuid::createUuid().toString(QUuid::WithoutBraces);

  emit agentEvent(QJsonObject{
    {QStringLiteral("agent"), name()},
    {QStringLiteral("lane"), laneName(lane)},
    {QStringLiteral("kind"), QStringLiteral("think")},
    {QStringLiteral("text"), QStringLiteral("Thinking...")}
  });

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
      .lane = laneName(lane),
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
  QString enrichedPrompt = systemPrompt();
  if (engine_) enrichedPrompt += QStringLiteral("\n\n") + projectSummaryText(engine_, sessionId);
  enrichedPrompt += QStringLiteral("\n\nCurrent scoped state: ") + scopedViewStr;

  QJsonArray allToolResults;
  QJsonArray proposed;
  QString respContent;
  QString status = QStringLiteral("ok");
  int revisionCount = 0;

  // No round cap: the turn runs until the model stops calling tools or the user presses Stop.
  for (int round = 0;; ++round) {
    if (round > 0) {
      logAgentTrace(AgentTraceRecord{
        .runId = runId,
        .agentName = name(),
        .role = domain(),
        .lane = laneName(lane),
        .action = QStringLiteral("retry"),
        .inputSummary = QStringLiteral("Turn round %1").arg(round),
        .model = llm_->lastObservedModel(),
        .status = QStringLiteral("success"),
        .retryCount = round
      }, projectDir_);
    }

    if (cancelled()) {
      status = QStringLiteral("cancelled");
      break;
    }
    QJsonObject llmResp = llm_->complete(enrichedPrompt, history_, availableTools(), slotId_, lane);
    if (cancelled() && llmResp.value(QStringLiteral("tool_calls")).toArray().isEmpty()) {
      status = QStringLiteral("cancelled");
      break;
    }
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
        .lane = laneName(lane),
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
        const QString act = (kind_ == SpecialistKind::Director &&
                             (lane == Lane::Deep || userMessage.contains(QStringLiteral("plan"), Qt::CaseInsensitive) ||
                              userMessage.contains(QStringLiteral("brief"), Qt::CaseInsensitive) ||
                              userMessage.contains(QStringLiteral("storyboard"), Qt::CaseInsensitive)))
                          ? QStringLiteral("plan")
                          : QStringLiteral("message");
        logAgentTrace(AgentTraceRecord{
          .runId = runId,
          .agentName = name(),
          .role = domain(),
          .lane = laneName(lane),
          .action = act,
          .inputSummary = userMessage,
          .outputSummary = respContent,
          .model = llm_->lastObservedModel(),
          .status = QStringLiteral("success"),
          .retryCount = round
        }, projectDir_);

        emit agentEvent(QJsonObject{
          {QStringLiteral("agent"), name()},
          {QStringLiteral("lane"), laneName(lane)},
          {QStringLiteral("kind"), QStringLiteral("message")},
          {QStringLiteral("text"), respContent},
          {QStringLiteral("ok"), true}
        });
      }
      break;
    }

    bool roundMutated = false;
    QString lastMutatingTool;
    QJsonObject lastMutatingArgs;
    QJsonObject lastMutatingResult;

    for (const auto& tcVal : toolCalls) {
      QJsonObject tc = tcVal.toObject();
      QString callId = tc.value(QStringLiteral("id")).toString();
      QJsonObject fn = tc.value(QStringLiteral("function")).toObject();
      QString fnName = fn.value(QStringLiteral("name")).toString();
      QJsonObject fnArgs = parseToolArgs(fn.value(QStringLiteral("arguments")));

      if (cancelled()) {
        // Stop: the call the model asked for is not run; the history still needs an answer for it
        addMessage(ChatMessage{
          .role = QStringLiteral("tool"),
          .content = QStringLiteral("{\"ok\":false,\"error\":\"cancelled\",\"message\":\"Stopped by the user before this call ran.\"}"),
          .name = fnName,
          .toolCallId = callId
        });
        status = QStringLiteral("cancelled");
        continue;
      }

      emit agentEvent(QJsonObject{
        {QStringLiteral("agent"), name()},
        {QStringLiteral("lane"), laneName(lane)},
        {QStringLiteral("kind"), QStringLiteral("tool")},
        {QStringLiteral("text"), fnName},
        {QStringLiteral("tool"), fnName},
        {QStringLiteral("args"), fnArgs}
      });

      QJsonObject toolResult;
      bool mutating = false;
      if (fnName == QStringLiteral("peer_request") || fnName == QStringLiteral("agent.assign")) {
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
      } else if (fnName == QStringLiteral("agent.assign")) {
        const QString toStr = fnArgs.value(QStringLiteral("to")).toString();
        auto optTarget = parseSpecialistKind(toStr);
        if (!optTarget) {
          toolResult = QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"), QStringLiteral("invalid_params")},
            {QStringLiteral("message"), QStringLiteral("Invalid target '%1' for agent.assign. Valid targets are: director, editor, colourist, audio.").arg(toStr)}
          };
        } else {
          SpecialistKind target = *optTarget;
          PeerResponse pr = sendPeerRequest(
            target,
            QStringLiteral("assign"),
            fnArgs,
            {},
            fnArgs.value(QStringLiteral("task")).toString(),
            true
          );
          toolResult = pr.toJson();
        }
      } else if (fnName == QStringLiteral("peer_request")) {
        const QString toStr = fnArgs.value(QStringLiteral("to")).toString();
        auto optTarget = parseSpecialistKind(toStr);
        if (!optTarget) {
          toolResult = QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"), QStringLiteral("invalid_params")},
            {QStringLiteral("message"), QStringLiteral("Invalid target '%1'. Valid targets are: director, editor, colourist, audio.").arg(toStr)}
          };
        } else {
          SpecialistKind target = *optTarget;
          PeerResponse pr = sendPeerRequest(
            target,
            fnArgs.value(QStringLiteral("kind")).toString(),
            fnArgs.value(QStringLiteral("args")).toObject(),
            fnArgs.value(QStringLiteral("constraints")).toObject(),
            fnArgs.value(QStringLiteral("reason")).toString(),
            fnArgs.value(QStringLiteral("await")).toBool(true)
          );
          toolResult = pr.toJson();
        }
      } else {
        toolResult = executeCommand(fnName, fnArgs, sessionId).toJson();
      }

      const bool isPeer = (fnName == QStringLiteral("peer_request") || fnName == QStringLiteral("agent.assign"));
      const QString targetAgent = isPeer ? fnArgs.value(QStringLiteral("to")).toString() : QString();
      const bool ok = toolResult.value(QStringLiteral("ok")).toBool(true);
      const QString toolStatus = (policy == TurnPolicy::Propose && mutating) ? QStringLiteral("needs_revision") : (ok ? QStringLiteral("success") : QStringLiteral("error"));

      if (ok && mutating && fnName != QStringLiteral("peer_request") && fnName != QStringLiteral("agent.assign")) {
        roundMutated = true;
        lastMutatingTool = fnName;
        lastMutatingArgs = fnArgs;
        lastMutatingResult = toolResult;
      }

      emit agentEvent(QJsonObject{
        {QStringLiteral("agent"), name()},
        {QStringLiteral("lane"), laneName(lane)},
        {QStringLiteral("kind"), QStringLiteral("result")},
        {QStringLiteral("text"), QString::fromUtf8(QJsonDocument(toolResult).toJson(QJsonDocument::Compact))},
        {QStringLiteral("tool"), fnName},
        {QStringLiteral("ok"), ok}
      });

      logAgentTrace(AgentTraceRecord{
        .runId = runId,
        .agentName = name(),
        .role = domain(),
        .lane = laneName(lane),
        .action = (fnName == QStringLiteral("agent.assign")) ? QStringLiteral("handoff") : QStringLiteral("tool_call"),
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

    if (cancelled()) {
      status = QStringLiteral("cancelled");
      break;
    }

    // Review loop: After any mutating step, run peer review on Lane judge
    if (peerReviewEnabled_ && roundMutated && policy == TurnPolicy::Execute &&
        bus_) {
      SpecialistKind reviewer = SpecialistKind::Editor;
      if (kind_ == SpecialistKind::Editor) {
        reviewer = SpecialistKind::Colourist;
      } else if (kind_ == SpecialistKind::Colourist || kind_ == SpecialistKind::Audio || kind_ == SpecialistKind::Director) {
        reviewer = SpecialistKind::Editor;
      }

      PeerResponse revResp = sendPeerRequest(
        reviewer,
        QStringLiteral("review"),
        QJsonObject{
          {QStringLiteral("tool"), lastMutatingTool},
          {QStringLiteral("arguments"), lastMutatingArgs},
          {QStringLiteral("result"), lastMutatingResult}
        },
        {},
        QStringLiteral("Review %1 changes").arg(lastMutatingTool),
        true
      );

      emit agentEvent(QJsonObject{
        {QStringLiteral("agent"), specialistName(reviewer)},
        {QStringLiteral("lane"), QStringLiteral("judge")},
        {QStringLiteral("kind"), QStringLiteral("review")},
        {QStringLiteral("text"), revResp.note},
        {QStringLiteral("ok"), revResp.status != QStringLiteral("needs_revision")}
      });

      if (revResp.status == QStringLiteral("needs_revision")) {
        ++revisionCount;
        addMessage(ChatMessage{
          .role = QStringLiteral("user"),
          .content = QStringLiteral("Peer review from %1 requested revision: %2. Please revise.")
                       .arg(specialistName(reviewer), revResp.note)
        });
      }
    }

  }

  if (kind_ == SpecialistKind::Director && lane == Lane::Deep) {
    logAgentTrace(AgentTraceRecord{
      .runId = runId,
      .agentName = name(),
      .role = domain(),
      .lane = QStringLiteral("deep"),
      .action = QStringLiteral("plan"),
      .inputSummary = userMessage,
      .outputSummary = respContent.isEmpty() ? QStringLiteral("Director storyboard plan") : respContent,
      .model = llm_ ? llm_->lastObservedModel() : QString(),
      .status = QStringLiteral("success"),
      .retryCount = 0
    }, projectDir_);
  }

  if (!allToolResults.isEmpty()) result.insert(QStringLiteral("tool_results"), allToolResults);
  if (!proposed.isEmpty()) result.insert(QStringLiteral("proposed"), proposed);
  result.insert(QStringLiteral("content"), respContent);
  result.insert(QStringLiteral("status"), status);
  return result;
}

} // namespace sf::agent
