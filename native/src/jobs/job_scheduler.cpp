#include "jobs/job_scheduler.h"

#include "core/schema.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace sf::jobs {

namespace {

constexpr int kWorkers = 2;
constexpr size_t kKeepFinished = 200;

QString q(const char* s) { return QString::fromLatin1(s); }

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

QString digest(const QByteArray& data, int chars) {
  return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex().left(chars));
}

QString paramsHashOf(const QJsonObject& normalized) {
  return digest(QJsonDocument(normalized).toJson(QJsonDocument::Compact), 16);
}

int priorityLevelOf(const QString& name) {
  if (name == QLatin1String("normal")) return 2;
  if (name == QLatin1String("below_normal")) return 1;
  return 0;
}

bool isFinished(JobState s) { return s == JobState::Done || s == JobState::Failed || s == JobState::Cancelled; }

} // namespace

QString jobStateName(JobState s) {
  switch (s) {
  case JobState::Queued: return q("queued");
  case JobState::Running: return q("running");
  case JobState::Done: return q("done");
  case JobState::Failed: return q("failed");
  case JobState::Cancelled: return q("cancelled");
  }
  return q("queued");
}

QJsonObject JobInfo::toJson() const {
  QJsonObject o{{q("jobId"), id},          {q("analyzer"), analyzer},       {q("analyzerVersion"), analyzerVersion}, {q("assetId"), assetId}, {q("params"), params},
                {q("priority"), priority}, {q("state"), jobStateName(state)}, {q("progress"), std::round(progress * 1000.0) / 1000.0}, {q("cached"), cached},
                {q("paused"), paused},     {q("cacheKey"), cacheKey}};
  if (!error.isEmpty()) o.insert(q("error"), error);
  if (startedMs > 0 && finishedMs >= startedMs) o.insert(q("seconds"), static_cast<double>(finishedMs - startedMs) / 1000.0);
  return o;
}

// ---------------------------------------------------------------- lifecycle

QString JobScheduler::defaultCacheDir() {
  const QString env = qEnvironmentVariable("SF_JOBS_CACHE_DIR");
  if (!env.isEmpty()) return env;
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)).filePath(q("analysis"));
}

JobScheduler::JobScheduler(QObject* parent) : QObject(parent), cacheDir_(defaultCacheDir()) {
  timer_.setInterval(500);
  connect(&timer_, &QTimer::timeout, this, [this] { refreshAuto(); });
  for (int i = 0; i < kWorkers; ++i) workers_.emplace_back([this] { workerLoop(); });
}

JobScheduler::~JobScheduler() {
  timer_.stop();
  {
    std::lock_guard<std::mutex> lk(mu_);
    stopping_ = true;
    for (auto& [id, j] : jobs_) j->cancel = true;
  }
  cv_.notify_all();
  doneCv_.notify_all();
  for (std::thread& t : workers_) t.join();
}

void JobScheduler::setCacheDir(const QString& dir) {
  std::lock_guard<std::mutex> lk(mu_);
  cacheDir_ = dir;
}
QString JobScheduler::cacheDir() const {
  std::lock_guard<std::mutex> lk(mu_);
  return cacheDir_;
}

void JobScheduler::registerAnalyzer(std::shared_ptr<Analyzer> a) {
  if (!a) return;
  std::lock_guard<std::mutex> lk(mu_);
  analyzers_[a->id()] = std::move(a);
}
std::shared_ptr<Analyzer> JobScheduler::analyzer(const QString& id) const {
  std::lock_guard<std::mutex> lk(mu_);
  const auto it = analyzers_.find(id);
  return it == analyzers_.end() ? nullptr : it->second;
}
QStringList JobScheduler::analyzerIds() const {
  std::lock_guard<std::mutex> lk(mu_);
  QStringList l;
  for (const auto& [id, a] : analyzers_) l << id;
  return l;
}

// ---------------------------------------------------------------- cache

