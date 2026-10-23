#include "agent/llm_config.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtGlobal>

namespace sf::agent {

LlmConfig LlmConfig::fromEnvironment() {
  LlmConfig c;
  const QString endpoint = qEnvironmentVariable("SF_LLM_ENDPOINT").trimmed();
  if (!endpoint.isEmpty()) c.endpoint = endpoint;
  const QString model = qEnvironmentVariable("SF_LLM_MODEL").trimmed();
  if (!model.isEmpty()) c.model = model;
  bool ok = false;
  const int ms = qEnvironmentVariable("SF_LLM_TIMEOUT_MS").toInt(&ok);
  if (ok && ms > 0) c.timeoutMs = ms;
  c.mockScript = qEnvironmentVariable("SF_LLM_MOCK_SCRIPT").trimmed();
  const QString apiKey = qEnvironmentVariable("SF_LLM_API_KEY").trimmed();
  if (!apiKey.isEmpty()) c.apiKey = apiKey;
  return c;
}

QJsonObject RecordingLlmClient::complete(const QString& systemPrompt, const std::vector<ChatMessage>& messages, const QJsonArray& tools, int slotId) {
  QJsonObject r = inner_ ? inner_->complete(systemPrompt, messages, tools, slotId) : QJsonObject{{QStringLiteral("error"), QStringLiteral("no LLM client")}};
  const QJsonValue err = r.value(QStringLiteral("error"));
  if (!err.isUndefined() && !r.contains(QStringLiteral("content")) && !r.contains(QStringLiteral("tool_calls"))) {
    lastError_ = err.isString() ? err.toString() : QString::fromUtf8(QJsonDocument(err.toObject()).toJson(QJsonDocument::Compact));
    if (lastError_.isEmpty()) lastError_ = QStringLiteral("the model returned an error");
  }
  return r;
}

bool RecordingLlmClient::reachable(QString* error) const {
  if (!inner_) {
    if (error) *error = QStringLiteral("no LLM client");
    return false;
  }
  return inner_->reachable(error);
}

bool loadMockScript(MockLlmClient& mock, const QString& path, QString* error) {
  auto fail = [&](const QString& m) {
    if (error) *error = m;
    return false;
  };
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return fail(QStringLiteral("can't read mock script %1").arg(path));
  QJsonParseError pe;
  const QJsonDocument d = QJsonDocument::fromJson(f.readAll(), &pe);
  if (!d.isObject()) return fail(QStringLiteral("mock script %1 is not a JSON object: %2").arg(path, pe.errorString()));
  const QJsonObject o = d.object();
  if (!o.contains(QStringLiteral("default")) && !o.contains(QStringLiteral("scripted"))) {
    mock.setDefaultResponse(o);
    return true;
  }
  if (o.value(QStringLiteral("default")).isObject()) mock.setDefaultResponse(o.value(QStringLiteral("default")).toObject());
  for (const QJsonValue& v : o.value(QStringLiteral("scripted")).toArray()) {
    const QJsonObject s = v.toObject();
    const QString trigger = s.value(QStringLiteral("trigger")).toString();
    if (!trigger.isEmpty()) mock.setScriptedResponse(trigger, s.value(QStringLiteral("response")).toObject());
  }
  return true;
}

std::shared_ptr<LlmClient> makeLlmClient(const LlmConfig& config, QString* error) {
  // Seam: the provider choice made in AiOptions.fromEnvironment → the OpenAI-compatible local client
  // or the offline mock.
  if (config.isMock()) {
    auto mock = std::make_shared<MockLlmClient>();
    if (!config.mockScript.isEmpty()) loadMockScript(*mock, config.mockScript, error);
    return mock;
  }
  auto http = std::make_shared<HttpLlmClient>(config.endpoint, config.model);
  http->setApiKey(config.apiKey);
  http->setTimeoutMs(config.timeoutMs);
  return http;
}

bool probeLlmEndpoint(const LlmConfig& config, QString* error) {
  if (config.isMock()) return true;
  return probeHttpEndpoint(config.endpoint, config.probeMs, error);
}

} // namespace sf::agent
