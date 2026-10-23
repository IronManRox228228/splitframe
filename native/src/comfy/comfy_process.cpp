// ComfyUI child process lifecycle manager, output logger, and liveness monitor.

#include "comfy/comfy_process.h"
#include "comfy/comfy_client.h"

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTimer>

#include <algorithm>
#include <chrono>
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
#if defined(Q_OS_LINUX)
#include <sys/prctl.h>
#include <signal.h>
#endif

namespace sf::comfy {

namespace {

std::mutex s_instanceMutex;
QString s_globalProgramOverride;

QString autoDetectPython(const QString& homeDir) {
  if (homeDir.isEmpty()) return {};
  QDir home(homeDir);
  if (!home.exists()) return {};

#ifdef Q_OS_WIN
  const QStringList candidates = {
    home.filePath(QStringLiteral("../python_embeded/python.exe")),
    home.filePath(QStringLiteral("venv/Scripts/python.exe")),
    home.filePath(QStringLiteral(".venv/Scripts/python.exe"))
  };
#else
  const QStringList candidates = {
    home.filePath(QStringLiteral("../python_embeded/bin/python")),
    home.filePath(QStringLiteral("venv/bin/python")),
    home.filePath(QStringLiteral(".venv/bin/python"))
  };
#endif

  for (const QString& c : candidates) {
    QFileInfo fi(c);
    if (fi.exists() && fi.isFile()) {
      return fi.canonicalFilePath();
    }
  }
  return {};
}

} // namespace

void ComfyProcess::setGlobalProgramOverride(const QString& prog) {
  std::lock_guard<std::mutex> lk(s_instanceMutex);
  s_globalProgramOverride = prog;
}

QString ComfyProcess::globalProgramOverride() {
  std::lock_guard<std::mutex> lk(s_instanceMutex);
  return s_globalProgramOverride;
}

ComfyProcess::ComfyProcess(QObject* parent) : QObject(parent) {
  {
    std::lock_guard<std::mutex> lk(s_instanceMutex);
    programOverride_ = s_globalProgramOverride;
  }

  // Load configuration from QSettings and environment
  QSettings s;
  s.beginGroup(QStringLiteral("comfy"));
  home_ = s.value(QStringLiteral("home")).toString();
  python_ = s.value(QStringLiteral("python")).toString();
  args_ = s.value(QStringLiteral("args")).toStringList();
  autoStart_ = s.value(QStringLiteral("autoStart"), false).toBool();
  QString cfgUrl = s.value(QStringLiteral("url")).toString();
  s.endGroup();

  const auto env = QProcessEnvironment::systemEnvironment();
  if (env.contains(QStringLiteral("SF_COMFY_HOME"))) {
    home_ = env.value(QStringLiteral("SF_COMFY_HOME"));
  }
  if (env.contains(QStringLiteral("SF_COMFY_PYTHON"))) {
    python_ = env.value(QStringLiteral("SF_COMFY_PYTHON"));
  }
  if (env.contains(QStringLiteral("SF_COMFY_URL"))) {
    cfgUrl = env.value(QStringLiteral("SF_COMFY_URL"));
  }

  if (!cfgUrl.isEmpty()) {
    configureExternal(QUrl(cfgUrl));
  } else if (!home_.isEmpty() || !python_.isEmpty()) {
    mode_ = QStringLiteral("managed");
  }
}

ComfyProcess::~ComfyProcess() {
  if (restartTimer_) {
    restartTimer_->stop();
  }

  stop();

#ifdef Q_OS_WIN
  if (jobHandle_) {
    TerminateJobObject(static_cast<HANDLE>(jobHandle_), 1);
    CloseHandle(static_cast<HANDLE>(jobHandle_));
    jobHandle_ = nullptr;
  }
#endif
}

QString ComfyProcess::modeString() const {
  std::lock_guard<std::mutex> lk(mu_);
  return mode_;
}

ComfyServerState ComfyProcess::state() const {
  std::lock_guard<std::mutex> lk(mu_);
  return state_;
}

QUrl ComfyProcess::url() const {
  std::lock_guard<std::mutex> lk(mu_);
  return url_;
}

int ComfyProcess::port() const {
  std::lock_guard<std::mutex> lk(mu_);
  return port_;
}

qint64 ComfyProcess::pid() const {
  std::lock_guard<std::mutex> lk(mu_);
  return pid_;
}

double ComfyProcess::uptimeSec() const {
  std::lock_guard<std::mutex> lk(mu_);
  if (state_ != ComfyServerState::Ready || readyTime_ == 0) return 0.0;
  return std::max(0.0, (QDateTime::currentMSecsSinceEpoch() - readyTime_) / 1000.0);
}

int ComfyProcess::restarts() const {
  std::lock_guard<std::mutex> lk(mu_);
  return restarts_;
}

QString ComfyProcess::home() const {
  std::lock_guard<std::mutex> lk(mu_);
  return home_;
}

QString ComfyProcess::python() const {
  std::lock_guard<std::mutex> lk(mu_);
  return python_;
}

QJsonObject ComfyProcess::lastExit() const {
  std::lock_guard<std::mutex> lk(mu_);
  return lastExit_;
}

QStringList ComfyProcess::recentLogs(int maxLines) const {
  std::lock_guard<std::mutex> lk(mu_);
  return getRecentLogsLocked(maxLines);
}

QStringList ComfyProcess::getRecentLogsLocked(int maxLines) const {
  if (maxLines <= 0) return {};
  const int count = std::min<int>(maxLines, static_cast<int>(logRing_.size()));
  QStringList out;
  out.reserve(count);
  auto it = logRing_.end() - count;
  for (; it != logRing_.end(); ++it) {
    out.append(*it);
  }
  return out;
}

QJsonObject ComfyProcess::statusObjectLocked() const {
  QJsonObject st{
    {QStringLiteral("mode"), mode_},
    {QStringLiteral("state"), stateToString(state_)},
    {QStringLiteral("url"), url_.toString()},
    {QStringLiteral("port"), port_},
    {QStringLiteral("pid"), pid_},
    {QStringLiteral("uptimeSec"), (state_ == ComfyServerState::Ready && readyTime_ > 0)
                                      ? std::max(0.0, (QDateTime::currentMSecsSinceEpoch() - readyTime_) / 1000.0)
                                      : 0.0},
    {QStringLiteral("restarts"), restarts_},
    {QStringLiteral("home"), home_},
    {QStringLiteral("python"), python_}
  };
  if (!lastExit_.isEmpty()) {
    st.insert(QStringLiteral("lastExit"), lastExit_);
  }
  return st;
}

QJsonObject ComfyProcess::statusObject() const {
  std::lock_guard<std::mutex> lk(mu_);
  return statusObjectLocked();
}

void ComfyProcess::setProgramOverride(const QString& prog) {
  std::lock_guard<std::mutex> lk(mu_);
  programOverride_ = prog;
}

QString ComfyProcess::programOverride() const {
  std::lock_guard<std::mutex> lk(mu_);
  return programOverride_;
}

void ComfyProcess::setReadyTimeoutMs(int ms) {
  readyTimeoutMs_ = ms;
}

void ComfyProcess::setPingIntervalMs(int ms) {
  pingIntervalMs_ = ms;
  if (pingTimer_ && pingTimer_->isActive()) {
    pingTimer_->setInterval(ms);
  }
}

void ComfyProcess::setPingTimeoutMs(int ms) {
  pingTimeoutMs_ = ms;
}

void ComfyProcess::setRestartBackoffMs(int ms) {
  restartBackoffMs_ = ms;
}

void ComfyProcess::setAutoStart(bool enable) {
  std::lock_guard<std::mutex> lk(mu_);
  autoStart_ = enable;
}

bool ComfyProcess::autoStart() const {
  std::lock_guard<std::mutex> lk(mu_);
  return autoStart_;
}

void ComfyProcess::setState(ComfyServerState newState, const QJsonObject& info) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (state_ == newState) return;
    state_ = newState;
    if (state_ == ComfyServerState::Ready) {
      readyTime_ = QDateTime::currentMSecsSinceEpoch();
    } else if (state_ != ComfyServerState::Starting) {
      readyTime_ = 0;
    }
  }
  emit stateChanged(newState, info);
}

