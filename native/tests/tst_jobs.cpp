// Background analysis jobs: scheduler ordering, disk cache, pause / resume, auto-pause, cancel, the one-ML-job rule; ONNX Runtime and the
// hands analyzer (QSKIP when ONNX Runtime or the models are missing); the waveform analyzer as the non-ML job.
// Inference only ever sees a handful of tiny frames.

#include "jobs/builtin_analyzers.h"
#include "jobs/hands_tracker.h"
#include "jobs/job_scheduler.h"
#include "jobs/ort_runtime.h"

#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

using namespace sf::jobs;

namespace {

QString Q(const char* s) { return QString::fromLatin1(s); }

// Counts its calls, records the order of its inputs, can be slow (steps with a checkpoint each) and ML-flagged.
class Fake final : public Analyzer {
public:
  explicit Fake(bool ml = false, int version = 1) : ml_(ml), version_(version) {}
  QString id() const override { return Q("fake"); }
  int version() const override { return version_; }
  bool isMl() const override { return ml_; }
  QJsonObject defaultParams() const override { return {{Q("n"), 1}}; }
  AnalyzerResult run(JobContext& ctx) override {
    ++calls;
    const int now = ++concurrent;
    int seen = maxConcurrent.load();
    while (now > seen && !maxConcurrent.compare_exchange_weak(seen, now)) {}
    {
      std::lock_guard<std::mutex> lk(mu);
      order << ctx.path;
    }
    for (int i = 0; i < steps; ++i) {
      if (ctx.stop()) {
        --concurrent;
        return AnalyzerResult::failure(Q("cancelled"));
      }
      ++stepsDone;
      ctx.report((i + 1.0) / steps);
      if (stepMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
    }
    --concurrent;
    return AnalyzerResult::success({{Q("path"), ctx.path}, {Q("n"), ctx.params.value(Q("n")).toInt()}});
  }
  std::atomic<int> calls{0}, concurrent{0}, maxConcurrent{0}, stepsDone{0};
  int steps = 1, stepMs = 0;
  QStringList order;
  std::mutex mu;

private:
  bool ml_;
  int version_;
};

JobRequest req(const QString& key, int priority = 0, const QJsonObject& params = {}) {
  JobRequest r;
  r.analyzer = Q("fake");
  r.assetId = key;
  r.assetKey = key;
  r.path = key;
  r.priority = priority;
  r.params = params;
  return r;
}

bool writeWav(const QString& path, double seconds) {
  const int sr = 48000;
  const int n = static_cast<int>(seconds * sr);
  QByteArray pcm(n * 2, 0);
  for (int i = 0; i < n; ++i) reinterpret_cast<qint16*>(pcm.data())[i] = static_cast<qint16>(8000.0 * std::sin(6.283185307179586 * 440.0 * i / sr));
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly)) return false;
  const auto le32 = [&](quint32 v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  const auto le16 = [&](quint16 v) { f.write(reinterpret_cast<const char*>(&v), 2); };
  f.write("RIFF");
  le32(36 + static_cast<quint32>(pcm.size()));
  f.write("WAVEfmt ");
  le32(16);
  le16(1);
  le16(1);
  le32(sr);
  le32(sr * 2);
  le16(2);
  le16(16);
  f.write("data");
  le32(static_cast<quint32>(pcm.size()));
  f.write(pcm);
  return true;
}

} // namespace

class TstJobs : public QObject {
  Q_OBJECT

  QTemporaryDir dir_;

  std::unique_ptr<JobScheduler> make(const std::shared_ptr<Analyzer>& a, const QString& sub) {
    auto s = std::make_unique<JobScheduler>();
    s->setCacheDir(dir_.filePath(sub));
    s->setAutoPause(true);
    if (a) s->registerAnalyzer(a);
    return s;
  }

private slots:
  void initTestCase() { QVERIFY(dir_.isValid()); }

