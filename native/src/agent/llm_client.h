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

class LlmClient {
public:
  virtual ~LlmClient() = default;

  // Sends chat completion request with system prompt, messages, and optional tool specs.
  // slotId >= 0 passes slot_id to llama-server to preserve warm KV caches for each specialist.
  virtual QJsonObject complete(const QString& systemPrompt,
                               const std::vector<ChatMessage>& messages,
                               const QJsonArray& tools = {},
                               int slotId = -1) = 0;
};

// Mock LLM client for deterministic unit testing.
class MockLlmClient : public LlmClient {
public:
  struct CallRecord {
    QString systemPrompt;
    std::vector<ChatMessage> messages;
    QJsonArray tools;
    int slotId = -1;
  };

  MockLlmClient() = default;
  ~MockLlmClient() override = default;

  void setScriptedResponse(const QString& triggerText, QJsonObject response);
  void setDefaultResponse(QJsonObject response);

  QJsonObject complete(const QString& systemPrompt,
                       const std::vector<ChatMessage>& messages,
                       const QJsonArray& tools = {},
                       int slotId = -1) override;

  const std::vector<CallRecord>& calls() const { return calls_; }
  void clearCalls() { calls_.clear(); }

private:
  std::map<QString, QJsonObject> scripted_;
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

private:
  QString endpoint_;
  QString model_;
  int timeoutMs_ = 30000;
};

} // namespace sf::agent