void ComfyProcess::configureExternal(const QUrl& url) {
  stop();

  {
    std::lock_guard<std::mutex> lk(mu_);
    mode_ = QStringLiteral("external");
    url_ = url;
    port_ = url.port(8188);
    pid_ = 0;
  }

  setState(ComfyServerState::Starting);
  startExternalMonitoring();
}

void ComfyProcess::startExternalMonitoring() {
  if (!pingTimer_) {
    pingTimer_ = new QTimer(this);
    connect(pingTimer_, &QTimer::timeout, this, &ComfyProcess::onExternalPingTimer);
  }
  consecutiveMisses_ = 0;
  pingTimer_->start(pingIntervalMs_);
  onExternalPingTimer(); // Run initial check immediately
}

void ComfyProcess::stopExternalMonitoring() {
  if (pingTimer_) {
    pingTimer_->stop();
  }
}

void ComfyProcess::onExternalPingTimer() {
  QUrl u;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (mode_ != QLatin1String("external")) return;
    u = url_;
  }

  Client c(u);
  QString err;
  auto stats = c.systemStats(&err, pingTimeoutMs_);
  if (stats.has_value()) {
    consecutiveMisses_ = 0;
    if (state() != ComfyServerState::Ready) {
      setState(ComfyServerState::Ready);
    }
  } else {
    consecutiveMisses_++;
    if (consecutiveMisses_ >= 3 && state() != ComfyServerState::Unreachable) {
      setState(ComfyServerState::Unreachable, QJsonObject{{QStringLiteral("reason"), err}});
    }
  }
}

