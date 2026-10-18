// Test suite for ComfyUI managed process lifecycle, liveness, and error detection.

#include "comfy/comfy_client.h"
#include "comfy/comfy_graph.h"
#include "comfy/comfy_process.h"
#include "comfy_fake_server.h"
#include "engine/engine.h"
#include "engine/cmd_comfy.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <future>
#include <thread>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace sf;
using namespace sf::comfy;
using namespace sf::engine;

class TstComfyProcess : public QObject {
  Q_OBJECT

  std::unique_ptr<QTemporaryDir> userTemplatesDir_;
  std::unique_ptr<QTemporaryDir> jobsCacheDir_;

  static QString q(const char* s) { return QString::fromLatin1(s); }

private slots:
  void initTestCase() {
    userTemplatesDir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(userTemplatesDir_->isValid());
    qputenv("SF_COMFY_TEMPLATES", userTemplatesDir_->path().toUtf8());

    jobsCacheDir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(jobsCacheDir_->isValid());
    qputenv("SF_JOBS_CACHE_DIR", jobsCacheDir_->path().toUtf8());
  }

  void cleanupTestCase() {
    qunsetenv("FAKE_COMFY_CRASH_AFTER_MS");
    qunsetenv("FAKE_COMFY_HANG_HISTORY");
    qunsetenv("SF_COMFY_URL");
    qunsetenv("SF_COMFY_HOME");
    qunsetenv("SF_COMFY_PYTHON");
  }

  // 1. Start reaches Ready; status reports the port and pid; logs contain the GUI line.
  void test1_startReadyStatusLogs() {
    ComfyProcess proc;
    proc.setProgramOverride(QStringLiteral(FAKE_COMFY_EXE));
    auto res = proc.start();
    QVERIFY2(res.ok, qPrintable(res.errorMessage));
    QCOMPARE(proc.state(), ComfyServerState::Ready);
    QVERIFY(proc.port() > 0);
    QVERIFY(proc.pid() > 0);
    QCOMPARE(proc.modeString(), QStringLiteral("managed"));

    // Poll logs up to 1 s if not immediately flushed
    QStringList logs;
    for (int i = 0; i < 20; ++i) {
      QCoreApplication::processEvents();
      logs = proc.recentLogs(100);
      bool found = false;
      for (const auto& line : logs) {
        if (line.contains(QLatin1String("To see the GUI go to:"))) {
          found = true;
          break;
        }
      }
      if (found) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    bool foundGui = false;
    for (const auto& line : logs) {
      if (line.contains(QLatin1String("To see the GUI go to:"))) {
        foundGui = true;
        break;
      }
    }
    QVERIFY(foundGui);
    proc.stop();
  }

  // 2. Stop reaches Stopped, and the pid is gone.
  void test2_stopStoppedPidGone() {
    ComfyProcess proc;
    proc.setProgramOverride(QStringLiteral(FAKE_COMFY_EXE));
    auto res = proc.start();
    QVERIFY(res.ok);
    const qint64 oldPid = proc.pid();
    QVERIFY(oldPid > 0);

    const bool stopped = proc.stop();
    QVERIFY(stopped);
    QCOMPARE(proc.state(), ComfyServerState::Stopped);
    QCOMPARE(proc.pid(), 0);

#if defined(Q_OS_WIN)
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(oldPid));
    if (h) {
      DWORD wr = WaitForSingleObject(h, 2000);
      CloseHandle(h);
      QCOMPARE(wr, WAIT_OBJECT_0);
    }
#endif
  }

