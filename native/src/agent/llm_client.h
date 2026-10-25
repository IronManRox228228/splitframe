#pragma once

// LLM client interface, Mock client for tests, and HTTP client for llama-server.

#include "agent/types.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>
#include <map>
#include <atomic>
#include <memory>
#include <vector>

namespace sf::agent {

// Strips <think>...</think> and markdown ```think...``` fences, plus separate reasoning/reasoning_content fields.
QJsonObject stripReasoning(const QJsonObject& message);

// Maps response error representation to one of:
// provider_unreachable, provider_auth, model_not_found, rate_limited, provider_error.
// Returns an empty QString if the response has no error code or is unmapped.
QString llmErrorCode(const QJsonObject& resp);

// LLM lanes are a product-level routing idea: Fast = planning/tool-calling turns, Deep =
// generation-prompt and storyboard writing, Judge = peer review. The agent mesh (Part C) picks
// a lane per call by purpose; with one OpenAI-compatible provider all lanes share its model.
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
                               int slotId = -1,
                               Lane lane = Lane::Fast) = 0;

  virtual Lane lastLane() const { return Lane::Fast; }

  // Stop: the harness's flag. A client that waits on a network reply stops waiting once it is set.
  void setCancelFlag(const std::atomic_bool* flag) { cancel_ = flag; }
  bool cancelRequested() const { return cancel_ && cancel_->load(); }

  // Reachability of this client (TCP to its endpoint, or always true for mock/offline).
  virtual bool reachable(QString* error = nullptr) const {
    if (error) error->clear();
    return true;
  }

  // Observed model id from the response (differs from requested when substituted by the provider).
  virtual QString lastObservedModel() const { return {}; }

 private:
  const std::atomic_bool* cancel_ = nullptr;
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
                       int slotId = -1,
                       Lane lane = Lane::Fast) override;

  // Records the lane the caller asked for so tests can assert lane selection (agent mesh).
  void setLane(Lane lane) { laneOverride_ = lane; }
  Lane lastLane() const override { return (laneOverride_ != Lane::Fast) ? laneOverride_ : lastLaneCalled_; }

  void setLastObservedModel(QString model) { lastObservedModel_ = std::move(model); }
  QString lastObservedModel() const override { return lastObservedModel_; }

  const std::vector<CallRecord>& calls() const { return calls_; }
  void clearCalls() { calls_.clear(); }

private:
  Lane laneOverride_ = Lane::Fast;
  Lane lastLaneCalled_ = Lane::Fast;
  std::map<QString, QJsonObject> scripted_;
  std::vector<QJsonObject> queued_;
  QJsonObject defaultResponse_{
    {QStringLiteral("content"), QStringLiteral("Acknowledged.")}
  };
  std::vector<CallRecord> calls_;
  QString lastObservedModel_;
};

// HTTP client connecting to llama-server over OpenAI-compatible endpoint.
class HttpLlmClient : public LlmClient {
public:
  explicit HttpLlmClient(QString endpoint = QStringLiteral("http://127.0.0.1:8080/v1/chat/completions"),
                         QString model = QStringLiteral("qwythos-9b-v2"));
  ~HttpLlmClient() override = default;

  void setLaneModels(QString fast, QString deep, QString judge) {
    modelFast_ = std::move(fast);
    modelDeep_ = std::move(deep);
    modelJudge_ = std::move(judge);
  }

  QString modelForLane(Lane lane) const {
    switch (lane) {
      case Lane::Fast: return !modelFast_.isEmpty() ? modelFast_ : model_;
      case Lane::Deep: return !modelDeep_.isEmpty() ? modelDeep_ : model_;
      case Lane::Judge: return !modelJudge_.isEmpty() ? modelJudge_ : model_;
    }
    return model_;
  }

  QJsonObject complete(const QString& systemPrompt,
                       const std::vector<ChatMessage>& messages,
                       const QJsonArray& tools = {},
                       int slotId = -1,
                       Lane lane = Lane::Fast) override;

  Lane lastLane() const override { return lastLaneCalled_; }

  void setTimeoutMs(int ms) { timeoutMs_ = ms; }
  const QString& endpoint() const { return endpoint_; }
  // Optional Bearer auth for local servers that enforce a key (SF_LLM_API_KEY).
  void setApiKey(QString key) { apiKey_ = std::move(key); }
  const QString& apiKey() const { return apiKey_; }
  bool reachable(QString* error = nullptr) const override;

  QString lastObservedModel() const override { return lastObservedModel_; }

private:
  QString endpoint_;
  QString model_;
  QString modelFast_;
  QString modelDeep_;
  QString modelJudge_;
  QString apiKey_;
  int timeoutMs_ = 30000;
  QString lastObservedModel_;
  Lane lastLaneCalled_ = Lane::Fast;
};

// TCP connect to host:port of an OpenAI-style URL. True for empty/"mock".
bool probeHttpEndpoint(const QString& endpoint, int timeoutMs, QString* error = nullptr);

} // namespace sf::agent