void ComfyProcess::assignJobObject() {
#ifdef Q_OS_WIN
  if (jobHandle_) {
    CloseHandle(static_cast<HANDLE>(jobHandle_));
    jobHandle_ = nullptr;
  }
  HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
  jobHandle_ = hJob;
  if (hJob) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    BOOL setInfoOk = SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    if (!setInfoOk) {
      appendLogLine(QStringLiteral("Warning: SetInformationJobObject failed: %1").arg(GetLastError()));
    }
    if (process_) {
      HANDLE hProc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(process_->processId()));
      if (hProc) {
        BOOL assignOk = AssignProcessToJobObject(hJob, hProc);
        if (!assignOk) {
          appendLogLine(QStringLiteral("Warning: AssignProcessToJobObject failed: %1").arg(GetLastError()));
        }
        CloseHandle(hProc);
      }
    }
  }
#endif
}

ComfyProcess::StartResult ComfyProcess::start(const QString& home, const QString& python, const QStringList& args, bool save, int timeoutMs) {
  // If external mode is configured, reject start
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (mode_ == QLatin1String("external")) {
      return StartResult{
        false,
        QStringLiteral("invalid"),
        QStringLiteral("external ComfyUI configured at %1").arg(url_.toString()),
        statusObjectLocked()
      };
    }
  }

  // Check if already running
  if (state() == ComfyServerState::Ready || state() == ComfyServerState::Starting) {
    return StartResult{true, {}, {}, statusObject()};
  }

  stopExternalMonitoring();

  // Resolve configuration
  QString resolvedHome = home;
  if (resolvedHome.isEmpty()) {
    const auto env = QProcessEnvironment::systemEnvironment();
    if (env.contains(QStringLiteral("SF_COMFY_HOME"))) {
      resolvedHome = env.value(QStringLiteral("SF_COMFY_HOME"));
    } else {
      std::lock_guard<std::mutex> lk(mu_);
      resolvedHome = home_;
    }
  }

  QString resolvedPython = python;
  if (resolvedPython.isEmpty()) {
    const auto env = QProcessEnvironment::systemEnvironment();
    if (env.contains(QStringLiteral("SF_COMFY_PYTHON"))) {
      resolvedPython = env.value(QStringLiteral("SF_COMFY_PYTHON"));
    } else {
      std::lock_guard<std::mutex> lk(mu_);
      resolvedPython = python_;
    }
  }

  QStringList resolvedArgs = args;
  if (resolvedArgs.isEmpty()) {
    std::lock_guard<std::mutex> lk(mu_);
    resolvedArgs = args_;
  }

  QString progOverride;
  {
    std::lock_guard<std::mutex> lk(mu_);
    progOverride = programOverride_;
  }

  if (progOverride.isEmpty()) {
    if (resolvedPython.isEmpty()) {
      resolvedPython = autoDetectPython(resolvedHome);
    }
    if (resolvedPython.isEmpty() || resolvedHome.isEmpty() || !QFileInfo::exists(resolvedPython)) {
      return StartResult{
        false,
        QStringLiteral("not_configured"),
        QStringLiteral("ComfyUI python or home directory not configured"),
        statusObject()
      };
    }
  }

  // Persist if requested
  if (save) {
    QSettings s;
    s.beginGroup(QStringLiteral("comfy"));
    if (!resolvedHome.isEmpty()) s.setValue(QStringLiteral("home"), resolvedHome);
    if (!resolvedPython.isEmpty()) s.setValue(QStringLiteral("python"), resolvedPython);
    if (!resolvedArgs.isEmpty()) s.setValue(QStringLiteral("args"), resolvedArgs);
    s.endGroup();
  }

  // Pick a free port
  QTcpServer tcpServer;
  if (!tcpServer.listen(QHostAddress::LocalHost, 0)) {
    return StartResult{
      false,
      QStringLiteral("start_failed"),
      QStringLiteral("Failed to allocate local port for ComfyUI"),
      statusObject()
    };
  }
  const quint16 chosenPort = tcpServer.serverPort();
  tcpServer.close();

  {
    std::lock_guard<std::mutex> lk(mu_);
    home_ = resolvedHome;
    python_ = resolvedPython;
    args_ = resolvedArgs;
    port_ = chosenPort;
    url_ = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(chosenPort));
    mode_ = QStringLiteral("managed");
  }

  // Launch process
  if (process_) {
    process_->disconnect(this);
    process_->kill();
    process_->waitForFinished(1000);
#ifdef Q_OS_WIN
    if (jobHandle_) {
      TerminateJobObject(static_cast<HANDLE>(jobHandle_), 1);
      CloseHandle(static_cast<HANDLE>(jobHandle_));
      jobHandle_ = nullptr;
    }
#endif
    process_.reset();
  }

  exitHandled_ = false;
  startTimeoutKill_ = false;

  process_ = std::make_unique<QProcess>(this);
  process_->setProcessChannelMode(QProcess::SeparateChannels);

  connect(process_.get(), &QProcess::readyReadStandardOutput, this, &ComfyProcess::onProcessReadyReadStdOut);
  connect(process_.get(), &QProcess::readyReadStandardError, this, &ComfyProcess::onProcessReadyReadStdErr);
  connect(process_.get(), QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
          this, &ComfyProcess::onProcessFinished);
  connect(process_.get(), &QProcess::errorOccurred, this, &ComfyProcess::onProcessErrorOccurred);

