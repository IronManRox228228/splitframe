// Background analysis: jobs.list / enqueue / cancel / pause / resume / policy, analysis.get and analysis.hands.track.
// The queue, the disk cache and the workers live in jobs/job_scheduler; this file maps assets and items onto it and the machine's
// resource decisions (sys/system_profile: mlThreads, backgroundPriority, battery, VRAM headroom) onto its knobs.
// None of these commands blocks unless asked: `wait` (with `timeoutMs`) is for the one-shot CLI, which exits when the call returns.

#include "engine/cmd_util.h"
#include "sys/system_profile.h"

#include <QDateTime>
#include <QFileInfo>

#include <algorithm>

namespace sf::engine {

namespace {

constexpr int kDefaultWaitMs = 30000;

// Feeds the shared system service's decisions into the scheduler (priority, threads, battery, VRAM). At most every 2 s: the first call
// queries the hardware, and the service re-evaluates on its own timer once monitoring runs.
void syncSystem(Engine& e) {
  static qint64 last = 0;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (last != 0 && now - last < 2000) return;
  last = now;
  sys::SystemService& s = sharedSystemService();
  const sys::Decisions d = s.decisions();
  const sys::LiveState live = s.live(1000);
  e.jobs().setResources(d.backgroundPriority, d.mlThreads);
  e.jobs().applySystem(live.power.onBattery, live.vram.valid && live.vram.localHeadroom() < sys::kGiB);
}

// assetId | itemId | path -> a request (analyzer, params and priority are filled by the caller).
Result resolveTarget(CallContext& c, const QJsonObject& a, jobs::JobRequest* req) {
  QString path, assetId;
  std::optional<QString> hash;
  if (a.contains(q("itemId"))) {
    const Item* it = getItem(c.session->project->doc(), a.value(q("itemId")).toString());
    if (!it) return notFound(QStringLiteral("Item %1 not found.").arg(a.value(q("itemId")).toString()), QStringLiteral("Call timeline.get."));
    if (!it->assetId) return invalid(QStringLiteral("Item %1 has no media asset.").arg(it->id));
    assetId = *it->assetId;
  } else if (a.contains(q("assetId"))) {
    assetId = a.value(q("assetId")).toString();
  }
  if (!assetId.isEmpty()) {
    const Asset* as = c.session->project->asset(assetId);
    if (!as) return notFound(QStringLiteral("Asset %1 not found.").arg(assetId), QStringLiteral("Call assets.list."));
    path = as->path;
    hash = as->hash;
  } else if (a.contains(q("path"))) {
    path = a.value(q("path")).toString();
  } else {
    return invalid(QStringLiteral("Pass itemId, assetId or path."));
  }
  const QFileInfo fi(path);
  if (!fi.exists() || !fi.isFile()) return notFound(QStringLiteral("No file at %1.").arg(path), QStringLiteral("Relink the asset (media.relink)."));
  req->assetId = assetId;
  req->path = fi.absoluteFilePath();
  // the content key: the asset's hash when it has one, else path + size + mtime
  req->assetKey = hash && !hash->isEmpty() ? QStringLiteral("h:") + *hash : QStringLiteral("f:%1|%2|%3").arg(fi.canonicalFilePath()).arg(fi.size()).arg(fi.lastModified().toMSecsSinceEpoch());
  req->params = a.value(q("params")).toObject();
  req->priority = a.value(q("priority")).toInt(0);
  return Result::success();
}

QJsonObject statusJson(Engine& e) {
  jobs::JobScheduler& s = e.jobs();
  return {{q("paused"), s.paused()}, {q("pauseReasons"), QJsonArray::fromStringList(s.pauseReasons())}, {q("autoPause"), s.autoPause()},
          {q("backgroundPriority"), s.backgroundPriority()}, {q("mlThreads"), s.mlThreads()}};
}

QJsonObject refJson(const jobs::JobRequest& r, const jobs::JobScheduler& s) {
  const auto a = s.analyzer(r.analyzer);
  return {{q("analyzer"), r.analyzer}, {q("assetId"), r.assetId}, {q("params"), a ? a->normalizeParams(r.params) : r.params}, {q("cacheKey"), s.cacheKeyOf(r)},
          {q("get"), q("analysis.get with these analyzer / assetId / params")}};
}

// Waits for a job when asked and describes where it stands; `result` inlined when finished and wanted.
Result describeJob(Engine& e, const QString& jobId, const jobs::JobRequest& req, const QJsonObject& a, bool cached, bool deduped, bool inlineResult) {
  jobs::JobScheduler& s = e.jobs();
  if (a.value(q("wait")).toBool(false)) s.wait(jobId, a.value(q("timeoutMs")).toInt(kDefaultWaitMs));
  const auto info = s.info(jobId);
  QJsonObject o = info ? info->toJson() : QJsonObject{{q("jobId"), jobId}};
  o.insert(q("cached"), cached || (info && info->cached));
  o.insert(q("deduped"), deduped);
  o.insert(q("status"), info ? jobs::jobStateName(info->state) : q("unknown"));
  o.insert(q("ref"), refJson(req, s));
  if (info && info->state == jobs::JobState::Done && inlineResult) {
    if (const auto r = s.result(jobId)) o.insert(q("result"), *r);
  }
  if (info && info->state != jobs::JobState::Done && info->state != jobs::JobState::Failed && info->state != jobs::JobState::Cancelled && a.value(q("wait")).toBool(false))
    o.insert(q("timedOut"), true);
  if (s.paused() && info && info->state == jobs::JobState::Queued) o.insert(q("note"), q("The queue is paused: ") + s.pauseReasons().join(QStringLiteral(", ")));
  return Result::success(o);
}

Result enqueueCmd(CallContext& c, const QJsonObject& a) {
  jobs::JobScheduler& s = c.engine->jobs();
  syncSystem(*c.engine);
  jobs::JobRequest req;
  req.analyzer = a.value(q("analyzer")).toString();
  if (Result r = resolveTarget(c, a, &req); !r.ok) return r;
  const jobs::Submitted sub = s.enqueue(req);
  if (!sub.ok) return Result::failure(s.analyzer(req.analyzer) ? q("unavailable") : q("invalid_params"), sub.error, QJsonObject{{q("analyzers"), QJsonArray::fromStringList(s.analyzerIds())}});
  return describeJob(*c.engine, sub.jobId, req, a, sub.cached, sub.deduped, a.value(q("includeResult")).toBool(false));
}

Result getCmd(CallContext& c, const QJsonObject& a) {
  jobs::JobScheduler& s = c.engine->jobs();
  jobs::JobRequest req;
  req.analyzer = a.value(q("analyzer")).toString();
  if (!s.analyzer(req.analyzer)) return invalid(QStringLiteral("Unknown analyzer \"%1\". Known: %2.").arg(req.analyzer, s.analyzerIds().join(QStringLiteral(", "))));
  if (Result r = resolveTarget(c, a, &req); !r.ok) return r;
  if (const auto hit = s.cached(req)) return Result::success(QJsonObject{{q("status"), q("done")}, {q("cached"), true}, {q("ref"), refJson(req, s)}, {q("result"), *hit}});
  QString jobId = s.jobFor(req);
  if (jobId.isEmpty() && a.value(q("enqueue")).toBool(false)) {
    syncSystem(*c.engine);
    const jobs::Submitted sub = s.enqueue(req);
    if (!sub.ok) return Result::failure(q("unavailable"), sub.error);
    jobId = sub.jobId;
  }
  if (jobId.isEmpty()) return Result::success(QJsonObject{{q("status"), q("none")}, {q("ref"), refJson(req, s)}, {q("hint"), q("Not analysed yet: call jobs.enqueue (or pass enqueue=true).")}});
  return describeJob(*c.engine, jobId, req, a, false, false, true);
}

Result handsTrack(CallContext& c, const QJsonObject& a) {
  jobs::JobScheduler& s = c.engine->jobs();
  syncSystem(*c.engine);
  jobs::JobRequest req;
  req.analyzer = q("hands");
  if (Result r = resolveTarget(c, a, &req); !r.ok) return r;
  if (a.contains(q("fps"))) req.params.insert(q("fps"), a.value(q("fps")));
  const jobs::Submitted sub = s.enqueue(req);
  if (!sub.ok) return Result::failure(q("unavailable"), sub.error);
  return describeJob(*c.engine, sub.jobId, req, a, sub.cached, sub.deduped, true);
}

Result listCmd(CallContext& c, const QJsonObject& a) {
  jobs::JobScheduler& s = c.engine->jobs();
  syncSystem(*c.engine);
  s.refreshAuto();
  const QString only = a.value(q("state")).toString();
  QJsonArray jobsArr;
  int running = 0, queued = 0, done = 0;
  for (const jobs::JobInfo& i : s.list()) {
    if (i.state == jobs::JobState::Running) ++running;
    else if (i.state == jobs::JobState::Queued) ++queued;
    else if (i.state == jobs::JobState::Done) ++done;
    if (only.isEmpty() || jobs::jobStateName(i.state) == only) jobsArr.append(i.toJson());
  }
  QJsonArray analyzers;
  for (const QString& id : s.analyzerIds()) {
    const auto an = s.analyzer(id);
    const QString why = an->unavailableReason();
    QJsonObject o{{q("id"), id}, {q("version"), an->version()}, {q("ml"), an->isMl()}, {q("available"), why.isEmpty()}, {q("description"), an->description()}, {q("defaults"), an->defaultParams()}};
    if (!why.isEmpty()) o.insert(q("unavailableReason"), why);
    analyzers.append(o);
  }
  QJsonObject o = statusJson(*c.engine);
  o.insert(q("jobs"), jobsArr);
  o.insert(q("running"), running);
  o.insert(q("queued"), queued);
  o.insert(q("done"), done);
  o.insert(q("analyzers"), analyzers);
  o.insert(q("cacheDir"), s.cacheDir());
  return Result::success(o);
}

Result cancelCmd(CallContext& c, const QJsonObject& a) {
  const QString id = a.value(q("jobId")).toString();
  jobs::JobScheduler& s = c.engine->jobs();
  if (!s.info(id)) return notFound(QStringLiteral("No job %1.").arg(id), QStringLiteral("Call jobs.list."));
  const bool ok = s.cancel(id);
  const auto i = s.info(id);
  return Result::success(QJsonObject{{q("jobId"), id}, {q("cancelled"), ok}, {q("state"), i ? jobs::jobStateName(i->state) : q("unknown")}, {q("note"), ok ? q("A running job stops at its next checkpoint.") : q("The job had already finished.")}});
}

Result pauseCmd(CallContext& c, const QJsonObject& a) {
  c.engine->jobs().pause(a.value(q("reason")).toString(QStringLiteral("user")));
  return Result::success(statusJson(*c.engine));
}

Result resumeCmd(CallContext& c, const QJsonObject&) {
  c.engine->jobs().resume();
  return Result::success(statusJson(*c.engine));
}

Result policyCmd(CallContext& c, const QJsonObject& a) {
  if (a.contains(q("autoPause"))) c.engine->jobs().setAutoPause(a.value(q("autoPause")).toBool());
  return Result::success(statusJson(*c.engine));
}

QJsonObject targetProps(QJsonObject props) {
  props.insert(q("assetId"), js::str(QStringLiteral("The media asset (assets.list)")));
  props.insert(q("itemId"), js::str(QStringLiteral("Or a timeline item: its asset is analysed (the whole file, not only the trimmed part)")));
  props.insert(q("path"), js::str(QStringLiteral("Or a file path")));
  return props;
}

} // namespace

static QJsonObject schemaOf(const QJsonObject& props, const QStringList& required = {}) {
  QJsonObject o{{q("type"), q("object")}, {q("properties"), props}, {q("additionalProperties"), false}};
  if (!required.isEmpty()) o.insert(q("required"), QJsonArray::fromStringList(required));
  return o;
}

void registerJobsCommands(Engine& e) {
  const QString waitDoc = QStringLiteral("true: block until the job finishes or `timeoutMs` (default 30000) passes; for the one-shot CLI. Without it the call returns at once.");
  const QJsonObject timeout = js::integer(QStringLiteral("Wait limit in ms (default 30000)"), 0, 3600000);
  e.add(spec("jobs.enqueue", "", "engine", false,
             QStringLiteral("Queue a background analysis of an asset or item. Results are cached on disk by (asset content, analyzer, analyzer version, params): the same request again returns cached=true at once with no work. "
                            "Returns {jobId, status: queued | running | done | failed, cached, deduped (an identical job was already queued), progress, ref}. At most one ML job runs at a time, at the machine's background priority; "
                            "the queue pauses itself during an export and on battery / VRAM pressure (jobs.list shows pauseReasons)."),
             schemaOf(targetProps({{q("analyzer"), js::str(QStringLiteral("hands | waveform (jobs.list shows all, with defaults)"))},
                                   {q("params"), js::any(QStringLiteral("Analyzer parameters; defaults are filled in and hashed into the cache key"))},
                                   {q("priority"), js::integer(QStringLiteral("Higher runs first (default 0)"))},
                                   {q("wait"), js::boolean(waitDoc)},
                                   {q("timeoutMs"), timeout},
                                   {q("includeResult"), js::boolean(QStringLiteral("Inline the result when the job is finished (default false: use analysis.get)"))}}),
                      {q("analyzer")}),
             enqueueCmd));
  e.add(spec("analysis.get", "", "engine", false,
             QStringLiteral("The cached result of an analysis, or where it stands: {status: done | queued | running | failed | cancelled | none, result?, progress?, jobId?}. Never runs anything unless enqueue=true (then a missing analysis is queued). "
                            "wait=true blocks (up to timeoutMs) for a queued or running job, so a one-shot CLI call can finish a small job."),
             schemaOf(targetProps({{q("analyzer"), js::str(QStringLiteral("hands | waveform"))},
                                   {q("params"), js::any(QStringLiteral("The same params the analysis was queued with (defaults filled in)"))},
                                   {q("enqueue"), js::boolean(QStringLiteral("Queue the analysis when it does not exist yet"))},
                                   {q("wait"), js::boolean(waitDoc)},
                                   {q("timeoutMs"), timeout}}),
                      {q("analyzer")}),
             getCmd));
  e.add(spec("analysis.hands.track", "", "engine", false,
             QStringLiteral("Track hands in a clip: palm detection + hand-landmark model (ONNX Runtime, CPU) sampled at `fps` (default 10). Result frames[{t, hands[{handedness, score, landmarks[21][x,y,z]}]}] with landmarks in normalized frame coordinates "
                            "(x / width, y / height). Cached per asset and fps; wait=true returns the result (up to timeoutMs), otherwise poll analysis.get / jobs.list. Reports `unavailable` when ONNX Runtime or the models are missing."),
             schemaOf(targetProps({{q("fps"), js::number(QStringLiteral("Samples per second (default 10)"), 0.1, 60)},
                                   {q("params"), js::any(QStringLiteral("More hands params: maxSeconds, palmScore, handScore, maxHands"))},
                                   {q("priority"), js::integer(QStringLiteral("Higher runs first (default 0)"))},
                                   {q("wait"), js::boolean(waitDoc)},
                                   {q("timeoutMs"), timeout}})),
             handsTrack));
  e.add(spec("jobs.list", "", "engine", false,
             QStringLiteral("The analysis queue: running, queued (by priority) and finished jobs with progress, plus paused / pauseReasons (manual:<why>, auto:export, auto:battery, auto:vram), the worker priority and ML threads, "
                            "and the analyzers with their defaults and whether they can run here (hands needs ONNX Runtime and the models in SF_MODELS_DIR)."),
             schemaOf({{q("state"), js::choice({q("queued"), q("running"), q("done"), q("failed"), q("cancelled")}, QStringLiteral("Only jobs in this state"))}}), listCmd, false));
  e.add(spec("jobs.cancel", "", "engine", false, QStringLiteral("Cancel a queued job (gone at once) or a running one (stops at its next checkpoint). Nothing of a cancelled job is cached."),
             schemaOf({{q("jobId"), js::str()}}, {q("jobId")}), cancelCmd, false));
  e.add(spec("jobs.pause", "", "engine", false,
             QStringLiteral("Pause the analysis queue: no job starts and a running one holds at its next checkpoint (an agent can call this while the local model generates). `reason` is shown in jobs.list. Undo with jobs.resume."),
             schemaOf({{q("reason"), js::str(QStringLiteral("Why (default \"user\")"))}}), pauseCmd, false));
  e.add(spec("jobs.resume", "", "engine", false, QStringLiteral("Lift the manual pause. Automatic pauses (export running, battery, VRAM pressure) lift on their own."), schemaOf({}), resumeCmd, false));
  e.add(spec("jobs.policy", "", "engine", false,
             QStringLiteral("autoPause=false makes the queue ignore exports, battery and VRAM pressure (manual pauses still hold). Returns the current state."),
             schemaOf({{q("autoPause"), js::boolean()}}), policyCmd, false));
}

} // namespace sf::engine
