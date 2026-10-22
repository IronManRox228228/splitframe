#pragma once

// LLM client interface, Mock client for tests, and HTTP client for llama-server.

#include "agent/types.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace sf::agent {

// Meterless lanes (Part A item 4): Fast = planning/tool-calling turns, Deep = generation-prompt and
// storyboard writing, Judge = peer review. Each lane resolves to its own model on the provider.
enum class Lane { Fast, Deep, Judge };

inline QString laneName(Lane lane) {
  switch (lane) {
    case Lane::Deep: return QStringLiteral("deep");
    case Lane::Judge: return QStringLiteral("judge");
    case Lane::Fast: break;
  }
  return QStringLiteral("fast");
}

class LlmClient {
public:
  virtual ~LlmClient() = default;

  // Sends chat completion request with system prompt, messages, and optional tool specs.
  // slotId >= 0 passes slot_id to llama-server to preserve warm KV caches for each specialist.
  virtual QJsonObject complete(const QString& systemPrompt,
                               const std::vector<ChatMessage>& messages,
                               const QJsonArray& tools = {},
                               int slotId = -1) = 0;

  // The lane-aware path (Part A item 4). Default: lanes make no difference outside Meterless, so this
  // forwards to the plain completion. Meterless overrides it to pick the lane's configured model.
  virtual QJsonObject complete(const QString& systemPrompt,
                               const std::vector<ChatMessage>& messages,
                               const QJsonArray& tools,
                               int slotId,
                               Lane lane) {
    Q_UNUSED(lane);
    return complete(systemPrompt, messages, tools, slotId);
  }

  // Reachability of this client (TCP to its endpoint, or always true for mock/offline).
  virtual bool reachable(QString* error = nullptr) const {
    if (error) error->clear();
    return true;
  }
};

// Mock LLM client for deterministic unit testing.
class MockLlmClient : public LlmClient {
public:
  struct CallRecord {
    QString systemPrompt;
    std::vector<ChatMessage> messages;
    QJsonArray tools;
    int slotId = -1;
    Lane lane = Lane::Fast;
  };

  MockLlmClient() = default;
  ~MockLlmClient() override = default;

  void setScriptedResponse(const QString& triggerText, QJsonObject response);
  void setDefaultResponse(QJsonObject response);
  void enqueueResponse(QJsonObject response);

  QJsonObject complete(const QString& systemPrompt,
                       const std::vector<ChatMessage>& messages,
                       const QJsonArray& tools = {},
                       int slotId = -1) override;

  // Lane-aware path (Part A item 4): the mock records the lane so tests can assert lane selection.
  QJsonObject complete(const QString& systemPrompt,
                       const std::vector<ChatMessage>& messages,
                       const QJsonArray& tools,
                       int slotId,
                       Lane lane) override {
    laneOverride_ = lane;
    return complete(systemPrompt, messages, tools, slotId);
  }

  Lane lastLane() const { return laneOverride_; }

  const std::vector<CallRecord>& calls() const { return calls_; }
  void clearCalls() { calls_.clear(); }

private:
  Lane laneOverride_ = Lane::Fast;
  std::map<QString, QJsonObject> scripted_;
  std::vector<QJsonObject> queued_;
  QJsonObject defaultResponse_{
    {QStringLiteral("content"), QStringLiteral("Acknowledged.")}
  };
  std::vector<CallRecord> calls_;
};

// HTTP client connecting to llama-server over OpenAI-compatible endpoint.
class HttpLlmClient : public LlmClient {
public:
  explicit HttpLlmClient(QString endpoint = QStringLiteral("http://127.0.0.1:8080/v1/chat/completions"),
                         QString model = QStringLiteral("qwythos-9b-v2"));
  ~HttpLlmClient() override = default;

  QJsonObject complete(const QString& systemPrompt,
                       const std::vector<ChatMessage>& messages,
                       const QJsonArray& tools = {},
                       int slotId = -1) override;

  void setTimeoutMs(int ms) { timeoutMs_ = ms; }
  const QString& endpoint() const { return endpoint_; }
  // Optional Bearer auth for local servers that enforce a key (Part A item 5: the MODEL_API_KEY alias).
  void setApiKey(QString key) { apiKey_ = std::move(key); }
  const QString& apiKey() const { return apiKey_; }
  bool reachable(QString* error = nullptr) const override;

private:
  QString endpoint_;
  QString model_;
  QString apiKey_;
  int timeoutMs_ = 30000;
};

// TCP connect to host:port of an OpenAI-style URL. True for empty/"mock".
bool probeHttpEndpoint(const QString& endpoint, int timeoutMs, QString* error = nullptr);

} // namespace sf::agent