#if defined(Q_OS_LINUX)
  process_->setChildProcessModifier([]() {
    prctl(PR_SET_PDEATHSIG, SIGTERM);
  });
#endif

  QString prog;
  QStringList cmdArgs;
  if (!progOverride.isEmpty()) {
    prog = progOverride;
    cmdArgs << QStringLiteral("--listen") << QStringLiteral("127.0.0.1")
            << QStringLiteral("--port") << QString::number(chosenPort)
            << QStringLiteral("--disable-auto-launch");
    cmdArgs.append(resolvedArgs);
    process_->setWorkingDirectory(resolvedHome.isEmpty() ? QDir::tempPath() : resolvedHome);
  } else {
    prog = resolvedPython;
    cmdArgs << QStringLiteral("-s") << QStringLiteral("main.py")
            << QStringLiteral("--listen") << QStringLiteral("127.0.0.1")
            << QStringLiteral("--port") << QString::number(chosenPort)
            << QStringLiteral("--disable-auto-launch");
    cmdArgs.append(resolvedArgs);
    process_->setWorkingDirectory(resolvedHome);
  }

  setState(ComfyServerState::Starting);
  process_->start(prog, cmdArgs);

  if (!process_->waitForStarted(3000)) {
    setState(ComfyServerState::Crashed, QJsonObject{
      {QStringLiteral("reason"), QStringLiteral("start_failed")}
    });
    return StartResult{
      false,
      QStringLiteral("start_failed"),
      QStringLiteral("Failed to start ComfyUI process: %1").arg(process_->errorString()),
      statusObject()
    };
  }

  {
    std::lock_guard<std::mutex> lk(mu_);
    pid_ = process_->processId();
  }

  assignJobObject();

  // Poll for ready state. NO processEvents: this can run from the engine thread while the
  // service object is being constructed/assigned — pumping the caller's event loop would
  // deliver engine events mid-bringup (the old re-entrancy that cloned the service).
  const int waitLimit = (timeoutMs > 0) ? timeoutMs : readyTimeoutMs_;
  const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + waitLimit;

  while (state() == ComfyServerState::Starting && QDateTime::currentMSecsSinceEpoch() < deadline) {
    QString pErr;
    Client testClient(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(chosenPort)));
    auto stats = testClient.systemStats(&pErr, 300);
    if (stats.has_value()) {
      setState(ComfyServerState::Ready);
      break;
    }
    if (state() != ComfyServerState::Starting) break; // stop()/crash raced the poll
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }

  if (state() == ComfyServerState::Starting) {
    // Timed out waiting for ready
    startTimeoutKill_ = true;
    if (restartTimer_) {
      restartTimer_->stop();
    }
    process_->kill();
    process_->waitForFinished(1000);
#ifdef Q_OS_WIN
    if (jobHandle_) {
      TerminateJobObject(static_cast<HANDLE>(jobHandle_), 1);
      CloseHandle(static_cast<HANDLE>(jobHandle_));
      jobHandle_ = nullptr;
    }
#endif
    {
      std::lock_guard<std::mutex> lk(mu_);
      lastExit_ = QJsonObject{
        {QStringLiteral("code"), process_ ? process_->exitCode() : -1},
        {QStringLiteral("status"), process_ ? static_cast<int>(process_->exitStatus()) : 0},
        {QStringLiteral("reason"), QStringLiteral("start_timeout")},
        {QStringLiteral("logs"), QJsonArray::fromStringList(getRecentLogsLocked(50))}
      };
      pid_ = 0;
    }
    setState(ComfyServerState::Crashed, QJsonObject{
      {QStringLiteral("reason"), QStringLiteral("start_timeout")}
    });
    return StartResult{
      false,
      QStringLiteral("start_timeout"),
      QStringLiteral("Timed out waiting for ComfyUI to become ready after %1 ms").arg(waitLimit),
      statusObject()
    };
  }

  if (state() != ComfyServerState::Ready) {
    return StartResult{
      false,
      QStringLiteral("start_failed"),
      QStringLiteral("ComfyUI process failed during startup"),
      statusObject()
    };
  }

  return StartResult{true, {}, {}, statusObject()};
}

