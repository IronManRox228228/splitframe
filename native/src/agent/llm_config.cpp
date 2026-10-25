#include "agent/llm_config.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QtGlobal>

namespace sf::agent {

LlmConfig LlmConfig::fromEnvironment() {
  LlmConfig c;
  QSettings settings(QStringLiteral("SplitFrame"), QStringLiteral("SplitFrame"));

  const QString endpoint = qEnvironmentVariable("SF_LLM_ENDPOINT").trimmed();
  if (!endpoint.isEmpty()) {
    c.endpoint = endpoint;
  } else {
    const QString sEndpoint = settings.value(QStringLiteral("SF_LLM_ENDPOINT")).toString().trimmed();
    if (!sEndpoint.isEmpty()) c.endpoint = sEndpoint;
  }

  const QString model = qEnvironmentVariable("SF_LLM_MODEL").trimmed();
  if (!model.isEmpty()) {
    c.model = model;
  } else {
    const QString sModel = settings.value(QStringLiteral("SF_LLM_MODEL")).toString().trimmed();
    if (!sModel.isEmpty()) c.model = sModel;
  }

  auto resolveLaneModel = [&](const char* envName, const QString& settingsKey1, const QString& settingsKey2) -> QString {
    const QString envVal = qEnvironmentVariable(envName).trimmed();
    if (!envVal.isEmpty()) return envVal;
    const QString sVal1 = settings.value(settingsKey1).toString().trimmed();
    if (!sVal1.isEmpty()) return sVal1;
    const QString sVal2 = settings.value(settingsKey2).toString().trimmed();
    if (!sVal2.isEmpty()) return sVal2;
    return c.model;
  };

  c.modelFast = resolveLaneModel("SF_LLM_MODEL_FAST", QStringLiteral("SF_LLM_MODEL_FAST"), QStringLiteral("ai/modelFast"));
  c.modelDeep = resolveLaneModel("SF_LLM_MODEL_DEEP", QStringLiteral("SF_LLM_MODEL_DEEP"), QStringLiteral("ai/modelDeep"));
  c.modelJudge = resolveLaneModel("SF_LLM_MODEL_JUDGE", QStringLiteral("SF_LLM_MODEL_JUDGE"), QStringLiteral("ai/modelJudge"));

  bool ok = false;
  const int ms = qEnvironmentVariable("SF_LLM_TIMEOUT_MS").toInt(&ok);
  if (ok && ms > 0) c.timeoutMs = ms;
  c.mockScript = qEnvironmentVariable("SF_LLM_MOCK_SCRIPT").trimmed();
  const QString apiKey = qEnvironmentVariable("SF_LLM_API_KEY").trimmed();
  if (!apiKey.isEmpty()) c.apiKey = apiKey;
  return c;
}

QJsonObject RecordingLlmClient::complete(const QString& systemPrompt, const std::vector<ChatMessage>& messages, const QJsonArray& tools, int slotId, Lane lane) {
  QJsonObject r = inner_ ? inner_->complete(systemPrompt, messages, tools, slotId, lane) : QJsonObject{{QStringLiteral("error"), QStringLiteral("no LLM client")}};
  const QJsonValue err = r.value(QStringLiteral("error"));
  if (!err.isUndefined() && !r.contains(QStringLiteral("content")) && !r.contains(QStringLiteral("tool_calls"))) {
    const QString code = llmErrorCode(r);
    if (!code.isEmpty()) {
      lastError_ = code;
    } else {
      lastError_ = err.isString() ? err.toString() : QString::fromUtf8(QJsonDocument(err.toObject()).toJson(QJsonDocument::Compact));
      if (lastError_.isEmpty()) lastError_ = QStringLiteral("the model returned an error");
    }
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
  // Seam: the provider choice made in AiOptions.fromEnvironment -> the OpenAI-compatible local client
  // or the offline mock.
  if (config.isMock()) {
    auto mock = std::make_shared<MockLlmClient>();
    if (!config.mockScript.isEmpty()) loadMockScript(*mock, config.mockScript, error);
    return mock;
  }
  auto http = std::make_shared<HttpLlmClient>(config.endpoint, config.model);
  http->setApiKey(config.apiKey);
  http->setTimeoutMs(config.timeoutMs);
  http->setLaneModels(config.modelFast, config.modelDeep, config.modelJudge);
  return http;
}

bool probeLlmEndpoint(const LlmConfig& config, QString* error) {
  if (config.isMock()) return true;
  return probeHttpEndpoint(config.endpoint, config.probeMs, error);
}

} // namespace sf::agent
