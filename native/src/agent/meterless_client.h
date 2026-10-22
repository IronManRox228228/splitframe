#pragma once

// Meterless client and VRAM-aware offloader. Part A (16-AGENT-crew.md):
// - no invented defaults: base URL, runtime token and routing mode come from METERLESS_BASE_URL,
//   METERLESS_RUNTIME_TOKEN and METERLESS_MODEL (default `meterless/auto`, a routing mode not a model);
// - runtime contract: Authorization Bearer + `meterless-app: splitframe` + `meterless-scope: user`
//   (scope only for chat), chat=POST {base}/v1/chat/completions, discovery=GET {base}/v1/models — a
//   /healthz success is never treated as proof the model works;
// - token source: METERLESS_RUNTIME_TOKEN, else Nightshift's state file
//   %LOCALAPPDATA%\ai.meterless.runtime\proxy-token (trimmed, never logged or displayed);
// - Nightshift may substitute the requested model, so the response's `model` field is recorded, not
//   the requested id; reasoning (a separate reasoning/reasoning_content field or <!-- --> blocks
//   in content) is stripped before the answer is used; max_tokens leaves room for reasoning plus answer;
// - errors use the codes provider_unreachable / provider_auth / model_not_found / rate_limited.

#include "agent/llm_client.h"

#include <QJsonObject>
#include <QString>

#include <array>
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

// One JSONL line appended to agent-trace.jsonl (env SF_AGENT_TRACE, else the app data dir; "off"
// disables). Logs models and fallback events, never the runtime token.
void logMeterlessTrace(const QJsonObject& row);

// The runtime token: METERLESS_RUNTIME_TOKEN first, then Nightshift's proxy-token file. Trimmed;
// never logged or displayed.
QString meterlessRuntimeToken();

// Maps a Meterless reply into the four provider error codes (Part A item 6); empty when the reply
// carries no error.
QString meterlessErrorCode(const QJsonObject& resp);

// Drops a separate reasoning/reasoning_content field and <!-- --> blocks from the message; keeps
// the clean answer only.
QJsonObject stripReasoning(const QJsonObject& message);

// Cloud inference client for the Meterless runtime.
class MeterlessClient : public LlmClient {
public:
  // No invented defaults (Part A item 2): with METERLESS_BASE_URL unset the client is unconfigured
  // and calls fail with provider_unreachable instead of pretending an endpoint exists.
  MeterlessClient();
  MeterlessClient(QString baseUrl, QString runtimeToken, QString routingMode);
  ~MeterlessClient() override = default;

  // Plain completion = the Fast lane.
  QJsonObject complete(const QString& systemPrompt,
                       const std::vector<ChatMessage>& messages,
                       const QJsonArray& tools = {},
                       int slotId = -1) override;

  // Lane-aware path (Part A item 4): the lane picks the configured model (METERLESS_LANE_* falling
  // back to METERLESS_MODEL for all three).
  QJsonObject complete(const QString& systemPrompt,
                       const std::vector<ChatMessage>& messages,
                       const QJsonArray& tools,
                       int slotId,
                       Lane lane) override;

  // Discovery (Part A items 3, 9): GET {base}/v1/models. "ok" requires a successful discovery call;
  // a /healthz success is never proof a model is connected. {"ok":true,"models":[...]} or {"ok":false,"error":code}.
  QJsonObject discoverModels() const;

  // Reachability of this client: a live discovery call for Meterless (item 9), plain TCP for other
  // implementations. `error` says why not (a provider error code).
  bool reachable(QString* error = nullptr) const override;

  // agent.provider.status payload (Part A item 3): {provider, baseUrl, reachable, models[], lanes{fast,deep,judge}}.
  QJsonObject statusJson();

  const QString& baseUrl() const { return baseUrl_; }
  void setBaseUrl(QString base) { baseUrl_ = std::move(base); }
  // The runtime token has no getter on purpose: it is never read back for logging or display.
  void setRuntimeToken(QString token) { runtimeToken_ = std::move(token); }

  // The routing mode default is `meterless/auto` (a routing mode, not a model id).
  const QString& routingMode() const { return routingMode_; }
  void setRoutingMode(QString mode) { routingMode_ = std::move(mode); }

  QString modelForLane(Lane lane) const {
    const QString& m = laneModels_[static_cast<int>(lane)];
    return m.isEmpty() ? routingMode_ : m;
  }
  void setModelForLane(Lane lane, QString model) { laneModels_[static_cast<int>(lane)] = std::move(model); }

  // The `model` field of the last chat response (Nightshift may substitute the requested id).
  const QString& lastObservedModel() const { return lastObservedModel_; }

  // Enough room for reasoning plus the answer (Part A item 9).
  void setMaxTokens(int maxTokens) { maxTokens_ = maxTokens; }

  void setTimeoutMs(int ms) { timeoutMs_ = ms; }
  int timeoutMs() const { return timeoutMs_; }

  const MeterlessTelemetry& telemetry() const { return telemetry_; }
  void resetTelemetry() { telemetry_ = {}; }

  bool configured() const { return !baseUrl_.trimmed().isEmpty(); }

  // Offline mock responder for testing and deterministic environments
  using MockHandler = std::function<QJsonObject(const QString&, const std::vector<ChatMessage>&, const QJsonArray&)>;
  void setMockHandler(MockHandler handler) { mockHandler_ = std::move(handler); }
  bool hasMockHandler() const { return static_cast<bool>(mockHandler_); }

private:
  QString chatUrl() const;
  QString modelsUrl() const;
  // Shared HTTP chat path: raw request, runtime headers, coded errors, reasoning strip, observed model.
  QJsonObject chatRequest(const QString& model, const QString& systemPrompt,
                          const std::vector<ChatMessage>& messages, const QJsonArray& tools, Lane lane);

  QString baseUrl_;                     // METERLESS_BASE_URL; empty = unconfigured
  QString runtimeToken_;                // METERLESS_RUNTIME_TOKEN or the proxy-token file
  QString routingMode_ = QStringLiteral("meterless/auto");
  std::array<QString, 3> laneModels_;   // METERLESS_LANE_FAST / _DEEP / _JUDGE; empty = routingMode
  QString lastObservedModel_;
  int timeoutMs_ = 30000;
  int probeMs_ = 5000; // discovery / reachability budget, so a guard call never hangs
  int maxTokens_ = 8192;
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

  QJsonObject complete(const QString& systemPrompt,
                       const std::vector<ChatMessage>& messages,
                       const QJsonArray& tools,
                       int slotId,
                       Lane lane) override {
    // Lane resolution belongs to whichever client actually sends the request; forward both halves.
    Q_UNUSED(lane);
    return complete(systemPrompt, messages, tools, slotId);
  }

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