QString JobScheduler::cachePathLocked(const QString& analyzerId, const QString& cacheKey) const {
  return QDir(cacheDir_).filePath(analyzerId + QLatin1Char('/') + cacheKey + QStringLiteral(".json"));
}

std::optional<QJsonObject> JobScheduler::readCache(const QString& path) const {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
  QJsonParseError pe;
  const QJsonDocument d = QJsonDocument::fromJson(f.readAll(), &pe);
  if (pe.error != QJsonParseError::NoError || !d.isObject() || !d.object().value(q("result")).isObject()) return std::nullopt;
  return d.object().value(q("result")).toObject();
}

void JobScheduler::writeCache(const QString& path, const JobInfo& info, const QJsonObject& value) const {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly)) return;
  const QJsonObject doc{{q("analyzer"), info.analyzer},   {q("version"), info.analyzerVersion}, {q("assetKey"), info.assetKey}, {q("params"), info.params},
                        {q("paramsHash"), info.paramsHash}, {q("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}, {q("result"), value}};
  f.write(QJsonDocument(doc).toJson(QJsonDocument::Compact));
  f.commit();
}

QString JobScheduler::cacheKeyOf(const JobRequest& r, QString* paramsHash, int* version) const {
  const auto a = analyzer(r.analyzer);
  if (!a) return {};
  const QString ph = paramsHashOf(a->normalizeParams(r.params));
  if (paramsHash) *paramsHash = ph;
  if (version) *version = a->version();
  return digest(QStringLiteral("%1|%2|%3|%4").arg(r.assetKey, r.analyzer).arg(a->version()).arg(ph).toUtf8(), 40);
}

std::optional<QJsonObject> JobScheduler::cached(const JobRequest& r) const {
  const QString key = cacheKeyOf(r);
  if (key.isEmpty()) return std::nullopt;
  QString path;
  {
    std::lock_guard<std::mutex> lk(mu_);
    path = cachePathLocked(r.analyzer, key);
  }
  return readCache(path);
}

QString JobScheduler::jobFor(const JobRequest& r) const {
  const QString key = cacheKeyOf(r);
  if (key.isEmpty()) return {};
  std::lock_guard<std::mutex> lk(mu_);
  QString best;
  for (const QString& id : order_) {
    const Job& j = *jobs_.at(id);
    if (j.info.cacheKey != key) continue;
    if (!isFinished(j.info.state) || j.info.state == JobState::Done) best = id; // newest wins
  }
  return best;
}

// ---------------------------------------------------------------- queue

Submitted JobScheduler::enqueue(const JobRequest& r) {
  Submitted out;
  const auto a = analyzer(r.analyzer);
  if (!a) {
    out.error = QStringLiteral("Unknown analyzer \"%1\".").arg(r.analyzer);
    return out;
  }
  if (const QString why = a->unavailableReason(); !why.isEmpty()) {
    out.error = QStringLiteral("Analyzer %1 is unavailable: %2").arg(r.analyzer, why);
    return out;
  }
  if (r.path.isEmpty()) {
    out.error = QStringLiteral("No file to analyse.");
    return out;
  }
  JobInfo info;
  info.analyzer = r.analyzer;
  info.analyzerVersion = a->version();
  info.assetId = r.assetId;
  info.assetKey = r.assetKey;
  info.path = r.path;
  info.params = a->normalizeParams(r.params);
  info.paramsHash = paramsHashOf(info.params);
  info.priority = r.priority;
  info.cacheKey = digest(QStringLiteral("%1|%2|%3|%4").arg(r.assetKey, r.analyzer).arg(info.analyzerVersion).arg(info.paramsHash).toUtf8(), 40);
  info.submittedMs = nowMs();

  QString cachePath;
  {
    std::lock_guard<std::mutex> lk(mu_);
    cachePath = cachePathLocked(r.analyzer, info.cacheKey);
  }
  const bool hit = readCache(cachePath).has_value();

  QString emitId;
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (const QString& id : order_) {
      Job& j = *jobs_.at(id);
      if (j.info.cacheKey == info.cacheKey && (j.info.state == JobState::Queued || j.info.state == JobState::Running)) {
        // an identical job is already on its way: share it (a higher priority request lifts it)
        if (j.info.state == JobState::Queued && r.priority > j.info.priority) j.info.priority = r.priority;
        out.ok = true;
        out.deduped = true;
        out.jobId = id;
        return out;
      }
    }
    auto job = std::make_unique<Job>();
    info.id = newId(q("job"));
    job->analyzer = a;
    job->seq = nextSeq_++;
    if (hit) {
      info.state = JobState::Done;
      info.cached = true;
      info.progress = 1;
      info.startedMs = info.finishedMs = info.submittedMs;
    }
    job->info = info;
    out.ok = true;
    out.cached = hit;
    out.jobId = info.id;
    order_.push_back(info.id);
    jobs_[info.id] = std::move(job);
    trimLocked();
    emitId = info.id;
  }
  cv_.notify_all();
  if (hit) doneCv_.notify_all();
  emit jobChanged(emitId);
  return out;
}

void JobScheduler::trimLocked() {
  size_t finished = 0;
  for (const QString& id : order_)
    if (isFinished(jobs_.at(id)->info.state)) ++finished;
  for (size_t i = 0; i < order_.size() && finished > kKeepFinished;) {
    if (isFinished(jobs_.at(order_[i])->info.state)) {
      jobs_.erase(order_[i]);
      order_.erase(order_.begin() + static_cast<std::ptrdiff_t>(i));
      --finished;
    } else {
      ++i;
    }
  }
}

JobScheduler::Job* JobScheduler::pickLocked() {
  Job* best = nullptr;
  for (const QString& id : order_) {
    Job& j = *jobs_.at(id);
    if (j.info.state != JobState::Queued) continue;
    if (j.analyzer->isMl() && mlRunning_ >= 1) continue; // one ML job at a time
    if (!best || j.info.priority > best->info.priority || (j.info.priority == best->info.priority && j.seq < best->seq)) best = &j;
  }
  return best;
}

void JobScheduler::finishLocked(Job& j, JobState s, const QString& error) {
  j.info.state = s;
  j.info.error = error;
  j.info.finishedMs = nowMs();
  if (s == JobState::Done) j.info.progress = 1;
}

bool JobScheduler::cancel(const QString& jobId) {
  bool changed = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = jobs_.find(jobId);
    if (it == jobs_.end()) return false;
    Job& j = *it->second;
    if (j.info.state == JobState::Queued) {
      finishLocked(j, JobState::Cancelled, {});
      changed = true;
    } else if (j.info.state == JobState::Running) {
      j.cancel = true;
    } else {
      return false;
    }
  }
  cv_.notify_all();
  doneCv_.notify_all();
  if (changed) emit jobChanged(jobId);
  return true;
}

