#pragma once

// The one place that decides which LLM the AI commands talk to: endpoint and model from the environment
// or CLI flags, plus the factory that builds the client. Part A (16-AGENT-crew.md): Meterless is the
// default provider unless SF_LLM_* is explicitly set, and the setup guide's names MODEL_BASE_URL,
// MODEL_API_KEY and NEMOTRON_MODEL alias onto the generic local provider. makeLlmClient() is the seam.

#include "agent/llm_client.h"
#include "agent/meterless_client.h"

#include <QString>

#include <functional>
#include <memory>

namespace sf::agent {

struct LlmConfig {
  static constexpr const char* kDefaultEndpoint = "http://127.0.0.1:8080/v1/chat/completions";
  static constexpr const char* kDefaultModel = "qwythos-9b-v2";
  static constexpr const char* kMockEndpoint = "mock";

  QString endpoint = QString::fromLatin1(kDefaultEndpoint);
  QString model = QString::fromLatin1(kDefaultModel);
  QString apiKey;         // Bearer for local providers that enforce a key (SF_LLM_API_KEY / MODEL_API_KEY)
  int timeoutMs = 60000;  // one completion request, bounded so nothing hangs
  int probeMs = 2000;     // reachability check before a turn starts
  QString mockScript;     // mock only: JSON file scripting the replies (see loadMockScript)
  bool meterless = false; // Part A item 5: Meterless is the default provider when configured

  bool isMock() const { return endpoint.trimmed().compare(QString::fromLatin1(kMockEndpoint), Qt::CaseInsensitive) == 0; }

  // Defaults overridden by SF_LLM_ENDPOINT / SF_LLM_MODEL / SF_LLM_TIMEOUT_MS / SF_LLM_MOCK_SCRIPT; the
  // setup guide's names MODEL_BASE_URL / MODEL_API_KEY / NEMOTRON_MODEL alias onto those same keys.
  static LlmConfig fromEnvironment();
};

// True when a Meterless runtime is configured (METERLESS_BASE_URL or a runtime token from the env or
// Nightshift's proxy-token file). Never inspects, logs or prints the token itself.
bool meterlessConfigured();

// Wraps another client and remembers whether the last completions failed (the HTTP client reports failures as {"error": ...}
// and the specialists carry on with an empty reply), so a turn can be turned into a clean `unavailable` error.
class RecordingLlmClient : public LlmClient {
public:
  explicit RecordingLlmClient(std::shared_ptr<LlmClient> inner) : inner_(std::move(inner)) {}
  QJsonObject complete(const QString& systemPrompt, const std::vector<ChatMessage>& messages, const QJsonArray& tools = {}, int slotId = -1) override;
  QJsonObject complete(const QString& systemPrompt, const std::vector<ChatMessage>& messages, const QJsonArray& tools, int slotId, Lane lane) override {
    laneOverride_ = lane;
    return inner_ ? inner_->complete(systemPrompt, messages, tools, slotId, lane)
                  : QJsonObject{{QStringLiteral("error"), QStringLiteral("no LLM client")}};
  }
  Lane lastLane() const { return laneOverride_; }
  bool reachable(QString* error = nullptr) const override;
  void clearError() { lastError_.clear(); }
  const QString& lastError() const { return lastError_; }
  const std::shared_ptr<LlmClient>& inner() const { return inner_; }

private:
  std::shared_ptr<LlmClient> inner_;
  QString lastError_;
  Lane laneOverride_ = Lane::Fast;
};

// Part A item 5: Meterless primary, one automatic retry on the local provider when a call fails with
// provider_unreachable or rate_limited. Never falls back on provider_auth or model_not_found — those
// surface as-is. The retried call is marked `service:"local"` / `fallback_from:"meterless"` plus a
// one-line notice, and both the notification sink and agent-trace.jsonl record the event.
class FallbackLlmClient : public LlmClient {
public:
  FallbackLlmClient(std::shared_ptr<LlmClient> primary,
                    std::shared_ptr<LlmClient> fallback,
                    QString primaryName = QStringLiteral("meterless"),
                    QString fallbackName = QStringLiteral("local"))
    : primary_(std::move(primary)), fallback_(std::move(fallback)),
      primaryName_(std::move(primaryName)), fallbackName_(std::move(fallbackName)) {}

  QJsonObject complete(const QString& systemPrompt,
                       const std::vector<ChatMessage>& messages,
                       const QJsonArray& tools = {},
                       int slotId = -1) override {
    return complete(systemPrompt, messages, tools, slotId, Lane::Fast);
  }

  QJsonObject complete(const QString& systemPrompt,
                       const std::vector<ChatMessage>& messages,
                       const QJsonArray& tools,
                       int slotId,
                       Lane lane) override;

  bool reachable(QString* error = nullptr) const override;

  int fallbackCount() const { return fallbackCount_; }
  void clearFallback() { fallbackCount_ = 0; }
  const QString& lastFallback() const { return lastFallback_; }
  // One-line notice for the agent panel; e.g. "Meterless unavailable — answered on the local provider."
  void setNoticeSink(std::function<void(const QString&)> sink) { noticeSink_ = std::move(sink); }

  const std::shared_ptr<LlmClient>& primary() const { return primary_; }
  const std::shared_ptr<LlmClient>& fallback() const { return fallback_; }

private:
  std::shared_ptr<LlmClient> primary_;
  std::shared_ptr<LlmClient> fallback_;
  QString primaryName_;
  QString fallbackName_;
  int fallbackCount_ = 0;
  QString lastFallback_;
  std::function<void(const QString&)> noticeSink_;
};

// Mock replies from a JSON file: either one reply object {"content": "...", "tool_calls": [...]} used for every request, or
// {"default": {reply}, "scripted": [{"trigger": "text", "response": {reply}}, ...]}. False (with `error`) on a bad file.
bool loadMockScript(MockLlmClient& mock, const QString& path, QString* error);

// The seam: builds the client for a configuration. When Meterless is the provider, *meterlessOut
// receives the live client (for agent.provider.status); a bad mock script falls back to the default
// mock reply and sets *error.
std::shared_ptr<LlmClient> makeLlmClient(const LlmConfig& config, QString* error = nullptr,
                                         std::shared_ptr<MeterlessClient>* meterlessOut = nullptr);

// Quick TCP connect to the endpoint's host:port (a few seconds at most); true for the mock. `error` says why not.
bool probeLlmEndpoint(const LlmConfig& config, QString* error);

} // namespace sf::agent
