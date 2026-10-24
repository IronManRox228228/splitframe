// ComfyUI integration: node discovery, schema introspection, typed workflow graph editing and deterministic validation.

#include "engine/cmd_util.h"
#include "engine/engine.h"
#include "comfy/comfy_client.h"
#include "comfy/comfy_graph.h"
#include "comfy/comfy_process.h"
#include "comfy/comfy_schema.h"
#include "comfy/comfy_validate.h"

#include "core/apply.h"
#include "core/timeline_doc.h"
#include "jobs/job_scheduler.h"
#include "media/probe.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QDateTime>
#include <QImage>
#include <QMap>
#include <QPainter>
#include "engine/cmd_comfy.h"
#include "render/layer_painter.h"
#include <QStandardPaths>
#include <QTimer>

namespace sf::engine {

ComfyService::ComfyService(Engine* engine) {
  process = std::make_unique<comfy::ComfyProcess>();
  if (process->url().isValid()) {
    client = comfy::Client(process->url());
  }

  QObject::connect(process.get(), &comfy::ComfyProcess::stateChanged,
                   [this, engine](comfy::ComfyServerState st, const QJsonObject& info) {
    if (st == comfy::ComfyServerState::Ready) {
      client = comfy::Client(process->url());
      schema.reset();
      schemaSource.clear();
    }
    if (engine) {
      QJsonObject evtData{
        {QStringLiteral("mode"), process->modeString()},
        {QStringLiteral("state"), comfy::stateToString(st)},
        {QStringLiteral("port"), process->port()},
        {QStringLiteral("pid"), process->pid()}
      };
      if (info.contains(QStringLiteral("reason"))) {
        evtData.insert(QStringLiteral("reason"), info.value(QStringLiteral("reason")));
      }
      engine->emitEvent(QStringLiteral("comfy.state"), evtData);
    }
  });

  if (process->autoStart() && process->modeString() == QLatin1String("managed")) {
    // deferred, NOT from the ctor: the engine assigns comfy_ only after this returns, so a
    // synchronous start (which waits for the server) would let a re-entrant Engine::comfy()
    // construct a second service/child process. State changes flow through stateChanged.
    QTimer::singleShot(0, process.get(), [process = process.get()] { process->start(); });
  }
}

ComfyService& Engine::comfy() {
  if (!comfy_) comfy_ = std::make_unique<ComfyService>(this);
  return *comfy_;
}

} // namespace sf::engine

void std::default_delete<sf::engine::ComfyService>::operator()(sf::engine::ComfyService* p) const {
  delete p;
}

namespace sf::engine {

namespace {

std::optional<QJsonObject> readSidecar(const QString& assetPath) {
  if (assetPath.isEmpty()) return std::nullopt;
  const QString scPath = assetPath.endsWith(QLatin1String(".sfgen.json")) ? assetPath : (assetPath + QStringLiteral(".sfgen.json"));
  QFile f(scPath);
  if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
  QJsonParseError pe;
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
  if (pe.error != QJsonParseError::NoError || !doc.isObject()) return std::nullopt;
  return doc.object();
}

QImage getStillForFile(const QString& filePath) {
  QImage img(filePath);
  if (!img.isNull()) return img;
  auto info = probeMedia(filePath);
  if (info && info->hasVideo) {
    const qint64 midMs = info->durationMs / 2;
    QString err;
    return decodeFrame(filePath, midMs, &err);
  }
  return decodeFrame(filePath, 0);
}

Result ensureSchema(ComfyService& svc, const QJsonObject& args) {
  const bool refresh = args.value(q("refresh")).toBool(false);
  const QString argPath = args.value(q("objectInfoPath")).toString();
  if (svc.schema && !refresh && (argPath.isEmpty() || argPath == svc.schemaSource)) {
    return Result::success();
  }

  QString objPath = argPath;
  if (objPath.isEmpty()) {
    objPath = qEnvironmentVariable("SF_COMFY_OBJECT_INFO");
  }

  QJsonObject objInfo;
  QString source;
  if (!objPath.isEmpty()) {
    QFile f(objPath);
    if (!f.open(QIODevice::ReadOnly)) {
      return Result::failure(q("unavailable"), QStringLiteral("Could not read schema file: %1").arg(objPath),
                            QStringLiteral("Start ComfyUI (python main.py --listen) or set SF_COMFY_URL / SF_COMFY_OBJECT_INFO."));
    }
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
      return Result::failure(q("unavailable"), QStringLiteral("Failed to parse schema JSON from %1: %2").arg(objPath, parseErr.errorString()),
                            QStringLiteral("Start ComfyUI (python main.py --listen) or set SF_COMFY_URL / SF_COMFY_OBJECT_INFO."));
    }
    objInfo = doc.object();
    source = objPath;
  } else {
    QString err;
    auto res = svc.client.objectInfo(&err);
    if (!res) {
      return Result::failure(q("unavailable"), err,
                            QStringLiteral("Start ComfyUI (python main.py --listen) or set SF_COMFY_URL / SF_COMFY_OBJECT_INFO."));
    }
    objInfo = *res;
    source = svc.client.base().toString();
  }

  QStringList warnings;
  svc.schema = comfy::SchemaSet::parse(objInfo, &warnings);
  svc.schemaSource = source;
  return Result::success();
}

QString kindToString(comfy::InputKind kind) {
  switch (kind) {
  case comfy::InputKind::Link: return QStringLiteral("link");
  case comfy::InputKind::Int: return QStringLiteral("int");
  case comfy::InputKind::Float: return QStringLiteral("float");
  case comfy::InputKind::String: return QStringLiteral("string");
  case comfy::InputKind::Boolean: return QStringLiteral("boolean");
  case comfy::InputKind::Combo: return QStringLiteral("combo");
  }
  return QStringLiteral("unknown");
}

struct TemplateInfo {
  QString id;
  QString name;
  QString kind;
  QString description;
  QJsonArray whenToUse;
  QJsonObject graph;
  QString outputNode;
  QJsonObject bindings;
  // schema v2 (v1 templates leave these parsed-but-derived): what the graph produces and what it
  // needs from the ComfyUI install, so generate.* can report readiness before queueing anything.
  QString outputKind;    // derived "video" | "image" | "audio"
  QJsonObject required;  // { nodes: [class_type...], models: [{folder, file}...] }; key stays "requires" in template JSON
  bool needsUserModel = false;
  QString source;
  QString filePath;
};

const QStringList kAllowedBindings = {
  QStringLiteral("prompt"), QStringLiteral("negative"), QStringLiteral("seed"),
  QStringLiteral("width"), QStringLiteral("height"), QStringLiteral("frames"),
  QStringLiteral("fps"), QStringLiteral("steps"), QStringLiteral("cfg"),
  QStringLiteral("image"), QStringLiteral("strength"), QStringLiteral("duration"),
  // v2 media bindings
  QStringLiteral("text"), QStringLiteral("voiceRef"), QStringLiteral("durationSec"),
  QStringLiteral("bpm"), QStringLiteral("lyrics"), QStringLiteral("tags"), QStringLiteral("audio")
};

// Media kinds for templates: image/video were there from v1, speech/music are v2.
const QStringList kTemplateKinds = {
  QStringLiteral("image"), QStringLiteral("video"), QStringLiteral("speech"), QStringLiteral("music")
};

// What a template's kind produces on disk, for the landing path: videos and images are files,
// speech and music audio templates deliver audio.
QString deriveOutputKind(const QString& kind) {
  if (kind == QLatin1String("video")) return QStringLiteral("video");
  if (kind == QLatin1String("image")) return QStringLiteral("image");
  if (kind == QLatin1String("speech") || kind == QLatin1String("music")) return QStringLiteral("audio");
  return {};
}

// ---- generate readiness: what generate.* consults before queueing a template ----

struct TemplateReadiness {
  bool ok = false;
  QStringList missingNodes;  // class_type strings this ComfyUI does not serve
  QJsonArray missingModels;  // {folder, file} entries no combo input offers
  QString reason;
};

// Checks one template against the cached /object_info: every node class the graph uses and every
// model the template requires must exist server-side. Models are offered as combo options on loader
// inputs; a file that appears in no combo anywhere is missing from this install.
TemplateReadiness checkTemplateReadiness(ComfyService& svc, const TemplateInfo& t) {
  TemplateReadiness r;
  if (!svc.schema) {
    r.reason = QStringLiteral("ComfyUI schema is not loaded (no reachable server or object_info file)");
    return r;
  }

  // every distinct class_type the graph uses plus the ones requires.nodes demands
  QStringList classes;
  {
    QSet<QString> seen;
    for (auto it = t.graph.begin(); it != t.graph.end(); ++it) {
      const QString ct = it.value().toObject().value(q("class_type")).toString();
      if (!ct.isEmpty() && !seen.contains(ct)) {
        seen.insert(ct);
        classes << ct;
      }
    }
    for (const QJsonValue& n : t.required.value(q("nodes")).toArray()) {
      const QString ct = n.toString();
      if (!ct.isEmpty() && !seen.contains(ct)) {
        seen.insert(ct);
        classes << ct;
      }
    }
  }
  classes.sort();
  for (const QString& ct : classes) {
    if (!svc.schema->find(ct)) r.missingNodes << ct;
  }

  // model files: compare against every combo option in the schema
  QSet<QString> comboFiles;
  for (const QString& cls : svc.schema->classTypes()) {
    const comfy::NodeSchema* ns = svc.schema->find(cls);
    if (!ns) continue;
    for (const comfy::InputSpec& in : ns->inputs) {
      if (in.kind != comfy::InputKind::Combo) continue;
      for (const QString& o : in.options) comboFiles.insert(o);
    }
  }
  for (const QJsonValue& m : t.required.value(q("models")).toArray()) {
    const QJsonObject mo = m.toObject();
    const QString folder = mo.value(q("folder")).toString();
    const QString file = mo.value(q("file")).toString();
    if (!file.isEmpty() && !comboFiles.contains(file)) {
      r.missingModels.append(QJsonObject{{q("folder"), folder}, {q("file"), file}});
    }
  }

  if (!r.missingNodes.isEmpty()) {
    r.reason = QStringLiteral("Node(s) not in ComfyUI: %1").arg(r.missingNodes.join(QStringLiteral(", ")));
  }
  for (const QJsonValue& m : r.missingModels) {
    if (!r.reason.isEmpty()) r.reason += QStringLiteral("; ");
    r.reason += QStringLiteral("Model file '%1' not installed in folder '%2'")
                    .arg(m.toObject().value(q("file")).toString(), m.toObject().value(q("folder")).toString());
  }
  if (r.reason.isEmpty()) r.ok = true;
  return r;
}

QString userTemplatesDir() {
  const QString env = qEnvironmentVariable("SF_COMFY_TEMPLATES");
  if (!env.isEmpty()) return env;
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("comfy_templates"));
}

QString builtinTemplatesDir() {
#ifdef SF_COMFY_TEMPLATES_DIR
  return QStringLiteral(SF_COMFY_TEMPLATES_DIR);
#else
  return QString();
#endif
}

std::optional<TemplateInfo> parseTemplateJson(const QByteArray& data, const QString& source, const QString& filePath) {
  QJsonParseError pe;
  const QJsonDocument doc = QJsonDocument::fromJson(data, &pe);
  if (pe.error != QJsonParseError::NoError || !doc.isObject()) return std::nullopt;
  const QJsonObject o = doc.object();
  TemplateInfo ti;
  ti.id = o.value(q("id")).toString();
  ti.name = o.value(q("name")).toString();
  ti.kind = o.value(q("kind")).toString();
  ti.description = o.value(q("description")).toString();
  ti.whenToUse = o.value(q("whenToUse")).toArray();
  ti.graph = o.value(q("graph")).toObject();
  ti.outputNode = o.value(q("outputNode")).toString();
  ti.bindings = o.value(q("bindings")).toObject();
  // v2 keys; a v1 template has none of them, so outputKind falls back to the kind's product
  ti.outputKind = o.value(q("outputKind")).toString();
  ti.required = o.value(q("requires")).toObject();
  ti.needsUserModel = o.value(q("needsUserModel")).toBool();
  ti.source = source;
  ti.filePath = filePath;
  if (ti.id.isEmpty() || ti.graph.isEmpty()) return std::nullopt;
  if (ti.outputKind.isEmpty()) ti.outputKind = deriveOutputKind(ti.kind);
  return ti;
}