std::optional<JobInfo> JobScheduler::info(const QString& jobId) const {
  std::lock_guard<std::mutex> lk(mu_);
  const auto it = jobs_.find(jobId);
  if (it == jobs_.end()) return std::nullopt;
  JobInfo i = it->second->info;
  i.paused = i.state == JobState::Running && pausedLocked();
  return i;
}

std::vector<JobInfo> JobScheduler::list() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<const Job*> running, queued, finished;
  for (const QString& id : order_) {
    const Job& j = *jobs_.at(id);
    (j.info.state == JobState::Running ? running : j.info.state == JobState::Queued ? queued : finished).push_back(&j);
  }
  std::stable_sort(queued.begin(), queued.end(), [](const Job* a, const Job* b) { return a->info.priority != b->info.priority ? a->info.priority > b->info.priority : a->seq < b->seq; });
  std::reverse(finished.begin(), finished.end());
  std::vector<JobInfo> out;
  for (const auto* group : {&running, &queued, &finished})
    for (const Job* j : *group) {
      out.push_back(j->info);
      out.back().paused = j->info.state == JobState::Running && pausedLocked();
    }
  return out;
}

std::optional<QJsonObject> JobScheduler::result(const QString& jobId) const {
  QString path;
  {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = jobs_.find(jobId);
    if (it == jobs_.end() || it->second->info.state != JobState::Done) return std::nullopt;
    if (!it->second->result.isEmpty()) return it->second->result; // the cache could not be written
    path = cachePathLocked(it->second->info.analyzer, it->second->info.cacheKey);
  }
  return readCache(path);
}