  // 3. Crash after 1500 ms reaches Crashed within 1 s of the exit, with lastExit.code == 3.
  void test3_crashReachesCrashed() {
    qputenv("FAKE_COMFY_CRASH_AFTER_MS", "1500");
    ComfyProcess proc;
    proc.setProgramOverride(QStringLiteral(FAKE_COMFY_EXE));
    auto res = proc.start();
    QVERIFY(res.ok);

    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    while (proc.state() != ComfyServerState::Crashed && (QDateTime::currentMSecsSinceEpoch() - startMs) < 4000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    const qint64 elapsedMs = QDateTime::currentMSecsSinceEpoch() - startMs;
    qunsetenv("FAKE_COMFY_CRASH_AFTER_MS");

    QCOMPARE(proc.state(), ComfyServerState::Crashed);
    // Process exits at ~1500 ms; must be detected within 1 s of exit (total <= 2500 ms + margin)
    QVERIFY(elapsedMs <= 2500);

    const auto lastExit = proc.lastExit();
    QCOMPARE(lastExit.value(QStringLiteral("code")).toInt(), 3);
  }

  // 4. Crash during a running comfy.generate job (FAKE_COMFY_HANG_HISTORY=1 + crash after 2000 ms):
  //    the job fails with an error starting comfy_crashed within 1.5 s of the exit.
  void test4_crashDuringJob() {
    qputenv("FAKE_COMFY_HANG_HISTORY", "1");
    qputenv("FAKE_COMFY_CRASH_AFTER_MS", "2000");
    ComfyProcess::setGlobalProgramOverride(QStringLiteral(FAKE_COMFY_EXE));

    Engine engine;
    qint64 crashTimeMs = 0;
    QObject::connect(&engine, &Engine::event, [&](const QString& name, const QJsonObject& data, qint64) {
      if (name == QLatin1String("comfy.state") && data.value(QLatin1String("state")).toString() == QLatin1String("crashed")) {
        crashTimeMs = QDateTime::currentMSecsSinceEpoch();
      }
    });

    Result rStart = engine.call(q("comfy.server.start"), {});
    QVERIFY(rStart.ok);

    Result rTmpl = engine.call(q("comfy.graph.fromTemplate"), {
      {q("templateId"), QStringLiteral("sd15_txt2img")},
      {q("params"), QJsonObject{{q("prompt"), QStringLiteral("crash test")}, {q("steps"), 20}, {q("seed"), 12345}}}
    });
    QVERIFY(rTmpl.ok);
    const QString gid = rTmpl.value.toObject().value(q("graphId")).toString();

    Result rRun = engine.call(q("comfy.run"), {
      {q("graphId"), gid},
      {q("wait"), false}
    });
    QVERIFY(rRun.ok);
    const QString jobId = rRun.value.toObject().value(q("jobId")).toString();

    // Poll until job reaches terminal state
    while (true) {
      auto info = engine.jobs().info(jobId);
      if (info && (info->state == jobs::JobState::Failed || info->state == jobs::JobState::Done)) {
        break;
      }
      QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    const qint64 jobFailedMs = QDateTime::currentMSecsSinceEpoch();
    qunsetenv("FAKE_COMFY_HANG_HISTORY");
    qunsetenv("FAKE_COMFY_CRASH_AFTER_MS");
    ComfyProcess::setGlobalProgramOverride({});

    auto finalInfo = engine.jobs().info(jobId);
    QVERIFY(finalInfo.has_value());
    QCOMPARE(finalInfo->state, jobs::JobState::Failed);
    QVERIFY2(finalInfo->error.startsWith(QLatin1String("comfy_crashed")), qPrintable(finalInfo->error));
    // Failure must arrive within 1.5 s of the crash
    QVERIFY(crashTimeMs > 0);
    QVERIFY2(jobFailedMs - crashTimeMs <= 1500, qPrintable(QString("Took %1 ms after crash").arg(jobFailedMs - crashTimeMs)));
  }

  // 5. External mode: the fake server stops responding, the state becomes Unreachable after 3 misses,
  //    and comes back to Ready when it responds again.
  void test5_externalModeLiveness() {
    FakeComfyServer server;
    ComfyProcess proc;
    proc.setPingIntervalMs(50);
    proc.setPingTimeoutMs(50);
    proc.configureExternal(server.url());

    // Wait until Ready
    const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
    while (proc.state() != ComfyServerState::Ready && (QDateTime::currentMSecsSinceEpoch() - t0) < 2000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    QCOMPARE(proc.state(), ComfyServerState::Ready);

    // Stop responding
    server.setResponding(false);

    // Wait for 3 misses -> Unreachable
    const qint64 t1 = QDateTime::currentMSecsSinceEpoch();
    while (proc.state() != ComfyServerState::Unreachable && (QDateTime::currentMSecsSinceEpoch() - t1) < 2000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    QCOMPARE(proc.state(), ComfyServerState::Unreachable);

    // Resume responding -> Ready
    server.setResponding(true);

    const qint64 t2 = QDateTime::currentMSecsSinceEpoch();
    while (proc.state() != ComfyServerState::Ready && (QDateTime::currentMSecsSinceEpoch() - t2) < 2000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    QCOMPARE(proc.state(), ComfyServerState::Ready);
    proc.stop();
  }

  // 6. Job lost: history stays {} and the queue is empty, so the job fails with comfy_lost.
  void test6_jobLost() {
    FakeComfyServer server;
    server.setHangHistory(true);
    server.setEmptyQueue(true);
    qputenv("SF_COMFY_URL", server.url().toString().toUtf8());

    Engine engine;

    Result rTmpl = engine.call(q("comfy.graph.fromTemplate"), {
      {q("templateId"), QStringLiteral("sd15_txt2img")},
      {q("params"), QJsonObject{{q("prompt"), QStringLiteral("lost job test")}, {q("steps"), 20}, {q("seed"), 555}}}
    });
    QVERIFY(rTmpl.ok);
    const QString gid = rTmpl.value.toObject().value(q("graphId")).toString();

    Result rRun = engine.call(q("comfy.run"), {
      {q("graphId"), gid},
      {q("wait"), true}
    });
    qunsetenv("SF_COMFY_URL");
    QVERIFY(!rRun.ok);
    QVERIFY2(rRun.error.message.contains(QLatin1String("comfy_lost")), qPrintable(rRun.error.message));
  }

  // 7. Deadline: timeoutMs: 1500 with a hung history fails with comfy_timeout, and /interrupt was called.
  void test7_deadlineToInterrupt() {
    FakeComfyServer server;
    server.setHangHistory(true);
    qputenv("SF_COMFY_URL", server.url().toString().toUtf8());

    Engine engine;

    Result rTmpl = engine.call(q("comfy.graph.fromTemplate"), {
      {q("templateId"), QStringLiteral("sd15_txt2img")},
      {q("params"), QJsonObject{{q("prompt"), QStringLiteral("timeout test")}, {q("steps"), 20}, {q("seed"), 777}}}
    });
    QVERIFY(rTmpl.ok);
    const QString gid = rTmpl.value.toObject().value(q("graphId")).toString();

    Result rRun = engine.call(q("comfy.run"), {
      {q("graphId"), gid},
      {q("wait"), true},
      {q("timeoutMs"), 1500}
    });
    qunsetenv("SF_COMFY_URL");
    QVERIFY(!rRun.ok);
    QVERIFY2(rRun.error.message.contains(QLatin1String("comfy_timeout")), qPrintable(rRun.error.message));
    QVERIFY(server.wasInterrupted());
  }

  // 8. generate.shot on a failed job returns comfy_error, not pending.
  void test8_generateShotFailedReturnsError() {
    FakeComfyServer server;
    server.setHangHistory(true);
    server.setEmptyQueue(true);
    qputenv("SF_COMFY_URL", server.url().toString().toUtf8());

    Engine engine;
    engine.createSession();

    Result rShot = engine.call(q("generate.shot"), {
      {q("templateId"), QStringLiteral("sd15_txt2img")},
      {q("prompt"), QStringLiteral("shot fail test")},
      {q("wait"), true},
      {q("timeoutMs"), 5000}
    });
    qunsetenv("SF_COMFY_URL");
    QVERIFY(!rShot.ok);
    QCOMPARE(rShot.error.code, QStringLiteral("comfy_error"));
    QVERIFY(!rShot.value.toObject().value(q("pending")).toBool(false));
  }

  // 9. Missing python / home gives not_configured.
  void test9_missingPythonHomeNotConfigured() {
    ComfyProcess proc;
    proc.setProgramOverride({});
    auto res = proc.start(QStringLiteral("C:/nonexistent_folder_abc123"), QStringLiteral("C:/nonexistent_python_xyz987"));
    QVERIFY(!res.ok);
    QCOMPARE(res.errorCode, QStringLiteral("not_configured"));
  }

  // 10. Auto-restart: two crashes restart; the third stays Crashed.
  void test10_autoRestartTwoCrashesThirdStaysCrashed() {
    qputenv("FAKE_COMFY_CRASH_AFTER_MS", "1500");

    ComfyProcess proc;
    proc.setProgramOverride(QStringLiteral(FAKE_COMFY_EXE));
    proc.setAutoStart(true);
    proc.setRestartBackoffMs(50);

    auto res = proc.start();
    QVERIFY2(res.ok, qPrintable(res.errorMessage + " " + res.errorCode));

    // Wait for 2 restarts and final crash (3 crashes total)
    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    while (proc.restarts() < 2 && (QDateTime::currentMSecsSinceEpoch() - startMs) < 15000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    while (proc.state() != ComfyServerState::Ready && (QDateTime::currentMSecsSinceEpoch() - startMs) < 15000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    while (proc.state() != ComfyServerState::Crashed && (QDateTime::currentMSecsSinceEpoch() - startMs) < 15000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    QCOMPARE(proc.restarts(), 2);
    QCOMPARE(proc.state(), ComfyServerState::Crashed);

    // Wait an additional 300 ms to confirm it stays Crashed
    const qint64 waitMore = QDateTime::currentMSecsSinceEpoch() + 300;
    while (QDateTime::currentMSecsSinceEpoch() < waitMore) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    QCOMPARE(proc.restarts(), 2);
    QCOMPARE(proc.state(), ComfyServerState::Crashed);

    qunsetenv("FAKE_COMFY_CRASH_AFTER_MS");
  }

  // 11. External mode start returns invalid within 2 s (hard timeout to prevent hanging on deadlock).
  void test11_externalModeStartReturnsInvalidWithin2s() {
    ComfyProcess proc;
    proc.configureExternal(QUrl(QStringLiteral("http://127.0.0.1:8188")));

    std::promise<ComfyProcess::StartResult> prom;
    auto fut = prom.get_future();
    std::thread([&]() {
      prom.set_value(proc.start());
    }).detach();

    QCOMPARE(fut.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto res = fut.get();
    QVERIFY(!res.ok);
    QCOMPARE(res.errorCode, QStringLiteral("invalid"));
    QVERIFY(res.errorMessage.contains(QStringLiteral("external ComfyUI configured at")));

    // Also verify through engine command
    Engine engine;
    engine.comfy().process->configureExternal(QUrl(QStringLiteral("http://127.0.0.1:8188")));
    std::promise<Result> cmdProm;
    auto cmdFut = cmdProm.get_future();
    std::thread([&]() {
      cmdProm.set_value(engine.call(q("comfy.server.start"), {}));
    }).detach();
    QCOMPARE(cmdFut.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto cmdRes = cmdFut.get();
    QVERIFY(!cmdRes.ok);
    QCOMPARE(cmdRes.error.code, QStringLiteral("invalid"));
    QVERIFY(cmdRes.error.message.contains(QStringLiteral("external ComfyUI configured at")));
  }

  // 12. External mode stop and destruction make NO /interrupt request.
  void test12_externalModeStopDoesNotInterrupt() {
    FakeComfyServer server;
    {
      ComfyProcess proc;
      proc.configureExternal(server.url());
      proc.stop();
    }
    QVERIFY(!server.wasInterrupted());
  }

  // 13. Crash handling idempotency: one crash uses exactly one restart slot.
  void test13_crashUsesOneRestartSlot() {
    qputenv("FAKE_COMFY_CRASH_AFTER_MS", "1500");

    ComfyProcess proc;
    proc.setProgramOverride(QStringLiteral(FAKE_COMFY_EXE));
    proc.setAutoStart(true);
    proc.setRestartBackoffMs(500);

    auto res = proc.start();
    QVERIFY2(res.ok, qPrintable(res.errorMessage + " " + res.errorCode));

    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    while (proc.restarts() < 1 && (QDateTime::currentMSecsSinceEpoch() - startMs) < 6000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    QCOMPARE(proc.restarts(), 1);

    proc.stop();
    qunsetenv("FAKE_COMFY_CRASH_AFTER_MS");
  }

  // 14. Stop during auto-restart backoff: cancel timer and do not restart.
  void test14_crashImmediatelyStopNoRestart() {
    qputenv("FAKE_COMFY_CRASH_AFTER_MS", "1500");

    ComfyProcess proc;
    proc.setProgramOverride(QStringLiteral(FAKE_COMFY_EXE));
    proc.setAutoStart(true);
    proc.setRestartBackoffMs(3000);

    auto res = proc.start();
    QVERIFY2(res.ok, qPrintable(res.errorMessage + " " + res.errorCode));

    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    while (proc.state() != ComfyServerState::Crashed && (QDateTime::currentMSecsSinceEpoch() - startMs) < 6000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    QCOMPARE(proc.state(), ComfyServerState::Crashed);

    proc.stop();
    QCOMPARE(proc.state(), ComfyServerState::Stopped);

    const qint64 waitEnd = QDateTime::currentMSecsSinceEpoch() + 4000;
    while (QDateTime::currentMSecsSinceEpoch() < waitEnd) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    QCOMPARE(proc.state(), ComfyServerState::Stopped);
    QCOMPARE(proc.pid(), 0);

    qunsetenv("FAKE_COMFY_CRASH_AFTER_MS");
  }

  // 15. Teardown order: destroy Engine while comfy.generate polls a hung fake server (exits cleanly in <= 3 s).
  void test15_destroyEngineWhileJobPollingHungServer() {
    FakeComfyServer server;
    server.setHangHistory(true);
    qputenv("SF_COMFY_URL", server.url().toString().toUtf8());

    const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
    {
      Engine engine;

      Result rTmpl = engine.call(q("comfy.graph.fromTemplate"), {
        {q("templateId"), QStringLiteral("sd15_txt2img")},
        {q("params"), QJsonObject{{q("prompt"), QStringLiteral("hung test")}, {q("steps"), 20}, {q("seed"), 12345}}}
      });
      QVERIFY(rTmpl.ok);
      const QString gid = rTmpl.value.toObject().value(q("graphId")).toString();

      Result rRun = engine.call(q("comfy.run"), {
        {q("graphId"), gid},
        {q("wait"), false}
      });
      QVERIFY(rRun.ok);

      const qint64 pollStart = QDateTime::currentMSecsSinceEpoch();
      while ((QDateTime::currentMSecsSinceEpoch() - pollStart) < 300) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    const qint64 elapsedMs = QDateTime::currentMSecsSinceEpoch() - t0;
    qunsetenv("SF_COMFY_URL");
    QVERIFY2(elapsedMs <= 3000, qPrintable(QString("Engine destruction took %1 ms").arg(elapsedMs)));
  }
};

QTEST_MAIN(TstComfyProcess)
#include "tst_comfy_process.moc"