  void priorityOrderAndOneMlAtATime() {
    auto fake = std::make_shared<Fake>(true);
    fake->stepMs = 5;
    fake->steps = 3;
    auto s = make(fake, Q("order"));
    s->pause(Q("test"));
    QStringList ids;
    for (const auto& [key, prio] : {std::pair<QString, int>{Q("A"), 0}, {Q("B"), 5}, {Q("C"), 0}, {Q("D"), 9}}) {
      const Submitted sub = s->enqueue(req(key, prio));
      QVERIFY2(sub.ok, qPrintable(sub.error));
      ids << sub.jobId;
    }
    QCOMPARE(s->list().size(), size_t(4));
    QCOMPARE(s->list().front().assetId, Q("D")); // the queue lists by priority
    s->resume();
    QVERIFY(s->waitIdle(10000));
    QCOMPARE(fake->order, QStringList({Q("D"), Q("B"), Q("A"), Q("C")}));
    QCOMPARE(fake->maxConcurrent.load(), 1); // ML jobs never overlap
    for (const QString& id : ids) QCOMPARE(s->info(id)->state, JobState::Done);
  }

  void cacheHitMakesNoAnalyzerCall() {
    auto fake = std::make_shared<Fake>();
    auto s = make(fake, Q("cache"));
    const Submitted a = s->enqueue(req(Q("clip1")));
    QVERIFY(a.ok && !a.cached);
    QVERIFY(s->wait(a.jobId, 5000));
    QCOMPARE(fake->calls.load(), 1);
    QCOMPARE(s->result(a.jobId)->value(Q("path")).toString(), Q("clip1"));

    const Submitted b = s->enqueue(req(Q("clip1"))); // same key: instant
    QVERIFY(b.ok && b.cached);
    QCOMPARE(s->info(b.jobId)->state, JobState::Done);
    QVERIFY(s->info(b.jobId)->cached);
    QCOMPARE(fake->calls.load(), 1);
    QVERIFY(s->cached(req(Q("clip1"))).has_value());
    QVERIFY(!s->cached(req(Q("clip2"))).has_value());

    // params that normalize to the same thing share the entry; a different value does not
    QVERIFY(s->enqueue(req(Q("clip1"), 0, {{Q("n"), 1}})).cached);
    const Submitted c = s->enqueue(req(Q("clip1"), 0, {{Q("n"), 2}}));
    QVERIFY(!c.cached);
    QVERIFY(s->wait(c.jobId, 5000));
    QCOMPARE(fake->calls.load(), 2);
    QCOMPARE(s->result(c.jobId)->value(Q("n")).toInt(), 2);

    // a new analyzer version starts over; the cache survives a new scheduler on the same folder
    s->registerAnalyzer(std::make_shared<Fake>(false, 2));
    QVERIFY(!s->enqueue(req(Q("clip1"))).cached);
    s->waitIdle(5000);
    auto again = make(std::make_shared<Fake>(), Q("cache"));
    QVERIFY(again->enqueue(req(Q("clip1"))).cached);
  }

  void identicalQueuedJobsAreShared() {
    auto fake = std::make_shared<Fake>();
    auto s = make(fake, Q("dedupe"));
    s->pause(Q("test"));
    const Submitted a = s->enqueue(req(Q("x")));
    const Submitted b = s->enqueue(req(Q("x"), 7));
    QCOMPARE(b.jobId, a.jobId);
    QVERIFY(b.deduped);
    QCOMPARE(s->info(a.jobId)->priority, 7);
    s->resume();
    QVERIFY(s->wait(a.jobId, 5000));
    QCOMPARE(fake->calls.load(), 1);
    QVERIFY(!s->enqueue(JobRequest{Q("nope"), {}, Q("k"), Q("p"), {}, 0}).ok); // unknown analyzer: a clean error
  }