bool ComfyProcess::stop() {
  if (restartTimer_) {
    restartTimer_->stop();
  }
  stopExternalMonitoring();

  if (state() == ComfyServerState::Stopped || state() == ComfyServerState::None) {
    return true;
  }

  exitHandled_ = true;
  setState(ComfyServerState::Stopping);

  bool isManaged = false;
  QUrl currentUrl;
  {
    std::lock_guard<std::mutex> lk(mu_);
    isManaged = (mode_ == QLatin1String("managed"));
    currentUrl = url_;
  }

  // POST /interrupt ONLY in managed mode, to the process SplitFrame started. Never in external mode.
  if (isManaged && process_ && currentUrl.isValid()) {
    QString err;
    Client c(currentUrl);
    c.interrupt(&err);
  }

  if (process_ && process_->state() != QProcess::NotRunning) {
    process_->terminate();
    if (!process_->waitForFinished(5000)) {
      process_->kill();
      process_->waitForFinished(1000);
    }
  }

#ifdef Q_OS_WIN
  if (jobHandle_) {
    TerminateJobObject(static_cast<HANDLE>(jobHandle_), 1);
    CloseHandle(static_cast<HANDLE>(jobHandle_));
    jobHandle_ = nullptr;
  }
#endif

  {
    std::lock_guard<std::mutex> lk(mu_);
    pid_ = 0;
    port_ = 0;
  }

  setState(ComfyServerState::Stopped);
  return true;
}

ComfyProcess::StartResult ComfyProcess::restart() {
  stop();
  return start();
}

void ComfyProcess::onProcessReadyReadStdOut() {
  if (!process_) return;
  stdoutBuf_.append(process_->readAllStandardOutput());
  while (true) {
    const int idx = stdoutBuf_.indexOf('\n');
    if (idx < 0) break;
    QString line = QString::fromUtf8(stdoutBuf_.left(idx)).trimmed();
    stdoutBuf_.remove(0, idx + 1);
    appendLogLine(line);
  }
}

