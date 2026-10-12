#pragma once

// Meterless cloud inference client and VRAM-aware offloader.
// Enables SplitFrame to route LLM requests to Meterless during heavy 4K playback
// or NVENC exports to avoid GPU VRAM contention on local machines.

#include "agent/llm_client.h"

#include <QJsonObject>
#include <QString>

#include <functional>
#include <memory>

namespace sf::agent {

struct MeterlessTelemetry {
  int promptTokens = 0;
  int completionTokens = 0;
  int totalTokens = 0;
  int totalCalls = 0;
  qint64 lastLatencyMs = 0;
};

// Cloud inference client for Meterless AI platform
class MeterlessClient : public LlmClient {
public:
  explicit MeterlessClient(QString apiKey = {},
                           QString endpoint = QStringLiteral("https://api.meterless.ai/v1/chat/completions"),
                           QString model = QStringLiteral("meterless-v1"));
  ~MeterlessClient() override = default;

  QJsonObject complete(const QString& systemPrompt,
                       const std::vector<ChatMessage>& messages,
                       const QJsonArray& tools = {},
                       int slotId = -1) override;

  const QString& apiKey() const { return apiKey_; }
  void setApiKey(QString key) { apiKey_ = std::move(key); }

  const QString& endpoint() const { return endpoint_; }
  void setEndpoint(QString ep) { endpoint_ = std::move(ep); }

  const QString& model() const { return model_; }
  void setModel(QString m) { model_ = std::move(m); }

  void setTimeoutMs(int ms) { timeoutMs_ = ms; }
  int timeoutMs() const { return timeoutMs_; }

  const MeterlessTelemetry& telemetry() const { return telemetry_; }
  void resetTelemetry() { telemetry_ = {}; }

  // Offline mock responder for testing and deterministic environments
  using MockHandler = std::function<QJsonObject(const QString&, const std::vector<ChatMessage>&, const QJsonArray&)>;
  void setMockHandler(MockHandler handler) { mockHandler_ = std::move(handler); }
  bool hasMockHandler() const { return static_cast<bool>(mockHandler_); }

private:
  QString apiKey_;
  QString endpoint_;
  QString model_;
  int timeoutMs_ = 30000;
  MeterlessTelemetry telemetry_;
  MockHandler mockHandler_;
};

// Decorator routing between local inference and Meterless offload
// based on GPU VRAM contention (e.g. active 4K playback, NVENC rendering).
class VramAwareLlmClient : public LlmClient {
public:
  VramAwareLlmClient(std::shared_ptr<LlmClient> localClient,
                     std::shared_ptr<LlmClient> offloadClient);
  ~VramAwareLlmClient() override = default;

  QJsonObject complete(const QString& systemPrompt,
                       const std::vector<ChatMessage>& messages,
                       const QJsonArray& tools = {},
                       int slotId = -1) override;

  bool isVramStrained() const { return vramStrained_; }
  void setVramStrained(bool strained) { vramStrained_ = strained; }

  std::shared_ptr<LlmClient> localClient() const { return localClient_; }
  std::shared_ptr<LlmClient> offloadClient() const { return offloadClient_; }

  int localCallCount() const { return localCallCount_; }
  int offloadCallCount() const { return offloadCallCount_; }
  int fallbackCount() const { return fallbackCount_; }

private:
  std::shared_ptr<LlmClient> localClient_;
  std::shared_ptr<LlmClient> offloadClient_;
  bool vramStrained_ = false;
  int localCallCount_ = 0;
  int offloadCallCount_ = 0;
  int fallbackCount_ = 0;
};

} // namespace sf::agent