bool JobScheduler::wait(const QString& jobId, int timeoutMs) {
  std::unique_lock<std::mutex> lk(mu_);
  return doneCv_.wait_for(lk, std::chrono::milliseconds(std::max(0, timeoutMs)), [&] {
    const auto it = jobs_.find(jobId);
    return stopping_ || it == jobs_.end() || isFinished(it->second->info.state);
  });
}

bool JobScheduler::waitIdle(int timeoutMs) {
  std::unique_lock<std::mutex> lk(mu_);
  return doneCv_.wait_for(lk, std::chrono::milliseconds(std::max(0, timeoutMs)), [&] {
    for (const auto& [id, j] : jobs_)
      if (!isFinished(j->info.state)) return false;
    return true;
  });
}

// ---------------------------------------------------------------- pausing

bool JobScheduler::pausedLocked() const {
  return !manualReason_.isEmpty() || (autoPause_ && (exportFlag_ || exportProbe_ || battery_ || vram_));
}

static QStringList reasonsOf(const QString& manual, bool autoOn, bool exp, bool battery, bool vram) {
  QStringList l;
  if (!manual.isEmpty()) l << QStringLiteral("manual:%1").arg(manual);
  if (autoOn) {
    if (exp) l << q("auto:export");
    if (battery) l << q("auto:battery");
    if (vram) l << q("auto:vram");
  }
  return l;
}

QStringList JobScheduler::pauseReasons() const {
  std::lock_guard<std::mutex> lk(mu_);
  return reasonsOf(manualReason_, autoPause_, exportFlag_ || exportProbe_, battery_, vram_);
}

bool JobScheduler::paused() const {
  std::lock_guard<std::mutex> lk(mu_);
  return pausedLocked();
}

// Applies `change` under the lock, wakes the workers and reports when the set of reasons moved.
#define SF_JOBS_MUTATE(change)                                                                                                         \
  QStringList before, after;                                                                                                           \
  {                                                                                                                                    \
    std::lock_guard<std::mutex> lk(mu_);                                                                                               \
    before = reasonsOf(manualReason_, autoPause_, exportFlag_ || exportProbe_, battery_, vram_);                                       \
    change;                                                                                                                            \
    after = reasonsOf(manualReason_, autoPause_, exportFlag_ || exportProbe_, battery_, vram_);                                        \
  }                                                                                                                                    \
  cv_.notify_all();                                                                                                                    \
  if (before != after) emit pauseChanged(!after.isEmpty(), after);

void JobScheduler::pause(const QString& reason) {
  const QString r = reason.trimmed().isEmpty() ? q("user") : reason.trimmed();
  SF_JOBS_MUTATE(manualReason_ = r)
}
void JobScheduler::resume() { SF_JOBS_MUTATE(manualReason_.clear()) }
void JobScheduler::setExportActive(bool on) { SF_JOBS_MUTATE(exportFlag_ = on) }
void JobScheduler::applySystem(bool onBattery, bool vramPressure) { SF_JOBS_MUTATE(battery_ = onBattery; vram_ = vramPressure) }
void JobScheduler::setAutoPause(bool on) { SF_JOBS_MUTATE(autoPause_ = on) }

bool JobScheduler::autoPause() const {
  std::lock_guard<std::mutex> lk(mu_);
  return autoPause_;
}

void JobScheduler::setBusyProbe(std::function<bool()> probe) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    busyProbe_ = std::move(probe);
  }
  if (QCoreApplication::instance()) timer_.start();
}

void JobScheduler::refreshAuto() {
  std::function<bool()> probe;
  {
    std::lock_guard<std::mutex> lk(mu_);
    probe = busyProbe_;
  }
  if (!probe) return;
  const bool busy = probe();
  SF_JOBS_MUTATE(exportProbe_ = busy)
}