  void pauseHoldsQueueAndRunningJob() {
    auto fake = std::make_shared<Fake>();
    auto s = make(fake, Q("pause"));
    s->pause(Q("llm generating"));
    QVERIFY(s->paused());
    QCOMPARE(s->pauseReasons(), QStringList({Q("manual:llm generating")}));
    const Submitted a = s->enqueue(req(Q("p1")));
    QVERIFY(!s->wait(a.jobId, 250));
    QCOMPARE(s->info(a.jobId)->state, JobState::Queued);
    QCOMPARE(fake->calls.load(), 0);
    s->resume();
    QVERIFY(s->wait(a.jobId, 5000));

    // a running job holds at its next checkpoint
    fake->steps = 200;
    fake->stepMs = 5;
    const Submitted b = s->enqueue(req(Q("p2")));
    QTRY_VERIFY_WITH_TIMEOUT(s->info(b.jobId)->state == JobState::Running, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(fake->stepsDone.load() > 5, 3000);
    s->pause(Q("again"));
    QThread::msleep(80); // let the step in flight finish
    const int frozen = fake->stepsDone.load();
    QThread::msleep(150);
    QCOMPARE(fake->stepsDone.load(), frozen);
    QVERIFY(s->info(b.jobId)->paused);
    s->resume();
    QVERIFY(s->wait(b.jobId, 8000));
    QCOMPARE(s->info(b.jobId)->state, JobState::Done);
  }

  void autoPauseDuringExportBatteryAndVram() {
    auto fake = std::make_shared<Fake>();
    auto s = make(fake, Q("auto"));
    QSignalSpy spy(s.get(), &JobScheduler::pauseChanged);
    s->setExportActive(true); // the simulated export flag
    QVERIFY(s->paused());
    QVERIFY(s->pauseReasons().contains(Q("auto:export")));
    QVERIFY(spy.count() >= 1);
    const Submitted a = s->enqueue(req(Q("e1")));
    QVERIFY(!s->wait(a.jobId, 250));
    QCOMPARE(fake->calls.load(), 0);
    s->setExportActive(false);
    QVERIFY(s->wait(a.jobId, 5000));

    s->applySystem(true, false);
    QVERIFY(s->pauseReasons().contains(Q("auto:battery")));
    s->applySystem(false, true);
    QCOMPARE(s->pauseReasons(), QStringList({Q("auto:vram")}));
    const Submitted b = s->enqueue(req(Q("e2")));
    QVERIFY(!s->wait(b.jobId, 200));
    s->setAutoPause(false); // ignoring the machine: it runs
    QVERIFY(!s->paused());
    QVERIFY(s->wait(b.jobId, 5000));
    s->setAutoPause(true);
    QVERIFY(s->paused());
    s->applySystem(false, false);
    QVERIFY(!s->paused());

    // the busy probe the engine installs (an export record is "running")
    std::atomic<bool> busy{true};
    s->setBusyProbe([&busy] { return busy.load(); });
    s->refreshAuto();
    QVERIFY(s->pauseReasons().contains(Q("auto:export")));
    busy = false;
    s->refreshAuto();
    QVERIFY(!s->paused());
  }

  void cancelQueuedAndRunning() {
    auto fake = std::make_shared<Fake>();
    fake->steps = 1000;
    fake->stepMs = 5;
    auto s = make(fake, Q("cancel"));
    const Submitted run = s->enqueue(req(Q("c1")));
    s->pause(Q("hold")); // the second job stays queued
    const Submitted queued = s->enqueue(req(Q("c2")));
    QVERIFY(s->cancel(queued.jobId));
    QCOMPARE(s->info(queued.jobId)->state, JobState::Cancelled);
    s->resume();
    QTRY_VERIFY_WITH_TIMEOUT(s->info(run.jobId)->progress > 0.01, 5000);
    QVERIFY(s->info(run.jobId)->state == JobState::Running);
    QVERIFY(s->cancel(run.jobId));
    QVERIFY(s->wait(run.jobId, 5000));
    QCOMPARE(s->info(run.jobId)->state, JobState::Cancelled);
    QVERIFY(!s->cached(req(Q("c1"))).has_value()); // nothing of a cancelled job is cached
    QVERIFY(!s->cancel(run.jobId));                // already finished
    QCOMPARE(fake->order.size(), 1);               // the queued job never ran
  }

  void waveformIsAGenericJob() {
    const QString wav = dir_.filePath(Q("tone.wav"));
    QVERIFY(writeWav(wav, 1.0));
    JobScheduler s;
    s.setCacheDir(dir_.filePath(Q("wave")));
    registerBuiltinAnalyzers(s);
    QVERIFY(s.analyzerIds().contains(Q("waveform")));
    JobRequest r;
    r.analyzer = Q("waveform");
    r.assetId = Q("tone");
    r.assetKey = Q("tone-v1");
    r.path = wav;
    r.params = {{Q("buckets"), 50}};
    const Submitted a = s.enqueue(r);
    QVERIFY2(a.ok, qPrintable(a.error));
    QVERIFY(s.wait(a.jobId, 10000));
    QCOMPARE(s.info(a.jobId)->state, JobState::Done);
    const QJsonObject res = *s.result(a.jobId);
    QVERIFY(res.value(Q("buckets")).toInt() <= 50 && res.value(Q("buckets")).toInt() > 10);
    QVERIFY(res.value(Q("peak")).toDouble() > 0.2 && res.value(Q("peak")).toDouble() < 0.3);
    QVERIFY(s.enqueue(r).cached);
    r.path = dir_.filePath(Q("missing.wav"));
    r.assetKey = Q("missing");
    const Submitted bad = s.enqueue(r);
    QVERIFY(bad.ok);
    QVERIFY(s.wait(bad.jobId, 10000));
    QCOMPARE(s.info(bad.jobId)->state, JobState::Failed);
    QVERIFY(!s.info(bad.jobId)->error.isEmpty());
  }

  // ---- ONNX Runtime and hands ----

  void missingModelIsACleanError() {
    const QByteArray old = qgetenv("SF_MODELS_DIR");
    qputenv("SF_MODELS_DIR", dir_.filePath(Q("no-models")).toUtf8());
    const QString why = HandTracker::unavailableReason();
    QVERIFY(!why.isEmpty());
    QString err;
    QVERIFY(!HandTracker::create(2, &err));
    QVERIFY(!err.isEmpty());
    JobScheduler s;
    s.setCacheDir(dir_.filePath(Q("nomodel")));
    registerBuiltinAnalyzers(s);
    const Submitted sub = s.enqueue([&] { JobRequest r; r.analyzer = Q("hands"); r.assetKey = Q("k"); r.path = dir_.filePath(Q("x.png")); return r; }());
    QVERIFY(!sub.ok);
    QVERIFY(sub.error.contains(Q("unavailable")));
    QString loadErr;
    QVERIFY(!sf::jobs::ort::Model::load(dir_.filePath(Q("missing.onnx")), 1, &loadErr));
    QVERIFY(!loadErr.isEmpty());
    if (old.isEmpty()) qunsetenv("SF_MODELS_DIR"); else qputenv("SF_MODELS_DIR", old);
  }

  void ortSessionCreation() {
    if (!sf::jobs::ort::compiledIn()) QSKIP("built without ONNX Runtime");
    QString err;
    const QString palm = sf::jobs::ort::findModel(Q("hands/palm_detection_mediapipe_2023feb.onnx"), &err);
    const QString pose = sf::jobs::ort::findModel(Q("hands/handpose_estimation_mediapipe_2023feb.onnx"), &err);
    if (palm.isEmpty() || pose.isEmpty()) QSKIP(qPrintable(err));
    qInfo().noquote() << "ONNX Runtime" << sf::jobs::ort::runtimeVersion();
    auto m = sf::jobs::ort::Model::load(palm, 2, &err);
    QVERIFY2(m, qPrintable(err));
    QCOMPARE(m->inputShape(), (std::vector<int64_t>{1, 192, 192, 3}));
    QCOMPARE(m->outputNames().size(), 2);
    QCOMPARE(m->intraThreads(), 2);
    auto p = sf::jobs::ort::Model::load(pose, 1, &err);
    QVERIFY2(p, qPrintable(err));
    QCOMPARE(p->inputShape(), (std::vector<int64_t>{1, 224, 224, 3}));
    QCOMPARE(p->outputNames().size(), 4);
    // a wrong-sized input is an error value, not a crash
    std::vector<sf::jobs::ort::Tensor> out;
    QVERIFY(!m->run(std::vector<float>(10), {1, 192, 192, 3}, &out, &err));
    QVERIFY(!err.isEmpty());
    QCOMPARE(HandTracker::palmAnchors().size(), size_t(2016));
  }

  void handsOnBlankFrameFindsNone() {
    if (!sf::jobs::ort::compiledIn()) QSKIP("built without ONNX Runtime");
    if (!HandTracker::unavailableReason().isEmpty()) QSKIP(qPrintable(HandTracker::unavailableReason()));
    QString err;
    auto t = HandTracker::create(2, &err);
    QVERIFY2(t, qPrintable(err));
    QImage blank(160, 90, QImage::Format_RGBA8888);
    blank.fill(QColor(128, 128, 128));
    QVERIFY(t->detect(blank, {}, &err).empty());
    QVERIFY2(err.isEmpty(), qPrintable(err));
    // a tall frame and a gradient with an odd size run through the letterbox without trouble
    QImage grad(97, 211, QImage::Format_RGBA8888);
    for (int y = 0; y < grad.height(); ++y)
      for (int x = 0; x < grad.width(); ++x) grad.setPixelColor(x, y, QColor(x * 2, y, (x + y) % 255));
    QVERIFY(t->detect(grad, {}, &err).size() <= 2);
    QVERIFY2(err.isEmpty(), qPrintable(err));
  }

  void handsJobEndToEnd() {
    if (!sf::jobs::ort::compiledIn()) QSKIP("built without ONNX Runtime");
    if (!HandTracker::unavailableReason().isEmpty()) QSKIP(qPrintable(HandTracker::unavailableReason()));
    const QString png = dir_.filePath(Q("blank.png"));
    QImage blank(128, 72, QImage::Format_RGB32);
    blank.fill(Qt::black);
    QVERIFY(blank.save(png));
    JobScheduler s;
    s.setCacheDir(dir_.filePath(Q("hands")));
    s.setResources(Q("below_normal"), 2);
    registerBuiltinAnalyzers(s);
    JobRequest r;
    r.analyzer = Q("hands");
    r.assetId = Q("blank");
    r.assetKey = Q("blank-v1");
    r.path = png;
    r.params = {{Q("fps"), 5}};
    const Submitted a = s.enqueue(r);
    QVERIFY2(a.ok, qPrintable(a.error));
    QVERIFY(s.wait(a.jobId, 30000));
    const auto info = s.info(a.jobId);
    QVERIFY2(info->state == JobState::Done, qPrintable(info->error));
    const QJsonObject res = *s.result(a.jobId);
    QCOMPARE(res.value(Q("frameCount")).toInt(), 1);
    QCOMPARE(res.value(Q("framesWithHands")).toInt(), 0);
    const QJsonObject f0 = res.value(Q("frames")).toArray().at(0).toObject();
    QVERIFY(f0.contains(Q("t")));
    QCOMPARE(f0.value(Q("hands")).toArray().size(), 0);
    QVERIFY(s.enqueue(r).cached); // second run: the cache
  }

  // Set SF_HANDS_TEST_IMAGE to a picture with a visible hand to sanity-check the landmarks (none ships with the repo).
  void handsOnARealPicture() {
    const QString path = qEnvironmentVariable("SF_HANDS_TEST_IMAGE");
    if (path.isEmpty()) QSKIP("SF_HANDS_TEST_IMAGE is not set (no hand picture on disk)");
    if (!sf::jobs::ort::compiledIn() || !HandTracker::unavailableReason().isEmpty()) QSKIP("ONNX Runtime or models missing");
    QString err;
    auto t = HandTracker::create(2, &err);
    QVERIFY2(t, qPrintable(err));
    const auto hands = t->detect(QImage(path), {}, &err);
    QVERIFY2(!hands.empty(), "no hand found");
    for (const Hand& h : hands) {
      QVERIFY(h.score >= 0.8);
      double minX = 2, maxX = -1, minY = 2, maxY = -1;
      for (const auto& p : h.landmarks) {
        minX = std::min(minX, p[0]);
        maxX = std::max(maxX, p[0]);
        minY = std::min(minY, p[1]);
        maxY = std::max(maxY, p[1]);
      }
      QVERIFY(minX > -0.2 && maxX < 1.2 && minY > -0.2 && maxY < 1.2);
      QVERIFY(maxX - minX > 0.03 || maxY - minY > 0.03);
    }
  }
};

QTEST_MAIN(TstJobs)
#include "tst_jobs.moc"
