#pragma once

// The one place that decides which LLM the AI commands talk to: endpoint, model and API key from the
// environment (`SF_LLM_*`, OpenAI-compatible — llama-server, LM Studio, Ollama, any similar server),
// plus the factory that builds the client. makeLlmClient() is the seam. The agent crew (Part C) runs
// on this provider; lanes (agent/llm_client.h) pick a model when the provider offers several.

#include "agent/llm_client.h"

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
  QString modelFast;  // defaults to SF_LLM_MODEL
  QString modelDeep;  // defaults to SF_LLM_MODEL
  QString modelJudge; // defaults to SF_LLM_MODEL
  QString apiKey;         // Bearer for local providers that enforce a key (SF_LLM_API_KEY)
  int timeoutMs = 60000;  // one completion request, bounded so nothing hangs
  int probeMs = 2000;     // reachability check before a turn starts
  QString mockScript;     // mock only: JSON file scripting the replies (see loadMockScript)

  bool isMock() const { return endpoint.trimmed().compare(QString::fromLatin1(kMockEndpoint), Qt::CaseInsensitive) == 0; }

  // Defaults overridden by SF_LLM_ENDPOINT / SF_LLM_MODEL / SF_LLM_TIMEOUT_MS / SF_LLM_MOCK_SCRIPT.
  static LlmConfig fromEnvironment();
};

// Wraps another client and remembers whether the last completions failed (the HTTP client reports failures as {"error": ...}
// and the specialists carry on with an empty reply), so a turn can be turned into a clean `unavailable` error.
class RecordingLlmClient : public LlmClient {
public:
  explicit RecordingLlmClient(std::shared_ptr<LlmClient> inner) : inner_(std::move(inner)) {}
  QJsonObject complete(const QString& systemPrompt, const std::vector<ChatMessage>& messages, const QJsonArray& tools = {}, int slotId = -1, Lane lane = Lane::Fast) override;
  bool reachable(QString* error = nullptr) const override;
  QString lastObservedModel() const override { return inner_ ? inner_->lastObservedModel() : QString(); }
  Lane lastLane() const override { return inner_ ? inner_->lastLane() : Lane::Fast; }
  void clearError() { lastError_.clear(); }
  const QString& lastError() const { return lastError_; }
  const std::shared_ptr<LlmClient>& inner() const { return inner_; }

private:
  std::shared_ptr<LlmClient> inner_;
  QString lastError_;
};

// Mock replies from a JSON file: either one reply object {"content": "...", "tool_calls": [...]} used for every request, or
// {"default": {reply}, "scripted": [{"trigger": "text", "response": {reply}}, ...]}. False (with `error`) on a bad file.
bool loadMockScript(MockLlmClient& mock, const QString& path, QString* error);

// The seam: builds the client for a configuration. A bad mock script falls back to the default mock
// reply and sets *error.
std::shared_ptr<LlmClient> makeLlmClient(const LlmConfig& config, QString* error = nullptr);

// Quick TCP connect to the endpoint's host:port (a few seconds at most); true for the mock. `error` says why not.
bool probeLlmEndpoint(const LlmConfig& config, QString* error);

} // namespace sf::agent
