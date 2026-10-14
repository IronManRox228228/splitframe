#pragma once

// Background analysis jobs.
//
//   Analyzer       one kind of analysis (id + version + parameter defaults + run()); ML analyzers are flagged so at most ONE runs at a time
//   JobScheduler   a priority queue of jobs keyed by (asset content key, analyzer id, analyzer version, params hash), a result cache on disk
//                  (<cacheDir>/<analyzer>/<key digest>.json: the same key again is a cache hit and never calls the analyzer), two worker threads
//                  at the Windows thread priority of the `backgroundPriority` decision (idle until told otherwise) and `mlThreads` intra-op threads
//                  for ML analyzers, cancellation, progress, pause / resume.
//
// Pausing: new jobs stop starting and a running job blocks at its next JobContext::checkpoint(). Reasons add up and each lifts on its own:
//   manual:<why>   pause(reason) / resume() (the `jobs.pause` command: an agent pauses the queue while the local LLM generates)
//   auto:export    an export is running (setExportActive(), or the busy probe the engine installs)
//   auto:battery / auto:vram   applySystem(): the machine runs on battery, or the GPU has under 1 GiB of VRAM headroom
// Nothing here runs on the caller's thread except bookkeeping under a short lock; wait() is the only call that blocks (CLI use, with a timeout).

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace sf::jobs {

struct JobContext {
  QString path;        // the asset's file
  QJsonObject params;  // normalized by the analyzer
  int threads = 1;     // intra-op threads for ML analyzers (the mlThreads decision)
  std::function<void(double)> progress;
  std::function<bool()> cancelled;
  std::function<bool()> checkpoint; // blocks while paused; false = cancelled, stop now
  void report(double fraction) const { if (progress) progress(fraction); }
  bool stop() const { return checkpoint ? !checkpoint() : (cancelled && cancelled()); }
};

struct AnalyzerResult {
  bool ok = false;
  QJsonObject value;
  QString error;
  static AnalyzerResult success(QJsonObject v) { return {true, std::move(v), {}}; }
  static AnalyzerResult failure(QString e) { return {false, {}, std::move(e)}; }
};

class Analyzer {
public:
  virtual ~Analyzer() = default;
  virtual QString id() const = 0;
  virtual int version() const = 0; // bump when the output changes: old cache entries stop matching
  virtual QString description() const { return {}; }
  virtual bool isMl() const { return false; }
  virtual QString unavailableReason() const { return {}; } // "" = can run; else why not (ORT or model missing ...)
  virtual QJsonObject defaultParams() const { return {}; }
  // Defaults filled in, values clamped: what gets hashed into the cache key.
  virtual QJsonObject normalizeParams(const QJsonObject& p) const {
    QJsonObject o = defaultParams();
    for (auto it = p.begin(); it != p.end(); ++it) o.insert(it.key(), it.value());
    return o;
  }
  virtual AnalyzerResult run(JobContext& ctx) = 0;
};

enum class JobState { Queued, Running, Done, Failed, Cancelled };
QString jobStateName(JobState s);

struct JobRequest {
  QString analyzer;
  QString assetId;   // for display and listing
  QString assetKey;  // content key of the asset (hash, or path + size + mtime): half of the cache key
  QString path;      // the file to analyse
  QJsonObject params;
  int priority = 0;  // higher first, then first come first served
};

struct JobInfo {
  QString id;
  QString analyzer;
  int analyzerVersion = 0;
  QString assetId, assetKey, path, paramsHash, cacheKey;
  QJsonObject params;
  int priority = 0;
  JobState state = JobState::Queued;
  double progress = 0;
  QString error;
  bool cached = false; // served from the disk cache, no analyzer call
  bool paused = false; // running, but held at a checkpoint
  qint64 submittedMs = 0, startedMs = 0, finishedMs = 0;
  QJsonObject toJson() const;
};

struct Submitted {
  bool ok = false;
  QString error;
  QString jobId;
  bool cached = false;
  bool deduped = false; // an identical job was already queued or running
};

class JobScheduler : public QObject {
  Q_OBJECT
public:
  explicit JobScheduler(QObject* parent = nullptr);
  ~JobScheduler() override;

  // $SF_JOBS_CACHE_DIR, else <AppConfigLocation>/analysis (the footage-index cache lives in the project folder, this one is per user).
  static QString defaultCacheDir();
  void setCacheDir(const QString& dir);
  QString cacheDir() const;

  void registerAnalyzer(std::shared_ptr<Analyzer> a); // replaces one with the same id
  std::shared_ptr<Analyzer> analyzer(const QString& id) const;
  QStringList analyzerIds() const;

  Submitted enqueue(const JobRequest& r);
  // The cached result for this request, nullopt when none (and no analyzer is ever called).
  std::optional<QJsonObject> cached(const JobRequest& r) const;
  QString cacheKeyOf(const JobRequest& r, QString* paramsHash = nullptr, int* version = nullptr) const; // "" when the analyzer is unknown
  // Job that is queued / running / finished for the request's key, "" when there is none.
  QString jobFor(const JobRequest& r) const;

  bool cancel(const QString& jobId); // queued: gone at once; running: stops at its next checkpoint
  std::optional<JobInfo> info(const QString& jobId) const;
  std::vector<JobInfo> list() const; // running, then queued by priority, then finished (newest first, last 200)
  std::optional<QJsonObject> result(const QString& jobId) const; // of a finished job
  // Waits (blocking, condition variable) until the job is finished or the time is up; true when it finished.
  bool wait(const QString& jobId, int timeoutMs);
  bool waitIdle(int timeoutMs);

  // ---- pausing ----
  void pause(const QString& reason = QStringLiteral("user"));
  void resume(); // lifts the manual pause only
  bool paused() const;
  QStringList pauseReasons() const;
  void setExportActive(bool on);
  void setBusyProbe(std::function<bool()> probe); // polled every 500 ms (needs an event loop) and by refreshAuto(); true = an export runs
  void applySystem(bool onBattery, bool vramPressure);
  void setAutoPause(bool on); // false: ignore export / battery / VRAM (tests, "I know what I'm doing")
  bool autoPause() const;
  void refreshAuto(); // re-evaluates the busy probe now

  // ---- resources ----
  // backgroundPriority: idle | below_normal | normal (Windows thread priority of the workers); mlThreads: intra-op threads.
  void setResources(const QString& backgroundPriority, int mlThreads);
  QString backgroundPriority() const;
  int mlThreads() const;
  int mlRunning() const; // 0 or 1

signals:
  void jobChanged(const QString& jobId);
  void pauseChanged(bool paused, const QStringList& reasons);

private:
  struct Job {
    JobInfo info;
    std::shared_ptr<Analyzer> analyzer;
    std::atomic<bool> cancel{false};
    QJsonObject result;
    quint64 seq = 0;
  };
  void workerLoop();
  Job* pickLocked();
  void finishLocked(Job& j, JobState s, const QString& error);
  bool pausedLocked() const;
  void trimLocked();
  QString cachePathLocked(const QString& analyzer, const QString& cacheKey) const;
  std::optional<QJsonObject> readCache(const QString& path) const;
  void writeCache(const QString& path, const JobInfo& info, const QJsonObject& value) const;
  void applyThreadPriority() const;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::condition_variable doneCv_;
  std::map<QString, std::shared_ptr<Analyzer>> analyzers_;
  std::map<QString, std::unique_ptr<Job>> jobs_;
  std::vector<QString> order_; // submission order of all jobs (for list())
  QString cacheDir_;
  std::vector<std::thread> workers_;
  bool stopping_ = false;
  quint64 nextSeq_ = 1;
  int running_ = 0, mlRunning_ = 0;
  QString manualReason_;
  bool exportFlag_ = false, exportProbe_ = false, battery_ = false, vram_ = false, autoPause_ = true;
  std::function<bool()> busyProbe_;
  std::atomic<int> priorityLevel_{0}; // 0 idle, 1 below_normal, 2 normal
  std::atomic<int> mlThreads_{2};
  QString priorityName_ = QStringLiteral("idle");
  QTimer timer_;
};

} // namespace sf::jobs