std::map<QString, TemplateInfo> loadAllTemplates() {
  std::map<QString, TemplateInfo> result;

  const QString bDir = builtinTemplatesDir();
  if (!bDir.isEmpty()) {
    QDir d(bDir);
    const QFileInfoList entries = d.entryInfoList({QStringLiteral("*.json")}, QDir::Files);
    for (const auto& fi : entries) {
      QFile f(fi.absoluteFilePath());
      if (f.open(QIODevice::ReadOnly)) {
        auto ti = parseTemplateJson(f.readAll(), QStringLiteral("builtin"), fi.absoluteFilePath());
        if (ti) result[ti->id] = *ti;
      }
    }
  }

  const QString uDir = userTemplatesDir();
  if (!uDir.isEmpty()) {
    QDir d(uDir);
    const QFileInfoList entries = d.entryInfoList({QStringLiteral("*.json")}, QDir::Files);
    for (const auto& fi : entries) {
      QFile f(fi.absoluteFilePath());
      if (f.open(QIODevice::ReadOnly)) {
        auto ti = parseTemplateJson(f.readAll(), QStringLiteral("user"), fi.absoluteFilePath());
        if (ti) result[ti->id] = *ti;
      }
    }
  }

  return result;
}

Frame calcTrackEnd(const TimelineDoc& d, const QString& trackId) {
  Frame end = 0;
  for (const Item& i : d.items) {
    if (i.trackId == trackId) end = std::max(end, itemEnd(i));
  }
  return end;
}

const Asset* findAssetByPath(const editor::Project& project, const QString& fPath) {
  const QFileInfo fi(fPath);
  const QString abs = fi.absoluteFilePath();
  for (const Asset& as : project.assets()) {
    if (as.path == fPath || as.path == abs || QFileInfo(as.path).absoluteFilePath() == abs) return &as;
  }
  return nullptr;
}

void appendProvenanceRecord(editor::Project& project, const QJsonObject& record) {
  if (project.path().isEmpty()) return;
  QDir dir(QFileInfo(project.path()).dir());
  if (!dir.mkpath(QStringLiteral("generated"))) return;
  QFile f(dir.filePath(QStringLiteral("provenance.jsonl")));
  if (!f.open(QIODevice::Append | QIODevice::WriteOnly | QIODevice::Text)) return;
  f.write(QJsonDocument(record).toJson(QJsonDocument::Compact));
  f.write("\n");
}