void ComfyProcess::onProcessReadyReadStdErr() {
  if (!process_) return;
  stderrBuf_.append(process_->readAllStandardError());
  while (true) {
    const int idx = stderrBuf_.indexOf('\n');
    if (idx < 0) break;
    QString line = QString::fromUtf8(stderrBuf_.left(idx)).trimmed();
    stderrBuf_.remove(0, idx + 1);
    appendLogLine(line);
  }
}

void ComfyProcess::appendLogLine(const QString& line) {
  if (line.isEmpty()) return;

  {
    std::lock_guard<std::mutex> lk(mu_);
    logRing_.push_back(line);
    while (logRing_.size() > 500) {
      logRing_.pop_front();
    }
  }

  // Append to log file
  QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  if (appData.isEmpty()) {
    appData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  }
  if (appData.isEmpty()) {
    appData = QDir::tempPath();
  }

  QDir logDir(QDir(appData).filePath(QStringLiteral("comfy")));
  logDir.mkpath(QStringLiteral("."));
  const QString logPath = logDir.filePath(QStringLiteral("comfy-%1.log").arg(QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"))));
  QFile logFile(logPath);
  if (logFile.open(QIODevice::Append | QIODevice::Text)) {
    logFile.write(line.toUtf8() + "\n");
    logFile.close();
  }
}

void ComfyProcess::handleExit(int exitCode, QProcess::ExitStatus exitStatus) {
  if (exitHandled_) return;
  if (startTimeoutKill_) { // our own start-timeout kill is not a crash: no bookkeeping, no restart
    exitHandled_ = true;
    return;
  }

  ComfyServerState curState;
  {
    std::lock_guard<std::mutex> lk(mu_);
    curState = state_;
  }

  if (curState == ComfyServerState::Stopping || curState == ComfyServerState::Stopped) {
    return;
  }

  exitHandled_ = true;

  // Flush remaining buffer
  if (!stdoutBuf_.isEmpty()) {
    appendLogLine(QString::fromUtf8(stdoutBuf_).trimmed());
    stdoutBuf_.clear();
  }
  if (!stderrBuf_.isEmpty()) {
    appendLogLine(QString::fromUtf8(stderrBuf_).trimmed());
    stderrBuf_.clear();
  }

  QJsonObject exitInfo;
  {
    std::lock_guard<std::mutex> lk(mu_);
    lastExit_ = QJsonObject{
      {QStringLiteral("code"), exitCode},
      {QStringLiteral("status"), static_cast<int>(exitStatus)},
      {QStringLiteral("logs"), QJsonArray::fromStringList(getRecentLogsLocked(50))}
    };
    exitInfo = lastExit_;
    pid_ = 0;
  }

  setState(ComfyServerState::Crashed, exitInfo);

  // Auto-restart handling
  bool shouldRestart = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (autoStart_) {
      const qint64 now = QDateTime::currentMSecsSinceEpoch();
      auto it = std::remove_if(restartTimestamps_.begin(), restartTimestamps_.end(), [now](qint64 t) {
        return now - t > 600000;
      });
      restartTimestamps_.erase(it, restartTimestamps_.end());

      if (restartTimestamps_.size() < 2) {
        restartTimestamps_.push_back(now);
        restarts_++;
        shouldRestart = true;
      }
    }
  }

  if (shouldRestart) {
    if (!restartTimer_) {
      restartTimer_ = new QTimer(this);
      restartTimer_->setSingleShot(true);
      connect(restartTimer_, &QTimer::timeout, this, [this]() {
        bool doStart = false;
        {
          std::lock_guard<std::mutex> lk(mu_);
          doStart = (state_ == ComfyServerState::Crashed && autoStart_);
        }
        if (doStart) {
          start();
        }
      });
    }
    restartTimer_->start(restartBackoffMs_);
  }
}

void ComfyProcess::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
  handleExit(exitCode, exitStatus);
}

void ComfyProcess::onProcessErrorOccurred(QProcess::ProcessError error) {
  Q_UNUSED(error);
  if (!process_) return;
  if (process_->state() == QProcess::NotRunning) {
    handleExit(process_->exitCode(), process_->exitStatus());
  }
}

} // namespace sf::comfy
