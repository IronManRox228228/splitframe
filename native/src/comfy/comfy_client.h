#pragma once

// ComfyUI synchronous HTTP client: node schema retrieval and system stats query.

#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QUrl>

#include <optional>
#include <vector>

namespace sf::comfy {

struct QueueResult {
  QString promptId;
  QString error;
  QJsonObject nodeErrors;
};

struct OutputFile {
  QString nodeId;
  QString key;
  QString filename;
  QString subfolder;
  QString type;
};

struct HistoryResult {
  bool finished = false;
  bool success = false;
  QString error;
  std::vector<OutputFile> outputs;
};

class Client {
public:
  explicit Client(QUrl base = {});                       // empty -> SF_COMFY_URL env -> http://127.0.0.1:8188
  QUrl base() const;
  // Synchronous with a timeout, the same pattern as llm_client.cpp. On failure it returns nullopt and sets *error to e.g.
  // "ComfyUI not reachable at http://127.0.0.1:8188 (connection refused)".
  std::optional<QJsonObject> objectInfo(QString* error = nullptr, int timeoutMs = 15000);
  std::optional<QJsonObject> systemStats(QString* error = nullptr, int timeoutMs = 3000);

  QueueResult queuePrompt(const QJsonObject& apiGraph, const QString& clientId = {}, int timeoutMs = 15000);
  QSet<QString> queue(QString* netError = nullptr, int timeoutMs = 5000);
  HistoryResult history(const QString& promptId, QString* netError = nullptr, int timeoutMs = 10000);
  bool download(const OutputFile& f, const QString& destPath, QString* error = nullptr, int timeoutMs = 120000);
  std::optional<QString> uploadImage(const QString& localPath, QString* error = nullptr, int timeoutMs = 60000);
  bool interrupt(QString* error = nullptr, int timeoutMs = 2000);

private:
  QUrl base_;
};

} // namespace sf::comfy