QJsonObject provBase(const QString& templateId, const TemplateInfo* tmpl, const QString& outputKind,
                     const QString& prompt, const QString& text, const QString& negative,
                     qint64 seed, const QString& promptId, const QString& workflowHash) {
  QJsonObject p;
  p.insert(q("templateId"), templateId);
  if (tmpl) p.insert(q("kind"), tmpl->kind);
  p.insert(q("outputKind"), outputKind);
  if (!prompt.isEmpty()) p.insert(q("prompt"), prompt);
  if (!text.isEmpty()) p.insert(q("text"), text);
  if (!negative.isEmpty()) p.insert(q("negative"), negative);
  p.insert(q("seed"), seed);
  if (!promptId.isEmpty()) p.insert(q("promptId"), promptId);
  if (!workflowHash.isEmpty()) p.insert(q("workflowHash"), workflowHash);
  p.insert(q("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
  return p;
}

// Imports `files` into the media pool, places one item per file on the timeline (one undo group,
// labelled `label`), and - when a template is known - stamps generation provenance on each landed
// asset and appends one JSON line to <project>/generated/provenance.jsonl.
Result landOutputs(CallContext& c, const QStringList& files, const QString& trackIdArg,
                   std::optional<Frame> atFrameArg, const QString& label,
                   const QString& /*outputKind*/, const QString& templateId,
                   const TemplateInfo* tmpl, const QJsonObject& provBase) {
  QJsonArray sidecarsArr;
  for (const QString& f : files) sidecarsArr.append(f + QStringLiteral(".sfgen.json"));
  if (files.isEmpty()) {
    return Result::success(QJsonObject{{q("created"), QJsonObject{}}, {q("sidecars"), sidecarsArr}});
  }

  c.session->pool->import(files);
  c.session->pool->waitForIdle(60000);

  TimelineDoc doc = c.session->project->doc();
  const double fps = static_cast<double>(doc.project.fps);

  QMap<QString, sf::MediaInfo> probed;
  for (const QString& fPath : files) {
    QString err;
    if (auto mi = sf::probeMedia(fPath, &err)) probed.insert(fPath, *mi);
  }

  std::vector<Op> ops;
  for (const QString& fPath : files) {
    const Asset* asset = findAssetByPath(*c.session->project, fPath);
    if (!asset) continue;

    ItemType type = ItemType::Image;
    if (asset->kind == AssetKind::Audio) type = ItemType::Audio;
    else if (asset->kind == AssetKind::Video) type = ItemType::Video;

    QString targetTrackId = trackIdArg;
    if (!targetTrackId.isEmpty()) {
      const Track* t = getTrack(doc, targetTrackId);
      if (!t || t->locked || !trackAllowsItem(t->kind, type)) targetTrackId.clear();
    }
    if (targetTrackId.isEmpty()) {
      for (const Track& t : doc.tracks) {
        if (!t.locked && trackAllowsItem(t.kind, type)) {
          targetTrackId = t.id;
          break;
        }
      }
    }
    if (targetTrackId.isEmpty()) {
      TrackKind tk = (type == ItemType::Audio ? TrackKind::Audio : TrackKind::Video);
      targetTrackId = newId(q("trk"));
      Track newTrk;
      newTrk.id = targetTrackId;
      newTrk.kind = tk;
      newTrk.name = (tk == TrackKind::Audio ? QStringLiteral("Audio") : QStringLiteral("Video"));
      ops.push_back(Op(TrackAdd{newTrk.id, newTrk.kind, newTrk.name}));
      doc.tracks.push_back(newTrk);
    }

    // With `atFrame`, the anchor applies to the first file only: each further file of the batch
    // advances past the previous one instead of piling every file onto the same frame.
    Frame start = atFrameArg ? *atFrameArg : calcTrackEnd(doc, targetTrackId);
    atFrameArg = std::nullopt;

    Frame dur = 1;
    const auto pit = probed.find(fPath);
    if (type == ItemType::Image) {
      dur = jsRound(3.0 * fps);
    } else if (pit != probed.end()) {
      dur = jsRound(static_cast<double>(pit.value().durationMs) / 1000.0 * fps);
    } else {
      dur = asset->durationMs > 0 ? jsRound(static_cast<double>(asset->durationMs) / 1000.0 * fps) : jsRound(fps);
    }

    ItemInit init;
    init.id = newId(q("itm"));
    init.trackId = targetTrackId;
    init.startFrame = start;
    init.durationFrames = dur;
    init.assetId = asset->id;
    if (type != ItemType::Image) init.sourceInFrame = 0;
    init.labels = Labels{type == ItemType::Audio ? QStringLiteral("audio") : QStringLiteral("clip"), std::nullopt};

    Item item = createItem(type, init);
    ops.push_back(Op(ItemAdd{item, false}));
    doc.items.push_back(item);
  }

  editor::Project& p = *c.session->project;
  if (!p.apply(ops, label, c.actor())) {
    return Result::failure(q("edit_rejected"), p.lastError());
  }

  if (!templateId.isEmpty() && tmpl) {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const QString& fPath : files) {
      const Asset* asset = findAssetByPath(*c.session->project, fPath);
      if (!asset) continue;
      QJsonObject prov = provBase;
      prov.insert(q("outputFile"), fPath);
      if (const auto pit = probed.find(fPath); pit != probed.end()) {
        prov.insert(q("width"), pit.value().width);
        prov.insert(q("height"), pit.value().height);
        prov.insert(q("durationMs"), static_cast<qint64>(pit.value().durationMs));
      }
      Asset updated = *asset;
      updated.metadata.provenance = prov;
      p.updateAsset(updated);
      appendProvenanceRecord(p, prov);
    }
  }

  return Result::success(QJsonObject{
    {q("created"), createdIds(ops)},
    {q("sidecars"), sidecarsArr}
  });
}

Result collectJobOutputs(CallContext& c, const QString& jobId, qint64 seed, const QString& trackIdArg, std::optional<Frame> atFrameArg) {
  auto res = c.engine->jobs().result(jobId);
  if (!res || !res->contains(q("files"))) {
    return Result::failure(q("internal"), QStringLiteral("Job result has no files."));
  }
  const QJsonArray filesArr = res->value(q("files")).toArray();
  QStringList files;
  for (const auto& v : filesArr) files.append(v.toString());

  if (files.isEmpty()) {
    return Result::success(QJsonObject{
      {q("jobId"), jobId},
      {q("seed"), seed},
      {q("files"), filesArr},
      {q("created"), QJsonObject{}},
      {q("sidecars"), QJsonArray{}}
    });
  }

  Result landed = landOutputs(c, files, trackIdArg, atFrameArg, QStringLiteral("generateShot"),
                              {}, {}, nullptr, {});
  if (!landed.ok) return landed;
  const QJsonObject landedObj = landed.value.toObject();
  return Result::success(QJsonObject{
    {q("jobId"), jobId},
    {q("seed"), seed},
    {q("files"), filesArr},
    {q("created"), landedObj.value(q("created"))},
    {q("sidecars"), landedObj.value(q("sidecars"))}
  });
}

// Deterministic seed (same recipe as the original generate.shot): sha1(templateId | basis) -> 32 bits.
qint64 deriveSeed(const QString& templateId, const QString& basis) {
  const QByteArray sha = QCryptographicHash::hash((templateId + QLatin1Char('|') + basis).toUtf8(), QCryptographicHash::Sha1).toHex();
  bool okSeed = false;
  const quint32 s32 = sha.left(8).toUInt(&okSeed, 16);
  return okSeed ? static_cast<qint64>(s32) : 0;
}

// Shared core for generate.video/image/speech/music: bind template params to the graph, run
// ComfyUI (1..4 variants), land outputs via the media pool + one undo group, record provenance.
// `expectedKind` must match the template's outputKind; `allowedExtensions` are the lowercase
// suffixes accepted from ComfyUI outputs for this kind.
Result runGenerate(CallContext& c, const QJsonObject& a, const QString& expectedKind,
                   const QStringList& allowedExtensions) {
  ComfyService& svc = c.engine->comfy();
  (void)ensureSchema(svc, a); // populate svc.schema so the readiness gate can run

  const QString templateId = a.value(q("templateId")).toString();
  if (templateId.isEmpty()) return invalid(QStringLiteral("Argument 'templateId' is required"));

  auto allT = loadAllTemplates();
  auto itT = allT.find(templateId);
  if (itT == allT.end()) {
    return notFound(QStringLiteral("Template '%1' not found").arg(templateId),
                    QStringLiteral("Call comfy.template.list to see imported templates."));
  }
  const TemplateInfo& tmpl = itT->second;
  if (!tmpl.outputKind.isEmpty() && tmpl.outputKind != expectedKind) {
    return invalid(QStringLiteral("Template '%1' produces %2, not %3").arg(templateId, tmpl.outputKind, expectedKind));
  }

  const TemplateReadiness ready = checkTemplateReadiness(svc, tmpl);
  if (!ready.ok) {
    return Result::failure(q("not_ready"), ready.reason,
                           QStringLiteral("Call comfy.ready for the missing nodes/models of '%1'.").arg(templateId));
  }

  const int variants = qBound(1, a.value(q("variants")).toInt(1), 4);
  const bool wait = a.value(q("wait")).toBool(true);
  const int timeoutMs = a.value(q("timeoutMs")).toInt(900000);

  const QString prompt = a.value(q("prompt")).toString();
  const QString text = a.value(q("text")).toString();
  const qint64 seed = a.contains(q("seed")) ? a.value(q("seed")).toVariant().toLongLong()
                                            : deriveSeed(templateId, prompt.isEmpty() ? text : prompt);

  const QString trackId = a.value(q("trackId")).toString();
  std::optional<Frame> nextAt;
  if (a.contains(q("atFrame"))) nextAt = a.value(q("atFrame")).toInteger();

  QJsonArray allFiles, allSidecars, allCreatedItems, allCreatedTracks;
  QString lastJobId;
  qint64 lastSeed = seed;

  for (int v = 0; v < variants; ++v) {
    const qint64 vSeed = seed + v;
    lastSeed = vSeed;

    QJsonObject tParams;
    tParams.insert(q("templateId"), templateId);
    if (!prompt.isEmpty()) tParams.insert(q("prompt"), prompt);
    if (!text.isEmpty()) tParams.insert(q("text"), text);
    tParams.insert(q("seed"), vSeed);
    for (const char* k : {"negative", "width", "height", "frames", "fps", "steps",
                          "image", "voiceRef", "durationSec", "bpm", "lyrics", "tags", "audio"}) {
      if (a.contains(q(k))) tParams.insert(q(k), a.value(q(k)));
    }
    if (a.contains(q("imageFromItemId"))) {
      const QString itmId = a.value(q("imageFromItemId")).toString();
      const Item* itm = getItem(c.session->project->doc(), itmId);
      if (!itm) {
        return notFound(QStringLiteral("Item '%1' not found").arg(itmId));
      }
      Result rRender = c.engine->call(q("frame.render"), QJsonObject{
        {q("session"), c.session->id}, {q("frame"), static_cast<double>(itm->startFrame)}
      });
      if (!rRender.ok) return rRender;
      tParams.insert(q("image"), rRender.value.toObject().value(q("path")).toString());
    }

    Result rTmpl = c.engine->call(q("comfy.graph.fromTemplate"), QJsonObject{
      {q("templateId"), templateId}, {q("params"), tParams}
    });
    if (!rTmpl.ok) return rTmpl;
    const QString graphId = rTmpl.value.toObject().value(q("graphId")).toString();

    QJsonObject runArgs{
      {q("graphId"), graphId}, {q("wait"), wait}, {q("timeoutMs"), timeoutMs},
      {q("params"), tParams}, {q("seed"), vSeed}
    };
    Result rRun = c.engine->call(q("comfy.run"), runArgs);
    if (!rRun.ok) return rRun;
    const QString jobId = rRun.value.toObject().value(q("jobId")).toString();
    lastJobId = jobId;

    auto info = c.engine->jobs().info(jobId);
    if (info && info->state == jobs::JobState::Failed) {
      return Result::failure(q("comfy_error"), info->error);
    }
    if (!info || info->state != jobs::JobState::Done) {
      return Result::success(QJsonObject{
        {q("jobId"), jobId},
        {q("seed"), lastSeed},
        {q("files"), QJsonArray{}},
        {q("created"), QJsonObject{}},
        {q("sidecars"), QJsonArray{}},
        {q("pending"), true},
        {q("hint"), QStringLiteral("call generate.collect")}
      });
    }

    const QJsonObject res = *c.engine->jobs().result(jobId);
    const QJsonArray filesArr = res.value(q("files")).toArray();
    const QString promptId = res.value(q("promptId")).toString();
    const QString workflowHash = res.value(q("workflowHash")).toString();

    QStringList files;
    for (const auto& vv : filesArr) files.append(vv.toString());

    // ComfyUI must only emit extensions this kind accepts.
    for (const QString& f : files) {
      const QString ext = QFileInfo(f).suffix().toLower();
      if (!allowedExtensions.contains(ext)) {
        return Result::failure(q("internal"), QStringLiteral("Template produced unexpected extension '%1' for kind '%2'").arg(ext, expectedKind));
      }
    }

    QJsonObject baseProv = provBase(templateId, &tmpl, expectedKind, prompt, text,
                                    a.value(q("negative")).toString(), vSeed, promptId, workflowHash);

    Result landed = landOutputs(c, files, trackId, nextAt,
                                QStringLiteral("Generate %1").arg(expectedKind),
                                expectedKind, templateId, &tmpl, baseProv);
    if (!landed.ok) return landed;
    nextAt = std::nullopt;

    const QJsonObject landedObj = landed.value.toObject();
    for (const auto& vv : landedObj.value(q("files")).toArray()) allFiles.append(vv);
    for (const auto& vv : landedObj.value(q("sidecars")).toArray()) allSidecars.append(vv);
    const QJsonObject created = landedObj.value(q("created")).toObject();
    for (const auto& vv : created.value(q("items")).toArray()) allCreatedItems.append(vv);
    for (const auto& vv : created.value(q("tracks")).toArray()) allCreatedTracks.append(vv);
  }

  QJsonObject allCreated;
  if (!allCreatedItems.isEmpty()) allCreated.insert(q("items"), allCreatedItems);
  if (!allCreatedTracks.isEmpty()) allCreated.insert(q("tracks"), allCreatedTracks);

  return Result::success(QJsonObject{
    {q("jobId"), lastJobId},
    {q("seed"), lastSeed},
    {q("files"), allFiles},
    {q("created"), allCreated},
    {q("sidecars"), allSidecars}
  });
}

// generate.thumbnail: one 1280x720 still from an image template, with a title burn painted via
// render::rasterizeItem over the generated PNG. Never touches the timeline.
Result generateThumbnail(CallContext& c, const QJsonObject& a) {
  ComfyService& svc = c.engine->comfy();
  (void)ensureSchema(svc, a);

  const QString templateId = a.value(q("templateId")).toString();
  if (templateId.isEmpty()) return invalid(QStringLiteral("Argument 'templateId' is required"));

  auto allT = loadAllTemplates();
  auto itT = allT.find(templateId);
  if (itT == allT.end()) {
    return notFound(QStringLiteral("Template '%1' not found").arg(templateId),
                    QStringLiteral("Call comfy.template.list to see imported templates."));
  }
  const TemplateInfo& tmpl = itT->second;
  if (tmpl.outputKind != QLatin1String("image")) {
    return invalid(QStringLiteral("Thumbnail requires an image template; '%1' produces %2").arg(templateId, tmpl.outputKind));
  }

  const TemplateReadiness ready = checkTemplateReadiness(svc, tmpl);
  if (!ready.ok) {
    return Result::failure(q("not_ready"), ready.reason,
                           QStringLiteral("Call comfy.ready for the missing nodes/models of '%1'.").arg(templateId));
  }

  const int timeoutMs = a.value(q("timeoutMs")).toInt(900000);
  const QString prompt = a.value(q("prompt")).toString();
  const qint64 seed = a.contains(q("seed")) ? a.value(q("seed")).toVariant().toLongLong()
                                            : deriveSeed(templateId, prompt);

  QJsonObject tParams;
  tParams.insert(q("templateId"), templateId);
  if (!prompt.isEmpty()) tParams.insert(q("prompt"), prompt);
  tParams.insert(q("seed"), seed);
  for (const char* k : {"negative", "width", "height", "steps"}) {
    if (a.contains(q(k))) tParams.insert(q(k), a.value(q(k)));
  }

  Result rTmpl = c.engine->call(q("comfy.graph.fromTemplate"), QJsonObject{
    {q("templateId"), templateId}, {q("params"), tParams}
  });
  if (!rTmpl.ok) return rTmpl;
  const QString graphId = rTmpl.value.toObject().value(q("graphId")).toString();

  QJsonObject runArgs{
    {q("graphId"), graphId}, {q("wait"), true}, {q("timeoutMs"), timeoutMs},
    {q("params"), tParams}, {q("seed"), seed}
  };
  Result rRun = c.engine->call(q("comfy.run"), runArgs);
  if (!rRun.ok) return rRun;
  const QString jobId = rRun.value.toObject().value(q("jobId")).toString();

  auto info = c.engine->jobs().info(jobId);
  if (info && info->state == jobs::JobState::Failed) {
    return Result::failure(q("comfy_error"), info->error);
  }
  if (!info || info->state != jobs::JobState::Done) {
    return Result::failure(q("internal"), QStringLiteral("Thumbnail job did not finish"));
  }

  const QJsonObject res = *c.engine->jobs().result(jobId);
  const QJsonArray filesArr = res.value(q("files")).toArray();
  if (filesArr.isEmpty()) {
    return Result::failure(q("internal"), QStringLiteral("Thumbnail job produced no output"));
  }

  const QString fPath = filesArr.at(0).toString();
  QString probeErr;
  auto mi = sf::probeMedia(fPath, &probeErr);
  if (!mi || !mi->hasVideo || mi->hasAudio) {
    return Result::failure(q("internal"), QStringLiteral("Thumbnail output is not a still image: %1").arg(fPath));
  }
  const QString ext = QFileInfo(fPath).suffix().toLower();
  if (ext != QLatin1String("png") && ext != QLatin1String("jpg") && ext != QLatin1String("webp")) {
    return Result::failure(q("invalid_params"), QStringLiteral("Unexpected thumbnail output extension '%1'").arg(ext));
  }

  QImage img(fPath);
  if (img.isNull()) {
    return Result::failure(q("internal"), QStringLiteral("Could not read thumbnail output %1").arg(fPath));
  }

  const QSize canvas(1280, 720);
  QImage base(canvas, QImage::Format_RGBA8888_Premultiplied);
  base.fill(Qt::black);
  QImage fit = img.scaled(canvas, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  QPoint offset((canvas.width() - fit.width()) / 2, (canvas.height() - fit.height()) / 2);
  {
    QPainter p(&base);
    p.drawImage(offset, fit);
    // Title burn: rasterize a caption text layer over the generated frame.
    const QString title = a.value(q("name")).toString();
    TimelineDoc doc;
    Item titleItem;
    titleItem.id = newId(q("itm"));
    titleItem.props = TextProps{title.isEmpty() ? prompt : title};
    if (auto layer = sf::render::rasterizeItem(doc, titleItem, 0, canvas)) {
      QPainter p2(&base);
      p2.drawImage(layer->origin, layer->image);
    }
  }

  QString stem = a.value(q("name")).toString();
  if (stem.isEmpty()) stem = QStringLiteral("%1_thumb_%2").arg(templateId).arg(seed, 0, 16);
  QDir outDir;
  if (!c.session->project->path().isEmpty()) {
    outDir = QDir(QFileInfo(c.session->project->path()).dir());
    outDir.mkpath(QStringLiteral("generated/thumbs"));
  } else {
    outDir = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    outDir.mkpath(QStringLiteral("SplitFrame/generated/thumbs"));
  }
  const QString thumbPath = outDir.filePath(stem + QStringLiteral(".png"));
  if (!base.save(thumbPath, "PNG")) {
    return Result::failure(q("internal"), QStringLiteral("Could not write thumbnail %1").arg(thumbPath));
  }

  return Result::success(QJsonObject{
    {q("templateId"), templateId},
    {q("seed"), seed},
    {q("files"), QJsonArray{thumbPath}},
    {q("sidecars"), QJsonArray{}}
  });
}

Result reshootVariantsCmd(CallContext& c, const QJsonObject& a) {
  const QString itemId = a.value(q("itemId")).toString();
  if (itemId.isEmpty()) return invalid(QStringLiteral("Argument 'itemId' is required"));

  const TimelineDoc& doc = c.session->project->doc();
  const Item* itm = getItem(doc, itemId);
  if (!itm) return notFound(QStringLiteral("Item %1 not found").arg(itemId));

  if (!itm->assetId || itm->assetId->isEmpty()) {
    return invalid(QStringLiteral("Item %1 was not generated by SplitFrame (no .sfgen.json)").arg(itemId));
  }
  const Asset* as = c.session->project->asset(*itm->assetId);
  if (!as) {
    return invalid(QStringLiteral("Item %1 was not generated by SplitFrame (no .sfgen.json)").arg(itemId));
  }

  auto scOpt = readSidecar(as->path);
  if (!scOpt) {
    return invalid(QStringLiteral("Item %1 was not generated by SplitFrame (no .sfgen.json)").arg(itemId));
  }

  const QJsonObject sidecar = *scOpt;
  const QJsonObject scParams = sidecar.value(q("params")).toObject();
  const qint64 origSeed = scParams.value(q("seed")).toVariant().toLongLong();
  const QString origPrompt = scParams.value(q("prompt")).toString();
  const QString templateId = scParams.value(q("templateId")).toString(QStringLiteral("sd15_txt2img"));

  const int count = std::clamp(a.value(q("count")).toInt(4), 2, 8);
  const QString vary = a.value(q("vary")).toString(QStringLiteral("seed"));
  const QJsonArray editsArr = a.value(q("promptEdits")).toArray();
  QStringList promptEdits;
  for (const auto& pe : editsArr) promptEdits.append(pe.toString());

  const bool wait = a.value(q("wait")).toBool(true);
  const int timeoutMs = a.value(q("timeoutMs")).toInt(1800000);

  QJsonArray variantsArr;
  QJsonArray pendingJobIds;
  std::vector<QImage> cellImages;
  QStringList cellLabels;

  for (int i = 0; i < count; ++i) {
    qint64 varSeed = origSeed;
    if (vary == QLatin1String("seed") || vary == QLatin1String("both")) {
      varSeed = origSeed + 1 + i;
    }

    QString varPrompt = origPrompt;
    QString editUsed;
    if ((vary == QLatin1String("prompt") || vary == QLatin1String("both")) && !promptEdits.isEmpty()) {
      editUsed = promptEdits[i % promptEdits.size()];
      varPrompt = origPrompt + QStringLiteral(", ") + editUsed;
    }

    QJsonObject varParams = scParams;
    varParams.remove(q("templateId"));
    varParams.insert(q("seed"), varSeed);
    varParams.insert(q("prompt"), varPrompt);

    Result rTmpl = c.engine->call(q("comfy.graph.fromTemplate"), QJsonObject{
      {q("templateId"), templateId},
      {q("params"), varParams}
    });
    if (!rTmpl.ok) return rTmpl;
    const QString graphId = rTmpl.value.toObject().value(q("graphId")).toString();

    QJsonObject jobParams = varParams;
    jobParams.insert(q("templateId"), templateId);
    QJsonObject runArgs{
      {q("graphId"), graphId},
      {q("wait"), wait},
      {q("timeoutMs"), timeoutMs},
      {q("params"), jobParams},
      {q("seed"), varSeed}
    };
    Result rRun = c.engine->call(q("comfy.run"), runArgs);
    if (!rRun.ok) return rRun;
    const QString jobId = rRun.value.toObject().value(q("jobId")).toString();

    auto info = c.engine->jobs().info(jobId);
    if (info && info->state == jobs::JobState::Failed) {
      return Result::failure(q("comfy_error"), info->error);
    }
    if (!info || info->state != jobs::JobState::Done) {
      pendingJobIds.append(jobId);
      continue;
    }

    auto res = c.engine->jobs().result(jobId);
    QJsonArray files = res ? res->value(QStringLiteral("files")).toArray() : QJsonArray{};
    const QString chosenFile = files.isEmpty() ? QString() : files.at(0).toString();

    const char letterChar = static_cast<char>('A' + i);
    const QString letter = QString(QChar(letterChar));
    QString label;
    if (!editUsed.isEmpty()) {
      label = QStringLiteral("%1 seed %2 · %3").arg(letter).arg(varSeed).arg(editUsed);
    } else {
      label = QStringLiteral("%1 seed %2").arg(letter).arg(varSeed);
    }

    QImage still = getStillForFile(chosenFile);
    cellImages.push_back(still);
    cellLabels.append(label);

    QJsonObject varObj{
      {q("letter"), letter},
      {q("seed"), varSeed},
      {q("prompt"), varPrompt},
      {q("file"), chosenFile},
      {q("jobId"), jobId}
    };
    variantsArr.append(varObj);
  }

  // Nothing completed yet (wait:false): do NOT overwrite the stored reshoot for this item —
  // reshoot.pick reads it and would see an empty variant list forever.
  if (variantsArr.isEmpty()) {
    return Result::success(QJsonObject{
      {q("sheetPath"), QString()},
      {q("variants"), QJsonArray{}},
      {q("pending"), pendingJobIds}
    });
  }

  QString sheetPath;
  if (!cellImages.empty()) {
    QImage sheet = composeGrid(cellImages, cellLabels, 0, 480);
    sheetPath = QDir::temp().filePath(QStringLiteral("reshoot_%1.png").arg(itemId));
    QDir().mkpath(QFileInfo(sheetPath).absolutePath());
    sheet.save(sheetPath, "PNG");
  }

  QJsonObject ret{
    {q("sheetPath"), QDir::toNativeSeparators(sheetPath)},
    {q("variants"), variantsArr},
    {q("original"), QJsonObject{{q("seed"), origSeed}, {q("prompt"), origPrompt}}}
  };
  if (!pendingJobIds.isEmpty()) {
    ret.insert(q("pending"), pendingJobIds);
  }

  c.engine->comfy().lastReshoot[itemId] = ret;
  return Result::success(ret);
}

Result reshootPickCmd(CallContext& c, const QJsonObject& a) {
  const QString itemId = a.value(q("itemId")).toString();
  if (itemId.isEmpty()) return invalid(QStringLiteral("Argument 'itemId' is required"));

  ComfyService& svc = c.engine->comfy();
  auto itReshoot = svc.lastReshoot.find(itemId);
  if (itReshoot == svc.lastReshoot.end()) {
    return invalid(QStringLiteral("Run reshoot.variants first"));
  }

  const QString letter = a.value(q("letter")).toString().toUpper();
  if (letter.isEmpty()) return invalid(QStringLiteral("Argument 'letter' is required"));

  const QJsonArray variants = itReshoot->second.value(q("variants")).toArray();
  QJsonObject chosenVariant;
  bool foundVariant = false;
  for (const auto& v : variants) {
    if (v.toObject().value(q("letter")).toString() == letter) {
      chosenVariant = v.toObject();
      foundVariant = true;
      break;
    }
  }
  if (!foundVariant) {
    return invalid(QStringLiteral("Variant '%1' not found in last reshoot").arg(letter));
  }

  const QString chosenFile = chosenVariant.value(q("file")).toString();
  const qint64 seed = chosenVariant.value(q("seed")).toVariant().toLongLong();

  c.session->pool->import({chosenFile});
  c.session->pool->waitForIdle(60000);

  const Asset* newAsset = nullptr;
  const QFileInfo cfi(chosenFile);
  for (const Asset& as : c.session->project->assets()) {
    if (QFileInfo(as.path).absoluteFilePath() == cfi.absoluteFilePath() || as.path == chosenFile) {
      newAsset = &as;
      break;
    }
  }
  if (!newAsset) {
    return Result::failure(q("internal"), QStringLiteral("Failed to import %1").arg(chosenFile));
  }

  const TimelineDoc& doc = c.session->project->doc();
  const Item* curItem = getItem(doc, itemId);
  if (!curItem) return notFound(QStringLiteral("Item %1 not found").arg(itemId));

  Item newItem = *curItem;
  newItem.assetId = newAsset->id;

  const bool keepTiming = a.value(q("keepTiming")).toBool(true);
  if (!keepTiming) {
    newItem.sourceInFrame = 0;
  }

  QJsonArray notes;
  const double fps = static_cast<double>(doc.project.fps);
  if (newAsset->durationMs > 0) {
    const Frame assetFrames = jsRound(static_cast<double>(newAsset->durationMs) / 1000.0 * fps);
    const Frame inF = newItem.sourceInFrame.value_or(0);
    const Frame maxAllowed = std::max<Frame>(1, assetFrames - inF);
    if (newItem.durationFrames > maxAllowed) {
      newItem.durationFrames = maxAllowed;
      notes.append(QStringLiteral("Duration capped to new media length (%1 frames)").arg(assetFrames));
    }
  }

  std::vector<Op> ops;
  ops.push_back(Op(ItemRemove{{itemId}, false}));
  ops.push_back(Op(ItemAdd{newItem, false}));

  Result r = applyToSession(c, ops, QStringLiteral("reshoot.pick %1").arg(letter), {{q("itemId"), itemId}, {q("letter"), letter}});
  if (!r.ok) return r;

  return Result::success(QJsonObject{
    {q("itemId"), itemId},
    {q("assetId"), newAsset->id},
    {q("file"), chosenFile},
    {q("seed"), seed},
    {q("notes"), notes}
  });
}

} // namespace

void registerComfyCommands(Engine& e) {
  e.add(spec("comfy.server.status", "", "comfy", false,
             QStringLiteral("Query managed ComfyUI process lifecycle state, pid, uptime, and configuration."),
             js::obj({}),
             [](CallContext& c, const QJsonObject&) {
               ComfyService& svc = c.engine->comfy();
               return Result::success(svc.process->statusObject());
             },
             false));

  e.add(spec("comfy.server.start", "", "comfy", false,
             QStringLiteral("Start managed ComfyUI process and wait until Ready or failure."),
             js::obj({{q("home"), js::str(QStringLiteral("ComfyUI home directory"))},
                      {q("python"), js::str(QStringLiteral("Python interpreter path"))},
                      {q("args"), js::array(js::str(), QStringLiteral("Extra command-line arguments"))},
                      {q("save"), js::boolean(QStringLiteral("Persist home, python, and args to QSettings (default false)"))},
                      {q("timeoutMs"), js::integer(QStringLiteral("Ready timeout in milliseconds (default 180000)"), 1000, 600000)}}),
             [](CallContext& c, const QJsonObject& a) {
               ComfyService& svc = c.engine->comfy();
               const QString home = a.value(q("home")).toString();
               const QString python = a.value(q("python")).toString();
               QStringList args;
               if (a.contains(q("args"))) {
                 for (const auto& v : a.value(q("args")).toArray()) {
                   args.append(v.toString());
                 }
               }
               const bool save = a.value(q("save")).toBool(false);
               const int timeoutMs = a.value(q("timeoutMs")).toInt(180000);
               auto res = svc.process->start(home, python, args, save, timeoutMs);
               if (!res.ok) {
                 return Result::failure(res.errorCode, res.errorMessage, res.status);
               }
               return Result::success(res.status);
             },
             false));

  e.add(spec("comfy.server.stop", "", "comfy", false,
             QStringLiteral("Stop managed ComfyUI process."),
             js::obj({}),
             [](CallContext& c, const QJsonObject&) {
               ComfyService& svc = c.engine->comfy();
               if (svc.process->modeString() == QLatin1String("external")) {
                 return invalid(QStringLiteral("Cannot stop external ComfyUI server"));
               }
               svc.process->stop();
               return Result::success(svc.process->statusObject());
             },
             false));

  e.add(spec("comfy.server.restart", "", "comfy", false,
             QStringLiteral("Restart managed ComfyUI process."),
             js::obj({}),
             [](CallContext& c, const QJsonObject&) {
               ComfyService& svc = c.engine->comfy();
               if (svc.process->modeString() == QLatin1String("external")) {
                 return invalid(QStringLiteral("Cannot restart external ComfyUI server"));
               }
               auto res = svc.process->restart();
               if (!res.ok) {
                 return Result::failure(res.errorCode, res.errorMessage, res.status);
               }
               return Result::success(res.status);
             },
             false));

  e.add(spec("comfy.server.logs", "", "comfy", false,
             QStringLiteral("Retrieve recent stdout/stderr log lines from ComfyUI process."),
             js::obj({{q("lines"), js::integer(QStringLiteral("Number of log lines (default 100, max 500)"), 1, 500)}}),
             [](CallContext& c, const QJsonObject& a) {
               ComfyService& svc = c.engine->comfy();
               int lines = a.value(q("lines")).toInt(100);
               if (lines < 1) lines = 100;
               if (lines > 500) lines = 500;
               return Result::success(QJsonObject{
                 {q("lines"), QJsonArray::fromStringList(svc.process->recentLogs(lines))}
               });
             },
             false));

  e.add(spec("comfy.status", "", "comfy", false,
             QStringLiteral("Query ComfyUI server reachability, base URL, system stats, and schema cache status. Never fails."),
             js::obj({}),
             [](CallContext& c, const QJsonObject&) {
               ComfyService& svc = c.engine->comfy();
               QString err;
               auto stats = svc.client.systemStats(&err, 1500);
               const bool reachable = stats.has_value();

               QJsonObject ret;
               ret.insert(q("reachable"), reachable);
               ret.insert(q("base"), svc.client.base().toString());
               if (reachable) {
                 ret.insert(q("systemStats"), *stats);
               } else {
                 ret.insert(q("error"), err);
               }
               ret.insert(q("schemaLoaded"), svc.schema.has_value());
               ret.insert(q("schemaSource"), svc.schemaSource);
               ret.insert(q("nodeCount"), svc.schema ? svc.schema->size() : 0);
               return Result::success(ret);
             },
             false));

  e.add(spec("comfy.nodes", "", "comfy", false,
             QStringLiteral("List available ComfyUI node classes filtered by search substring or category prefix. Edits and schemas are validated deterministically by code, not a model."),
             js::obj({{q("search"), js::str(QStringLiteral("Case-insensitive substring of classType, displayName, or category"))},
                      {q("category"), js::str(QStringLiteral("Category prefix filter"))},
                      {q("limit"), js::integer(QStringLiteral("Maximum nodes to return (default 50)"), 1, 1000)},
                      {q("objectInfoPath"), js::str(QStringLiteral("Local JSON file to load object_info from"))},
                      {q("refresh"), js::boolean(QStringLiteral("Force reload schema cache"))}}),
             [](CallContext& c, const QJsonObject& a) {
               ComfyService& svc = c.engine->comfy();
               Result r = ensureSchema(svc, a);
               if (!r.ok) return r;

               const QString search = a.value(q("search")).toString();
               const QString category = a.value(q("category")).toString();
               const int limit = a.value(q("limit")).toInt(50);

               QJsonArray nodesArr;
               int totalCount = 0;
               const QStringList classes = svc.schema->classTypes();
               for (const QString& cls : classes) {
                 const comfy::NodeSchema* ns = svc.schema->find(cls);
                 if (!ns) continue;

                 if (!category.isEmpty() && !ns->category.startsWith(category, Qt::CaseInsensitive)) {
                   continue;
                 }

                 if (!search.isEmpty()) {
                   const bool match = ns->classType.contains(search, Qt::CaseInsensitive) ||
                                      ns->displayName.contains(search, Qt::CaseInsensitive) ||
                                      ns->category.contains(search, Qt::CaseInsensitive);
                   if (!match) continue;
                 }

                 ++totalCount;
                 if (nodesArr.size() < limit) {
                   QJsonArray inNames;
                   for (const auto& in : ns->inputs) inNames.append(in.name);
                   QJsonArray outTypes;
                   for (const auto& out : ns->outputs) outTypes.append(out.type);

                   nodesArr.append(QJsonObject{
                     {q("classType"), ns->classType},
                     {q("displayName"), ns->displayName},
                     {q("category"), ns->category},
                     {q("outputNode"), ns->outputNode},
                     {q("inputs"), inNames},
                     {q("outputs"), outTypes}
                   });
                 }
               }

               return Result::success(QJsonObject{
                 {q("count"), totalCount},
                 {q("nodes"), nodesArr}
               });
             },
             false));

  e.add(spec("comfy.node.schema", "", "comfy", false,
             QStringLiteral("Get the complete typed schema for a ComfyUI node class including toolSchema for agent calls. Edits and schemas are validated deterministically by code, not a model."),
             js::obj({{q("classType"), js::str(QStringLiteral("ComfyUI node class name"))},
                      {q("objectInfoPath"), js::str(QStringLiteral("Local JSON file to load object_info from"))},
                      {q("refresh"), js::boolean(QStringLiteral("Force reload schema cache"))}},
                     {q("classType")}),
             [](CallContext& c, const QJsonObject& a) {
               ComfyService& svc = c.engine->comfy();
               Result r = ensureSchema(svc, a);
               if (!r.ok) return r;

               const QString classType = a.value(q("classType")).toString();
               const comfy::NodeSchema* ns = svc.schema->find(classType);
               if (!ns) {
                 return notFound(QStringLiteral("Node class '%1' not found").arg(classType),
                                 QStringLiteral("Call comfy.nodes to see available classes."));
               }

               QJsonArray inputsArr;
               for (const auto& in : ns->inputs) {
                 QJsonObject inObj;
                 inObj.insert(q("name"), in.name);
                 inObj.insert(q("required"), in.required);
                 inObj.insert(q("kind"), kindToString(in.kind));
                 if (in.kind == comfy::InputKind::Link && !in.linkTypes.isEmpty()) {
                   inObj.insert(q("linkTypes"), QJsonArray::fromStringList(in.linkTypes));
                 }
                 if (in.kind == comfy::InputKind::Combo && !in.options.isEmpty()) {
                   inObj.insert(q("options"), QJsonArray::fromStringList(in.options));
                 }
                 if (in.min) inObj.insert(q("min"), *in.min);
                 if (in.max) inObj.insert(q("max"), *in.max);
                 if (in.step) inObj.insert(q("step"), *in.step);
                 if (!in.defaultValue.isUndefined()) inObj.insert(q("default"), in.defaultValue);
                 if (in.multiline) inObj.insert(q("multiline"), in.multiline);
                 inputsArr.append(inObj);
               }

               QJsonArray outputsArr;
               for (const auto& out : ns->outputs) {
                 outputsArr.append(QJsonObject{
                   {q("type"), out.type},
                   {q("name"), out.name},
                   {q("isList"), out.isList}
                 });
               }

               return Result::success(QJsonObject{
                 {q("classType"), ns->classType},
                 {q("displayName"), ns->displayName},
                 {q("category"), ns->category},
                 {q("description"), ns->description},
                 {q("outputNode"), ns->outputNode},
                 {q("inputs"), inputsArr},
                 {q("outputs"), outputsArr},
                 {q("toolSchema"), comfy::toolSchema(*ns)}
               });
             },
             false));

  e.add(spec("comfy.graph.new", "", "comfy", false,
             QStringLiteral("Create a new empty ComfyUI workflow graph document in memory. Edits and schemas are validated deterministically by code, not a model."),
             js::obj({{q("name"), js::str(QStringLiteral("Optional descriptive document name"))}}),
             [](CallContext& c, const QJsonObject& a) {
               ComfyService& svc = c.engine->comfy();
               const QString gid = QStringLiteral("g%1").arg(svc.nextGraph++);
               svc.graphs[gid] = ComfyService::Doc{comfy::Graph{}, {}, a.value(q("name")).toString()};
               return Result::success(QJsonObject{{q("graphId"), gid}});
             },
             false));

  e.add(spec("comfy.graph.load", "", "comfy", false,
             QStringLiteral("Load a ComfyUI workflow graph from API-format JSON or a file path. Edits and schemas are validated deterministically by code, not a model."),
             js::obj({{q("json"), js::any(QStringLiteral("API-format ComfyUI workflow object"))},
                      {q("path"), js::str(QStringLiteral("File path to an API-format workflow JSON"))},
                      {q("name"), js::str(QStringLiteral("Optional descriptive document name"))}}),
             [](CallContext& c, const QJsonObject& a) {
               ComfyService& svc = c.engine->comfy();
               QJsonObject graphJson;
               if (a.contains(q("json"))) {
                 if (!a.value(q("json")).isObject()) {
                   return invalid(QStringLiteral("Argument 'json' must be an object"));
                 }
                 graphJson = a.value(q("json")).toObject();
               } else if (a.contains(q("path"))) {
                 const QString path = a.value(q("path")).toString();
                 QFile f(path);
                 if (!f.open(QIODevice::ReadOnly)) {
                   return invalid(QStringLiteral("Could not read graph file: %1").arg(path));
                 }
                 QJsonParseError parseErr;
                 const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseErr);
                 if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
                   return invalid(QStringLiteral("Invalid JSON in graph file %1: %2").arg(path, parseErr.errorString()));
                 }
                 graphJson = doc.object();
               } else {
                 return invalid(QStringLiteral("Provide 'json' or 'path'"));
               }

               QString err;
               auto parsed = comfy::Graph::fromApiJson(graphJson, &err);
               if (!parsed) {
                 return invalid(err);
               }

               const QString gid = QStringLiteral("g%1").arg(svc.nextGraph++);
               const int count = static_cast<int>(parsed->nodes().size());
               svc.graphs[gid] = ComfyService::Doc{std::move(*parsed), {}, a.value(q("name")).toString()};
               return Result::success(QJsonObject{
                 {q("graphId"), gid},
                 {q("nodeCount"), count}
               });
             },
             false));

  e.add(spec("comfy.graph.get", "", "comfy", false,
             QStringLiteral("Retrieve a ComfyUI workflow graph in API JSON format. Edits and schemas are validated deterministically by code, not a model."),
             js::obj({{q("graphId"), js::str(QStringLiteral("Graph document ID"))}}, {q("graphId")}),
             [](CallContext& c, const QJsonObject& a) {
               ComfyService& svc = c.engine->comfy();
               const QString gid = a.value(q("graphId")).toString();
               auto it = svc.graphs.find(gid);
               if (it == svc.graphs.end()) {
                 return notFound(QStringLiteral("Graph '%1' not found").arg(gid),
                                 QStringLiteral("Call comfy.graph.list to see open graphs."));
               }
               return Result::success(QJsonObject{
                 {q("graphId"), gid},
                 {q("name"), it->second.name},
                 {q("graph"), it->second.graph.toApiJson()},
                 {q("nodeCount"), static_cast<int>(it->second.graph.nodes().size())}
               });
             },
             false));

  e.add(spec("comfy.graph.op", "", "comfy", false,
             QStringLiteral("Apply a batch of operations atomically to a ComfyUI workflow graph with undo tracking and validation. Edits are validated deterministically by code, not a model."),
             js::obj({{q("graphId"), js::str(QStringLiteral("Graph document ID"))},
                      {q("ops"), js::array(js::any(), QStringLiteral("Array of graph operations"))},
                      {q("objectInfoPath"), js::str(QStringLiteral("Local JSON file to load object_info from"))},
                      {q("refresh"), js::boolean(QStringLiteral("Force reload schema cache"))}},
                     {q("graphId"), q("ops")}),
             [](CallContext& c, const QJsonObject& a) {
               ComfyService& svc = c.engine->comfy();
               const QString gid = a.value(q("graphId")).toString();
               auto it = svc.graphs.find(gid);
               if (it == svc.graphs.end()) {
                 return notFound(QStringLiteral("Graph '%1' not found").arg(gid),
                                 QStringLiteral("Call comfy.graph.list to see open graphs."));
               }
               if (!a.value(q("ops")).isArray()) {
                 return invalid(QStringLiteral("Argument 'ops' must be an array"));
               }

               auto opRes = comfy::applyOps(it->second.graph, a.value(q("ops")).toArray());
               if (!opRes.ok) {
                 return invalid(opRes.error);
               }

               it->second.undo.push_back(std::move(it->second.graph));
               it->second.graph = std::move(opRes.graph);

               ensureSchema(svc, a);
               std::vector<comfy::Issue> issues;
               if (svc.schema) {
                 issues = comfy::validate(it->second.graph, *svc.schema);
               }

               return Result::success(QJsonObject{
                 {q("graphId"), gid},
                 {q("createdIds"), QJsonArray::fromStringList(opRes.createdIds)},
                 {q("nodeCount"), static_cast<int>(it->second.graph.nodes().size())},
                 {q("issues"), comfy::issuesToJson(issues)}
               });
             },
             false));

  e.add(spec("comfy.graph.undo", "", "comfy", false,
             QStringLiteral("Undo the last batch of operations on a ComfyUI workflow graph. Edits and schemas are validated deterministically by code, not a model."),
             js::obj({{q("graphId"), js::str(QStringLiteral("Graph document ID"))}}, {q("graphId")}),
             [](CallContext& c, const QJsonObject& a) {
               ComfyService& svc = c.engine->comfy();
               const QString gid = a.value(q("graphId")).toString();
               auto it = svc.graphs.find(gid);
               if (it == svc.graphs.end()) {
                 return notFound(QStringLiteral("Graph '%1' not found").arg(gid),
                                 QStringLiteral("Call comfy.graph.list to see open graphs."));
               }

               if (it->second.undo.empty()) {
                 return Result::success(QJsonObject{
                   {q("graphId"), gid},
                   {q("undone"), false},
                   {q("nodeCount"), static_cast<int>(it->second.graph.nodes().size())}
                 });
               }

               it->second.graph = std::move(it->second.undo.back());
               it->second.undo.pop_back();

               return Result::success(QJsonObject{
                 {q("graphId"), gid},
                 {q("undone"), true},
                 {q("nodeCount"), static_cast<int>(it->second.graph.nodes().size())}
               });
             },
             false));

  e.add(spec("comfy.graph.validate", "", "comfy", false,
             QStringLiteral("Validate a ComfyUI workflow graph against node schemas. Validation is performed deterministically by code, not a model."),
             js::obj({{q("graphId"), js::str(QStringLiteral("Graph document ID"))},
                      {q("objectInfoPath"), js::str(QStringLiteral("Local JSON file to load object_info from"))},
                      {q("refresh"), js::boolean(QStringLiteral("Force reload schema cache"))}},
                     {q("graphId")}),
             [](CallContext& c, const QJsonObject& a) {
               ComfyService& svc = c.engine->comfy();
               const QString gid = a.value(q("graphId")).toString();
               auto it = svc.graphs.find(gid);
               if (it == svc.graphs.end()) {
                 return notFound(QStringLiteral("Graph '%1' not found").arg(gid),
                                 QStringLiteral("Call comfy.graph.list to see open graphs."));
               }

               Result r = ensureSchema(svc, a);
               if (!r.ok) return r;

               const auto issues = comfy::validate(it->second.graph, *svc.schema);
               return Result::success(QJsonObject{
                 {q("graphId"), gid},
                 {q("valid"), issues.empty()},
                 {q("issues"), comfy::issuesToJson(issues)}
               });
             },
             false));

  e.add(spec("comfy.graph.save", "", "comfy", false,
             QStringLiteral("Save a ComfyUI workflow graph to disk in indented API JSON format. Edits and schemas are validated deterministically by code, not a model."),
             js::obj({{q("graphId"), js::str(QStringLiteral("Graph document ID"))},
                      {q("path"), js::str(QStringLiteral("File path to write"))}},
                     {q("graphId"), q("path")}),
             [](CallContext& c, const QJsonObject& a) {
               ComfyService& svc = c.engine->comfy();
               const QString gid = a.value(q("graphId")).toString();
               auto it = svc.graphs.find(gid);
               if (it == svc.graphs.end()) {
                 return notFound(QStringLiteral("Graph '%1' not found").arg(gid),
                                 QStringLiteral("Call comfy.graph.list to see open graphs."));
               }

               const QString path = a.value(q("path")).toString();
               if (path.isEmpty()) {
                 return invalid(QStringLiteral("Argument 'path' cannot be empty"));
               }

               QFile f(path);
               if (!f.open(QIODevice::WriteOnly)) {
                 return Result::failure(q("io_error"), QStringLiteral("Could not open file for writing: %1").arg(path));
               }

               const QByteArray bytes = QJsonDocument(it->second.graph.toApiJson()).toJson(QJsonDocument::Indented);
               if (f.write(bytes) != bytes.size()) {
                 return Result::failure(q("io_error"), QStringLiteral("Failed to write graph to %1").arg(path));
               }

               return Result::success(QJsonObject{
                 {q("path"), path},
                 {q("bytes"), static_cast<int>(bytes.size())}
               });
             },
             false));

  e.add(spec("comfy.graph.list", "", "comfy", false,
             QStringLiteral("List all currently open in-memory ComfyUI workflow graph documents. Edits and schemas are validated deterministically by code, not a model."),
             js::obj({}),
             [](CallContext& c, const QJsonObject&) {
               ComfyService& svc = c.engine->comfy();
               QJsonArray graphsArr;
               for (const auto& [gid, doc] : svc.graphs) {
                 graphsArr.append(QJsonObject{
                   {q("graphId"), gid},
                   {q("name"), doc.name},
                   {q("nodeCount"), static_cast<int>(doc.graph.nodes().size())}
                 });
               }
               return Result::success(QJsonObject{{q("graphs"), graphsArr}});
             },
             false));

  e.add(spec("comfy.template.list", "", "comfy", false,
             QStringLiteral("List available ComfyUI workflow templates (built-in and user-imported). Templates are loaded deterministically from disk."),
             js::obj({{q("kind"), js::str(QStringLiteral("Filter templates by kind ('image', 'video', 'speech', 'music')"))}}),
             [](CallContext&, const QJsonObject& a) {
               auto allTemplates = loadAllTemplates();
               const QString kindFilter = a.value(q("kind")).toString();
               QJsonArray tArr;
               for (const auto& [tid, t] : allTemplates) {
                 if (!kindFilter.isEmpty() && t.kind != kindFilter) continue;
                 QJsonArray bNames;
                 for (auto it = t.bindings.begin(); it != t.bindings.end(); ++it) {
                   bNames.append(it.key());
                 }
                 tArr.append(QJsonObject{
                   {q("id"), t.id},
                   {q("name"), t.name},
                   {q("kind"), t.kind},
                   {q("description"), t.description},
                   {q("whenToUse"), t.whenToUse},
                   {q("bindings"), bNames},
                   {q("source"), t.source},
                   {q("outputNode"), t.outputNode},
                   {q("outputKind"), t.outputKind},
                   {q("requires"), t.required},
                   {q("needsUserModel"), t.needsUserModel}
                 });
               }
               return Result::success(QJsonObject{{q("templates"), tArr}});
             },
             false));

  e.add(spec("comfy.ready", "", "comfy", false,
             QStringLiteral("Report which generation templates can run on the configured ComfyUI: per template, the missing node classes and model files, from the cached /object_info."),
             js::obj({{q("templateId"), js::str(QStringLiteral("Check a single template instead of every one"))},
                      {q("objectInfoPath"), js::str(QStringLiteral("Path to an object_info.json to use instead of the live server"))},
                      {q("refresh"), js::boolean(QStringLiteral("Re-fetch the schema even when a copy is cached (default false)"))}}),
             [](CallContext& c, const QJsonObject& a) {
               ComfyService& svc = c.engine->comfy();
               ensureSchema(svc, a); // best effort: without a schema every row reports why it cannot check
               auto allTemplates = loadAllTemplates();
               const QString one = a.value(q("templateId")).toString();
               if (!one.isEmpty() && !allTemplates.contains(one)) {
                 return notFound(QStringLiteral("Template '%1' not found").arg(one),
                                 QStringLiteral("Call comfy.template.list to see imported templates."));
               }
               QJsonArray rows;
               for (const auto& [tid, t] : allTemplates) {
                 if (!one.isEmpty() && tid != one) continue;
                 const TemplateReadiness r = checkTemplateReadiness(svc, t);
                 rows.append(QJsonObject{
                   {q("id"), t.id},
                   {q("kind"), t.kind},
                   {q("ok"), r.ok},
                   {q("missingNodes"), QJsonArray::fromStringList(r.missingNodes)},
                   {q("missingModels"), r.missingModels},
                   {q("reason"), r.reason}
                 });
               }
               if (!one.isEmpty()) return Result::success(rows.at(0).toObject());
               return Result::success(QJsonObject{{q("templates"), rows}});
             },
             false));

  e.add(spec("comfy.template.import", "", "comfy", false,
             QStringLiteral("Import a ComfyUI API workflow graph as a template with typed bindings. Validates that referenced nodes and inputs exist."),
             js::obj({{q("id"), js::str(QStringLiteral("Template identifier"))},
                      {q("name"), js::str(QStringLiteral("Human-readable template name"))},
                      {q("kind"), js::str(QStringLiteral("Media kind: 'image', 'video', 'speech', or 'music'"))},
                      {q("outputNode"), js::str(QStringLiteral("ID of node producing the final output"))},
                      {q("bindings"), js::any(QStringLiteral("Map of parameter names to {node, input}"))},
                      {q("path"), js::str(QStringLiteral("Path to ComfyUI API JSON file"))},
                      {q("graph"), js::any(QStringLiteral("Inlined ComfyUI API JSON graph"))},
                      {q("description"), js::str(QStringLiteral("Template description"))},
                      {q("whenToUse"), js::array(js::str(), QStringLiteral("Keywords for when to use this template"))},
                      {q("outputKind"), js::str(QStringLiteral("File kind this workflow produces: 'video', 'image', or 'audio'; derived from kind when omitted"))},
                      {q("requires"), js::any(QStringLiteral("Optional {nodes: [class_type...], models: [{folder, file}...]} the ComfyUI install must have"))},
                      {q("needsUserModel"), js::boolean(QStringLiteral("True when the template's model files are placeholders the user must have (default false)"))}},
                     {q("id"), q("name"), q("kind"), q("outputNode"), q("bindings")}),
             [](CallContext&, const QJsonObject& a) {
               const QString id = a.value(q("id")).toString();
               if (id.isEmpty()) return invalid(QStringLiteral("Argument 'id' is required"));
               static const QRegularExpression kValidId(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
               if (!kValidId.match(id).hasMatch()) {
                 return invalid(QStringLiteral("Invalid template id: must match ^[A-Za-z0-9_-]{1,64}$"));
               }
               const QString name = a.value(q("name")).toString();
               const QString kind = a.value(q("kind")).toString();
               if (!kTemplateKinds.contains(kind)) {
                 return invalid(QStringLiteral("Invalid kind '%1': must be one of image, video, speech, music").arg(kind));
               }
               const QString outputNode = a.value(q("outputNode")).toString();
               const QJsonObject bindings = a.value(q("bindings")).toObject();
               QString outputKind = a.value(q("outputKind")).toString();
               if (!outputKind.isEmpty() && outputKind != QLatin1String("video")
                   && outputKind != QLatin1String("image") && outputKind != QLatin1String("audio")) {
                 return invalid(QStringLiteral("Invalid outputKind '%1': must be video, image, or audio").arg(outputKind));
               }
               if (outputKind.isEmpty()) outputKind = deriveOutputKind(kind);
               const QJsonObject required = a.value(q("requires")).toObject();
               const QJsonArray reqNodes = required.value(q("nodes")).toArray();
               for (const QJsonValue& n : reqNodes) {
                 if (!n.isString()) {
                   return invalid(QStringLiteral("requires.nodes must be an array of node class_type strings"));
                 }
               }
               const QJsonArray reqModels = required.value(q("models")).toArray();
               for (const QJsonValue& m : reqModels) {
                 if (!m.isObject()
                     || !m.toObject().value(q("folder")).isString()
                     || !m.toObject().value(q("file")).isString()) {
                   return invalid(QStringLiteral("requires.models must be an array of {folder, file} objects"));
                 }
               }
               const bool needsUserModel = a.value(q("needsUserModel")).toBool();
               QJsonObject apiGraph;

               if (a.contains(q("graph"))) {
                 apiGraph = a.value(q("graph")).toObject();
               } else if (a.contains(q("path"))) {
                 const QString p = a.value(q("path")).toString();
                 QFile f(p);
                 if (!f.open(QIODevice::ReadOnly)) {
                   return Result::failure(q("io_error"), QStringLiteral("Could not read graph file: %1").arg(p));
                 }
                 QJsonParseError pe;
                 const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
                 if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
                   return invalid(QStringLiteral("Invalid JSON in graph file %1: %2").arg(p, pe.errorString()));
                 }
                 apiGraph = doc.object();
               } else {
                 return invalid(QStringLiteral("Must pass 'graph' or 'path'"));
               }

               if (!apiGraph.contains(outputNode)) {
                 return invalid(QStringLiteral("outputNode '%1' does not exist in graph").arg(outputNode));
               }

               for (auto it = bindings.begin(); it != bindings.end(); ++it) {
                 const QString bName = it.key();
                 if (!kAllowedBindings.contains(bName)) {
                   return invalid(QStringLiteral("Invalid binding name '%1'").arg(bName));
                 }
                 if (!it.value().isObject()) {
                   return invalid(QStringLiteral("Binding '%1' must be an object").arg(bName));
                 }
                 const QJsonObject bObj = it.value().toObject();
                 const QString bNode = bObj.value(q("node")).toString();
                 const QString bInput = bObj.value(q("input")).toString();
                 if (!apiGraph.contains(bNode)) {
                   return invalid(QStringLiteral("Binding '%1' references missing node '%2'").arg(bName, bNode));
                 }
                 const QJsonObject nodeObj = apiGraph.value(bNode).toObject();
                 const QJsonObject inObj = nodeObj.value(q("inputs")).toObject();
                 if (!inObj.contains(bInput)) {
                   return invalid(QStringLiteral("Node '%1' has no input '%2' for binding '%3'").arg(bNode, bInput, bName));
                 }
               }

               const QString uDir = userTemplatesDir();
               QDir().mkpath(uDir);
               const QString outPath = QDir(uDir).filePath(id + QStringLiteral(".json"));
               const QJsonObject templateDoc{
                 {q("id"), id},
                 {q("name"), name},
                 {q("kind"), kind},
                 {q("description"), a.value(q("description")).toString()},
                 {q("whenToUse"), a.value(q("whenToUse")).toArray()},
                 {q("graph"), apiGraph},
                 {q("outputNode"), outputNode},
                 {q("bindings"), bindings},
                 {q("outputKind"), outputKind},
                 {q("requires"), required},
                 {q("needsUserModel"), needsUserModel}
               };

               QFile outF(outPath);
               if (!outF.open(QIODevice::WriteOnly)) {
                 return Result::failure(q("io_error"), QStringLiteral("Could not write template to %1").arg(outPath));
               }
               outF.write(QJsonDocument(templateDoc).toJson(QJsonDocument::Indented));
               outF.close();

               return Result::success(QJsonObject{
                 {q("id"), id},
                 {q("path"), outPath}
               });
             },
             false));

  e.add(spec("comfy.graph.fromTemplate", "", "comfy", false,
             QStringLiteral("Instantiate a workflow graph from a template and bind parameter values. Edits are deterministic."),
             js::obj({{q("templateId"), js::str(QStringLiteral("Template identifier"))},
                      {q("params"), js::any(QStringLiteral("Parameter values to bind"))}},
                     {q("templateId"), q("params")}),
             [](CallContext& c, const QJsonObject& a) {
               const QString templateId = a.value(q("templateId")).toString();
               auto allTemplates = loadAllTemplates();
               auto itT = allTemplates.find(templateId);
               if (itT == allTemplates.end()) {
                 return notFound(QStringLiteral("Template '%1' not found").arg(templateId));
               }
               const TemplateInfo& tmpl = itT->second;

               QString gErr;
               auto optG = comfy::Graph::fromApiJson(tmpl.graph, &gErr);
               if (!optG) {
                 return Result::failure(q("internal"), QStringLiteral("Template '%1' has invalid graph: %2").arg(templateId, gErr));
               }

               ComfyService::Doc doc;
               doc.graph = std::move(*optG);
               doc.name = tmpl.name;

               const QJsonObject params = a.value(q("params")).toObject();
               doc.params = params;
               if (params.contains(q("seed"))) {
                 doc.seed = params.value(q("seed")).toVariant().toLongLong();
               }
               QJsonArray applied;

               for (auto it = params.begin(); it != params.end(); ++it) {
                 const QString paramKey = it.key();
                 if (paramKey == QLatin1String("templateId")) continue;
                 if (!tmpl.bindings.contains(paramKey)) {
                   return invalid(QStringLiteral("Unknown parameter '%1' for template '%2'").arg(paramKey, templateId));
                 }
                 const QJsonObject bObj = tmpl.bindings.value(paramKey).toObject();
                 const QString bNode = bObj.value(q("node")).toString();
                 const QString bInput = bObj.value(q("input")).toString();
                 const QJsonValue val = it.value();

                 if (paramKey == QLatin1String("image")) {
                   doc.uploads.insert(QStringLiteral("%1.%2").arg(bNode, bInput), val.toString());
                 } else {
                   const QString setErr = doc.graph.setInput(bNode, bInput, val);
                   if (!setErr.isEmpty()) {
                     return invalid(QStringLiteral("Failed to set input '%1.%2': %3").arg(bNode, bInput, setErr));
                   }
                 }
                 applied.append(paramKey);
               }

               ComfyService& svc = c.engine->comfy();
               const QString gid = QStringLiteral("g%1").arg(svc.nextGraph++);
               svc.graphs[gid] = std::move(doc);

               return Result::success(QJsonObject{
                 {q("graphId"), gid},
                 {q("applied"), applied}
               });
             },
             false));

  e.add(spec("comfy.run", "", "comfy", false,
             QStringLiteral("Enqueue a ComfyUI workflow graph execution as a background job after deterministic validation."),
             js::obj({{q("graphId"), js::str(QStringLiteral("Graph document ID"))},
                      {q("outputDir"), js::str(QStringLiteral("Output directory for generated files"))},
                      {q("wait"), js::boolean(QStringLiteral("Wait for generation to complete (default false)"))},
                       {q("timeoutMs"), js::integer(QStringLiteral("Timeout in milliseconds (default 900000)"))},
                       {q("params"), js::any(QStringLiteral("Optional generation parameters"))},
                       {q("seed"), js::anything(QStringLiteral("Optional seed"))}},
                      {q("graphId")}),
             [](CallContext& c, const QJsonObject& a) {
               ComfyService& svc = c.engine->comfy();
               const QString gid = a.value(q("graphId")).toString();
               auto it = svc.graphs.find(gid);
               if (it == svc.graphs.end()) {
                 return notFound(QStringLiteral("Graph '%1' not found").arg(gid));
               }

               Result r = ensureSchema(svc, a);
               if (!r.ok) return r;

               const auto issues = comfy::validate(it->second.graph, *svc.schema);
               if (!issues.empty()) {
                 return Result::failure(q("invalid"), QStringLiteral("Graph validation failed"), QJsonObject{{q("issues"), comfy::issuesToJson(issues)}});
               }

               QString outputDir = a.value(q("outputDir")).toString();
               if (outputDir.isEmpty()) {
                 if (c.session && c.session->project && !c.session->project->path().isEmpty()) {
                   const QFileInfo fi(c.session->project->path());
                   outputDir = fi.dir().filePath(QStringLiteral("generated"));
                 } else {
                   outputDir = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("generated"));
                 }
               }
               outputDir = QDir::toNativeSeparators(QFileInfo(outputDir).absoluteFilePath());

               QJsonObject jobParams = it->second.params;
               if (a.contains(q("params"))) {
                 jobParams = a.value(q("params")).toObject();
               }
               qint64 jobSeed = it->second.seed;
               if (a.contains(q("seed"))) {
                 jobSeed = a.value(q("seed")).toVariant().toLongLong();
               }
               const int timeoutMs = a.value(q("timeoutMs")).toInt(900000);
               jobs::JobRequest req;
               req.analyzer = QStringLiteral("comfy.generate");
               req.path = outputDir;
               req.params = QJsonObject{
                 {q("graph"), it->second.graph.toApiJson()},
                 {q("outputDir"), outputDir},
                 {q("uploads"), it->second.uploads},
                 {q("baseUrl"), svc.client.base().toString()},
                 {q("params"), jobParams},
                 {q("seed"), jobSeed},
                 {q("timeoutMs"), timeoutMs}
               };

               jobs::Submitted sub = c.engine->jobs().enqueue(req);
               if (!sub.ok) {
                 return Result::failure(q("internal"), QStringLiteral("Failed to enqueue job: %1").arg(sub.error));
               }

               const bool wait = a.value(q("wait")).toBool(false);
               if (wait && !sub.cached) {
                 c.engine->jobs().wait(sub.jobId, timeoutMs + 2000);
               }

               auto info = c.engine->jobs().info(sub.jobId);
               if (wait && info && info->state == jobs::JobState::Failed) {
                 return Result::failure(q("comfy_error"), info->error, QJsonObject{{q("jobId"), sub.jobId}});
               }

               QJsonObject ret;
               ret.insert(q("jobId"), sub.jobId);
               ret.insert(q("cached"), sub.cached);
               if (info && info->state == jobs::JobState::Done) {
                 auto res = c.engine->jobs().result(sub.jobId);
                 if (res && res->contains(QStringLiteral("files"))) {
                   ret.insert(q("files"), res->value(QStringLiteral("files")));
                 }
               }
               return Result::success(ret);
             },
             false));

  e.add(spec("comfy.result", "", "comfy", false,
             QStringLiteral("Query status and results of a ComfyUI generation job."),
             js::obj({{q("jobId"), js::str(QStringLiteral("Job ID"))}}, {q("jobId")}),
             [](CallContext& c, const QJsonObject& a) {
               const QString jobId = a.value(q("jobId")).toString();
               auto info = c.engine->jobs().info(jobId);
               if (!info) return notFound(QStringLiteral("Job '%1' not found").arg(jobId));

               QJsonObject ret;
               ret.insert(q("status"), jobs::jobStateName(info->state));
               ret.insert(q("progress"), info->progress);
               if (!info->error.isEmpty()) ret.insert(q("error"), info->error);
               if (info->state == jobs::JobState::Done) {
                 auto res = c.engine->jobs().result(jobId);
                 if (res && res->contains(QStringLiteral("files"))) {
                   ret.insert(q("files"), res->value(QStringLiteral("files")));
                 }
               }
               return Result::success(ret);
             },
             false));

  e.add(spec("generate.shot", "", "editor", true,
             QStringLiteral("Generate a media shot from a ComfyUI workflow template and place outputs onto the timeline in one undo step."),
             js::obj({{q("templateId"), js::str(QStringLiteral("Template identifier"))},
                      {q("prompt"), js::str(QStringLiteral("Prompt text"))},
                      {q("negative"), js::str(QStringLiteral("Negative prompt"))},
                      {q("seed"), js::integer(QStringLiteral("Deterministic seed"), 0)},
                      {q("width"), js::integer(QStringLiteral("Output width"), 64, 4096)},
                      {q("height"), js::integer(QStringLiteral("Output height"), 64, 4096)},
                      {q("frames"), js::integer(QStringLiteral("Frame count"), 1, 1000)},
                      {q("fps"), js::number(QStringLiteral("Frame rate"), 1, 120)},
                      {q("steps"), js::integer(QStringLiteral("Inference steps"), 1, 200)},
                      {q("imageFromItemId"), js::str(QStringLiteral("Item ID to capture starting frame as image input"))},
                      {q("trackId"), js::str(QStringLiteral("Target track ID"))},
                      {q("atFrame"), js::integer(QStringLiteral("Start frame on timeline"), 0)},
                      {q("wait"), js::boolean(QStringLiteral("Wait for completion (default true)"))},
                      {q("timeoutMs"), js::integer(QStringLiteral("Timeout in milliseconds (default 900000)"))}},
                     {q("templateId"), q("prompt")}),
             [](CallContext& c, const QJsonObject& a) {
               ComfyService& svc = c.engine->comfy();
               (void)ensureSchema(svc, a); // best effort: populate svc.schema so the readiness gate below can run
               const QString templateId = a.value(q("templateId")).toString();
               if (templateId.isEmpty()) return invalid(QStringLiteral("Argument 'templateId' is required"));
               const QString prompt = a.value(q("prompt")).toString();
               if (prompt.isEmpty()) return invalid(QStringLiteral("Argument 'prompt' is required"));

               qint64 seed = 0;
               if (a.contains(q("seed"))) {
                 seed = a.value(q("seed")).toVariant().toLongLong();
               } else {
                 const QByteArray sha = QCryptographicHash::hash((templateId + QLatin1Char('|') + prompt).toUtf8(), QCryptographicHash::Sha1).toHex();
                 bool okSeed = false;
                 const quint32 s32 = sha.left(8).toUInt(&okSeed, 16);
                 seed = static_cast<qint64>(s32);
               }

               QJsonObject tParams;
               tParams.insert(q("templateId"), templateId);
               tParams.insert(q("prompt"), prompt);
               tParams.insert(q("seed"), seed);
               for (const char* k : {"negative", "width", "height", "frames", "fps", "steps"}) {
                 if (a.contains(q(k))) tParams.insert(q(k), a.value(q(k)));
               }

               if (a.contains(q("imageFromItemId"))) {
                 const QString itmId = a.value(q("imageFromItemId")).toString();
                 const Item* itm = getItem(c.session->project->doc(), itmId);
                 if (!itm) {
                   return notFound(QStringLiteral("Item '%1' not found").arg(itmId));
                 }
                 Result rRender = c.engine->call(q("frame.render"), QJsonObject{
                   {q("session"), c.session->id},
                   {q("frame"), static_cast<double>(itm->startFrame)}
                 });
                 if (!rRender.ok) return rRender;
                 const QString tempPng = rRender.value.toObject().value(q("path")).toString();
                 tParams.insert(q("image"), tempPng);
               }

               Result rTmpl = c.engine->call(q("comfy.graph.fromTemplate"), QJsonObject{
                 {q("templateId"), templateId},
                 {q("params"), tParams}
               });
               if (!rTmpl.ok) return rTmpl;
               const QString graphId = rTmpl.value.toObject().value(q("graphId")).toString();

               // Readiness gate: fail before queueing when this install lacks the node classes or
               // model files the template needs, so callers get a repairable not_ready result
               // instead of a wedged ComfyUI job.
               auto allT = loadAllTemplates();
               auto itT = allT.find(templateId);
               if (itT != allT.end()) {
                 const TemplateReadiness ready = checkTemplateReadiness(svc, itT->second);
                 if (!ready.ok) {
                   return Result::failure(q("not_ready"), ready.reason,
                                          QStringLiteral("Call comfy.ready for the missing nodes/models of '%1'.").arg(templateId));
                 }
               }

               const bool wait = a.value(q("wait")).toBool(true);
               const int timeoutMs = a.value(q("timeoutMs")).toInt(900000);

               QJsonObject runArgs{
                 {q("graphId"), graphId},
                 {q("wait"), wait},
                 {q("timeoutMs"), timeoutMs},
                 {q("params"), tParams},
                 {q("seed"), seed}
               };
               Result rRun = c.engine->call(q("comfy.run"), runArgs);
               if (!rRun.ok) return rRun;

               const QString jobId = rRun.value.toObject().value(q("jobId")).toString();

               auto info = c.engine->jobs().info(jobId);
               if (info && info->state == jobs::JobState::Failed) {
                 return Result::failure(q("comfy_error"), info->error);
               }
               return runGenerate(c, a, QStringLiteral("image"),
                                  {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("webp")});
             }));

  e.add(spec("generate.collect", "", "editor", true,
             QStringLiteral("Collect outputs of a finished ComfyUI generation job and add them to the timeline."),
             js::obj({{q("jobId"), js::str(QStringLiteral("Job ID"))},
                      {q("trackId"), js::str(QStringLiteral("Target track ID"))},
                      {q("atFrame"), js::integer(QStringLiteral("Start frame on timeline"), 0)}},
                     {q("jobId")}),
             [](CallContext& c, const QJsonObject& a) {
               const QString jobId = a.value(q("jobId")).toString();
               if (jobId.isEmpty()) return invalid(QStringLiteral("Argument 'jobId' is required"));

               auto info = c.engine->jobs().info(jobId);
               if (!info) return notFound(QStringLiteral("Job '%1' not found").arg(jobId));

               if (info->state != jobs::JobState::Done) {
                 return invalid(QStringLiteral("Job %1 is not done (state: %2)").arg(jobId, jobs::jobStateName(info->state)));
               }

               const qint64 seed = info->params.value(q("seed")).toVariant().toLongLong();
               std::optional<Frame> optAtFrame;
               if (a.contains(q("atFrame"))) optAtFrame = a.value(q("atFrame")).toInteger();

               return collectJobOutputs(c, jobId, seed, a.value(q("trackId")).toString(), optAtFrame);
             }));
  e.add(spec("generate.video", "", "editor", true,
             QStringLiteral("Generate a video clip from a ComfyUI workflow template and place outputs on the timeline in one undo step."),
             js::obj({{q("templateId"), js::str(QStringLiteral("Template identifier"))},
                      {q("prompt"), js::str(QStringLiteral("Prompt text"))},
                      {q("negative"), js::str(QStringLiteral("Negative prompt"))},
                      {q("seed"), js::integer(QStringLiteral("Deterministic seed"), 0)},
                      {q("width"), js::integer(QStringLiteral("Output width"), 64, 4096)},
                      {q("height"), js::integer(QStringLiteral("Output height"), 64, 4096)},
                      {q("frames"), js::integer(QStringLiteral("Frame count"), 1, 1000)},
                      {q("fps"), js::number(QStringLiteral("Frame rate"), 1, 120)},
                      {q("steps"), js::integer(QStringLiteral("Inference steps"), 1, 200)},
                      {q("variants"), js::integer(QStringLiteral("Variant count (1-4, default 1)"), 1, 4)},
                      {q("trackId"), js::str(QStringLiteral("Target track ID"))},
                      {q("atFrame"), js::integer(QStringLiteral("Start frame on timeline"), 0)},
                      {q("name"), js::str(QStringLiteral("Output name stem"))},
                      {q("wait"), js::boolean(QStringLiteral("Wait for completion (default true)"))},
                      {q("timeoutMs"), js::integer(QStringLiteral("Timeout in milliseconds (default 900000)"))}},
                     {q("templateId"), q("prompt")}),
             [](CallContext& c, const QJsonObject& a) {
               return runGenerate(c, a, QStringLiteral("video"),
                                  {QStringLiteral("mp4"), QStringLiteral("webm"), QStringLiteral("webp"), QStringLiteral("gif"), QStringLiteral("png")});
             },
             false));

  e.add(spec("generate.image", "", "editor", true,
             QStringLiteral("Generate a still image from a ComfyUI workflow template and place it on the timeline in one undo step."),
             js::obj({{q("templateId"), js::str(QStringLiteral("Template identifier"))},
                      {q("prompt"), js::str(QStringLiteral("Prompt text"))},
                      {q("negative"), js::str(QStringLiteral("Negative prompt"))},
                      {q("seed"), js::integer(QStringLiteral("Deterministic seed"), 0)},
                      {q("width"), js::integer(QStringLiteral("Output width"), 64, 4096)},
                      {q("height"), js::integer(QStringLiteral("Output height"), 64, 4096)},
                      {q("steps"), js::integer(QStringLiteral("Inference steps"), 1, 200)},
                      {q("variants"), js::integer(QStringLiteral("Variant count (1-4, default 1)"), 1, 4)},
                      {q("trackId"), js::str(QStringLiteral("Target track ID"))},
                      {q("atFrame"), js::integer(QStringLiteral("Start frame on timeline"), 0)},
                      {q("name"), js::str(QStringLiteral("Output name stem"))},
                      {q("wait"), js::boolean(QStringLiteral("Wait for completion (default true)"))},
                      {q("timeoutMs"), js::integer(QStringLiteral("Timeout in milliseconds (default 900000)"))}},
                     {q("templateId"), q("prompt")}),
             [](CallContext& c, const QJsonObject& a) {
               return runGenerate(c, a, QStringLiteral("image"),
                                  {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("webp")});
             },
             false));

  e.add(spec("generate.speech", "", "editor", true,
             QStringLiteral("Generate speech audio from a ComfyUI workflow template and place it on the timeline in one undo step."),
             js::obj({{q("templateId"), js::str(QStringLiteral("Template identifier"))},
                      {q("text"), js::str(QStringLiteral("Text to speak"))},
                      {q("voiceRef"), js::str(QStringLiteral("Reference voice item id"))},
                      {q("seed"), js::integer(QStringLiteral("Deterministic seed"), 0)},
                      {q("variants"), js::integer(QStringLiteral("Variant count (1-4, default 1)"), 1, 4)},
                      {q("trackId"), js::str(QStringLiteral("Target track ID"))},
                      {q("atFrame"), js::integer(QStringLiteral("Start frame on timeline"), 0)},
                      {q("name"), js::str(QStringLiteral("Output name stem"))},
                      {q("wait"), js::boolean(QStringLiteral("Wait for completion (default true)"))},
                      {q("timeoutMs"), js::integer(QStringLiteral("Timeout in milliseconds (default 900000)"))}},
                     {q("templateId"), q("text")}),
             [](CallContext& c, const QJsonObject& a) {
               return runGenerate(c, a, QStringLiteral("audio"),
                                  {QStringLiteral("wav"), QStringLiteral("flac"), QStringLiteral("mp3"), QStringLiteral("ogg")});
             },
             false));

  e.add(spec("generate.music", "", "editor", true,
             QStringLiteral("Generate music from a ComfyUI workflow template and place it on the timeline in one undo step."),
             js::obj({{q("templateId"), js::str(QStringLiteral("Template identifier"))},
                      {q("bpm"), js::number(QStringLiteral("Tempo in beats per minute"))},
                      {q("lyrics"), js::str(QStringLiteral("Lyrics"))},
                      {q("tags"), js::str(QStringLiteral("Genre/style tags"))},
                      {q("durationSec"), js::number(QStringLiteral("Target duration in seconds"))},
                      {q("seed"), js::integer(QStringLiteral("Deterministic seed"), 0)},
                      {q("variants"), js::integer(QStringLiteral("Variant count (1-4, default 1)"), 1, 4)},
                      {q("trackId"), js::str(QStringLiteral("Target track ID"))},
                      {q("atFrame"), js::integer(QStringLiteral("Start frame on timeline"), 0)},
                      {q("name"), js::str(QStringLiteral("Output name stem"))},
                      {q("wait"), js::boolean(QStringLiteral("Wait for completion (default true)"))},
                      {q("timeoutMs"), js::integer(QStringLiteral("Timeout in milliseconds (default 900000)"))}},
                     {q("templateId")}),
             [](CallContext& c, const QJsonObject& a) {
               return runGenerate(c, a, QStringLiteral("audio"),
                                  {QStringLiteral("wav"), QStringLiteral("flac"), QStringLiteral("mp3"), QStringLiteral("ogg")});
             },
             false));

  e.add(spec("generate.thumbnail", "", "editor", true,
             QStringLiteral("Render a 1280x720 still from an image template with a title burn; returned as a file path, never placed on the timeline."),
             js::obj({{q("templateId"), js::str(QStringLiteral("Template identifier"))},
                      {q("prompt"), js::str(QStringLiteral("Prompt text"))},
                      {q("name"), js::str(QStringLiteral("Output name stem"))},
                      {q("timeoutMs"), js::integer(QStringLiteral("Timeout in milliseconds (default 900000)"))}},
                     {q("templateId")}),
             [](CallContext& c, const QJsonObject& a) {
               return generateThumbnail(c, a);
             },
             false));

  e.add(spec("generate.history", "", "editor", false,
             QStringLiteral("List past generation records from <project>/generated/provenance.jsonl."),
             js::obj({}),
             [](CallContext& c, const QJsonObject&) {
               QJsonArray arr;
               if (!c.session->project->path().isEmpty()) {
                 QFile f(QFileInfo(c.session->project->path()).dir().filePath(QStringLiteral("generated/provenance.jsonl")));
                 if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                   while (!f.atEnd()) {
                     QByteArray line = f.readLine().trimmed();
                     if (line.isEmpty()) continue;
                     QJsonParseError pe;
                     QJsonDocument doc = QJsonDocument::fromJson(line, &pe);
                     if (pe.error == QJsonParseError::NoError && doc.isObject()) arr.append(doc.object());
                   }
                 }
               }
               return Result::success(QJsonObject{{q("history"), arr}});
             },
             false));

  e.add(spec("reshoot.variants", "", "generate", false,
             QStringLiteral("Pick a generated clip on the timeline, render N alternatives (new seeds and/or prompt tweaks), and compose a contact sheet."),
             js::obj({{q("itemId"), js::str(QStringLiteral("Target item ID"))},
                      {q("count"), js::integer(QStringLiteral("Number of variants (2..8, default 4)"), 2, 8)},
                      {q("vary"), js::choice({q("seed"), q("prompt"), q("both")})},
                      {q("promptEdits"), js::array(js::str(), QStringLiteral("List of prompt suffix strings"))},
                      {q("wait"), js::boolean(QStringLiteral("Wait for generations to complete (default true)"))},
                      {q("timeoutMs"), js::integer(QStringLiteral("Timeout in milliseconds (default 1800000)"))}},
                     {q("itemId")}),
             reshootVariantsCmd));

  e.add(spec("reshoot.pick", "", "generate", true,
             QStringLiteral("Pick one variant from the last reshoot contact sheet and swap the timeline item to point at it in one undo step."),
             js::obj({{q("itemId"), js::str(QStringLiteral("Target item ID"))},
                      {q("letter"), js::choice({q("A"), q("B"), q("C"), q("D"), q("E"), q("F"), q("G"), q("H")})},
                      {q("keepTiming"), js::boolean(QStringLiteral("Keep sourceInFrame (default true)"))}},
                     {q("itemId"), q("letter")}),
             reshootPickCmd));
}

} // namespace sf::engine