// ---------------------------------------------------------------- resources

void JobScheduler::setResources(const QString& backgroundPriority, int mlThreads) {
  priorityLevel_ = priorityLevelOf(backgroundPriority);
  mlThreads_ = std::clamp(mlThreads, 1, 64);
  std::lock_guard<std::mutex> lk(mu_);
  priorityName_ = backgroundPriority;
}
QString JobScheduler::backgroundPriority() const {
  std::lock_guard<std::mutex> lk(mu_);
  return priorityName_;
}
int JobScheduler::mlThreads() const { return mlThreads_; }
int JobScheduler::mlRunning() const {
  std::lock_guard<std::mutex> lk(mu_);
  return mlRunning_;
}

void JobScheduler::applyThreadPriority() const {
#ifdef _WIN32
  static thread_local int applied = -1;
  const int level = priorityLevel_;
  if (level == applied) return;
  applied = level;
  SetThreadPriority(GetCurrentThread(), level == 0 ? THREAD_PRIORITY_IDLE : level == 1 ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL);
#endif
}

// ---------------------------------------------------------------- workers

void JobScheduler::workerLoop() {
  for (;;) {
    Job* job = nullptr;
    std::shared_ptr<Analyzer> analyzer;
    JobContext ctx;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [&] { return stopping_ || (!pausedLocked() && pickLocked() != nullptr); });
      if (stopping_) return;
      job = pickLocked();
      job->info.state = JobState::Running;
      job->info.startedMs = nowMs();
      ++running_;
      if (job->analyzer->isMl()) ++mlRunning_;
      analyzer = job->analyzer;
      ctx.path = job->info.path;
      ctx.params = job->info.params;
    }
    const QString id = job->info.id; // the id never changes
    emit jobChanged(id);
    applyThreadPriority();
    ctx.threads = analyzer->isMl() ? mlThreads_.load() : 1;
    auto lastEmit = std::make_shared<double>(0);
    ctx.progress = [this, job, id, lastEmit](double f) {
      f = std::clamp(f, 0.0, 1.0);
      {
        std::lock_guard<std::mutex> lk(mu_);
        job->info.progress = f;
      }
      if (f - *lastEmit >= 0.02 || f >= 1.0) {
        *lastEmit = f;
        emit jobChanged(id);
      }
    };
    ctx.cancelled = [this, job] { return job->cancel.load() || stopping_; };
    ctx.checkpoint = [this, job] {
      applyThreadPriority();
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [&] { return stopping_ || job->cancel.load() || !pausedLocked(); });
      return !stopping_ && !job->cancel.load();
    };

    AnalyzerResult res;
    try {
      res = analyzer->run(ctx);
    } catch (const std::exception& e) {
      res = AnalyzerResult::failure(QString::fromUtf8(e.what()));
    } catch (...) {
      res = AnalyzerResult::failure(q("Unknown failure in the analyzer."));
    }

    const bool cancelled = job->cancel.load() || stopping_;
    bool cacheOk = false;
    if (res.ok && !cancelled) {
      QString path;
      JobInfo snapshot;
      {
        std::lock_guard<std::mutex> lk(mu_);
        path = cachePathLocked(job->info.analyzer, job->info.cacheKey);
        snapshot = job->info;
      }
      writeCache(path, snapshot, res.value);
      cacheOk = QFileInfo::exists(path);
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      --running_;
      if (analyzer->isMl()) --mlRunning_;
      if (cancelled) finishLocked(*job, JobState::Cancelled, {});
      else if (res.ok) {
        if (!cacheOk) job->result = res.value;
        finishLocked(*job, JobState::Done, {});
      } else finishLocked(*job, JobState::Failed, res.error.isEmpty() ? q("The analyzer failed.") : res.error);
      trimLocked();
    }
    cv_.notify_all(); // an ML slot is free again
    doneCv_.notify_all();
    emit jobChanged(id);
  }
}

} // namespace sf::jobs
