#pragma once

// ComfyUI child process lifecycle manager, output logger, and liveness monitor.

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace sf::comfy {

enum class ComfyServerState {
  None,
  Starting,
  Ready,
  Stopping,
  Stopped,
  Crashed,
  Unreachable
};

inline QString stateToString(ComfyServerState s) {
  switch (s) {
    case ComfyServerState::None: return QStringLiteral("none");
    case ComfyServerState::Starting: return QStringLiteral("starting");
    case ComfyServerState::Ready: return QStringLiteral("ready");
    case ComfyServerState::Stopping: return QStringLiteral("stopping");
    case ComfyServerState::Stopped: return QStringLiteral("stopped");
    case ComfyServerState::Crashed: return QStringLiteral("crashed");
    case ComfyServerState::Unreachable: return QStringLiteral("unreachable");
  }
  return QStringLiteral("none");
}

class ComfyProcess : public QObject {
  Q_OBJECT

public:
  struct StartResult {
    bool ok = false;
    QString errorCode; // "not_configured", "start_failed", "start_timeout", "invalid"
    QString errorMessage;
    QJsonObject status;
  };

  explicit ComfyProcess(QObject* parent = nullptr);
  ~ComfyProcess() override;

  // Mode and state
  QString modeString() const;
  ComfyServerState state() const;
  QUrl url() const;
  int port() const;
  qint64 pid() const;
  double uptimeSec() const;
  int restarts() const;
  QString home() const;
  QString python() const;
  QJsonObject lastExit() const;
  QStringList recentLogs(int maxLines = 100) const;
  QJsonObject statusObject() const;

  // Configuration and test overrides
  static void setGlobalProgramOverride(const QString& prog);
  static QString globalProgramOverride();
  void setProgramOverride(const QString& prog);
  QString programOverride() const;
  void setReadyTimeoutMs(int ms);
  void setPingIntervalMs(int ms);
  void setPingTimeoutMs(int ms);
  void setRestartBackoffMs(int ms);
  void setAutoStart(bool enable);
  bool autoStart() const;

  // Actions
  StartResult start(const QString& home = {}, const QString& python = {}, const QStringList& args = {}, bool save = false, int timeoutMs = 180000);
  bool stop();
  StartResult restart();

  void configureExternal(const QUrl& url);

signals:
  void stateChanged(sf::comfy::ComfyServerState state, const QJsonObject& info);

private slots:
  void onProcessReadyReadStdOut();
  void onProcessReadyReadStdErr();
  void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
  void onProcessErrorOccurred(QProcess::ProcessError error);
  void onExternalPingTimer();

private:
  void setState(ComfyServerState newState, const QJsonObject& info = {});
  void appendLogLine(const QString& line);
  QStringList getRecentLogsLocked(int maxLines) const;
  QJsonObject statusObjectLocked() const;
  void handleExit(int exitCode, QProcess::ExitStatus exitStatus);
  void startExternalMonitoring();
  void stopExternalMonitoring();
  void assignJobObject();

  mutable std::mutex mu_;
  ComfyServerState state_ = ComfyServerState::None;
  QString mode_ = QStringLiteral("none");
  QUrl url_;
  int port_ = 0;
  qint64 pid_ = 0;
  qint64 readyTime_ = 0;
  int restarts_ = 0;
  QString home_;
  QString python_;
  QStringList args_;
  bool autoStart_ = false;
  QJsonObject lastExit_;
  std::deque<QString> logRing_;
  std::vector<qint64> restartTimestamps_;

  QString programOverride_;
  int readyTimeoutMs_ = 180000;
  int pingIntervalMs_ = 5000;
  int pingTimeoutMs_ = 3000;
  int restartBackoffMs_ = 3000;
  int consecutiveMisses_ = 0;

  std::unique_ptr<QProcess> process_;
  QByteArray stdoutBuf_;
  QByteArray stderrBuf_;
  class QTimer* pingTimer_ = nullptr;
  class QTimer* restartTimer_ = nullptr;
  bool exitHandled_ = false;
  bool startTimeoutKill_ = false;

#ifdef Q_OS_WIN
  void* jobHandle_ = nullptr;
#endif
};

} // namespace sf::comfy

Q_DECLARE_METATYPE(sf::comfy::ComfyServerState)
