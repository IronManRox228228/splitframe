#include "agent/llm_config.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QUrl>
#include <QtGlobal>

namespace sf::agent {

namespace {

bool envHas(const char* name) {
  return !qEnvironmentVariable(name).trimmed().isEmpty();
}

} // namespace

bool meterlessConfigured() {
  if (envHas("METERLESS_BASE_URL")) return true;
  if (envHas("METERLESS_RUNTIME_TOKEN")) return true;
  // Nightshift's token file: mere existence counts; the value is never read here or logged.
  const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
  if (localAppData.isEmpty()) return false;
  return QFile::exists(localAppData + QStringLiteral("\\ai.meterless.runtime\\proxy-token"));
}

LlmConfig LlmConfig::fromEnvironment() {
  LlmConfig c;
  bool explicitLocal = false;
  const QString endpoint = qEnvironmentVariable("SF_LLM_ENDPOINT").trimmed();
  bool ok = false;
  if (!endpoint.isEmpty()) { c.endpoint = endpoint; explicitLocal = true; }
  const QString model = qEnvironmentVariable("SF_LLM_MODEL").trimmed();
  if (!model.isEmpty()) { c.model = model; explicitLocal = true; }
  const int ms = qEnvironmentVariable("SF_LLM_TIMEOUT_MS").toInt(&ok);
  if (ok && ms > 0) c.timeoutMs = ms;
  c.mockScript = qEnvironmentVariable("SF_LLM_MOCK_SCRIPT").trimmed();

  // Part A item 5: the setup guide's names alias onto the generic provider; the SF_LLM_* values win.
  const QString aliasEndpoint = qEnvironmentVariable("MODEL_BASE_URL").trimmed();
  if (c.endpoint == QString::fromLatin1(kDefaultEndpoint) && !aliasEndpoint.isEmpty()) {
    c.endpoint = aliasEndpoint;
    explicitLocal = true;
  }
  const QString aliasModel = qEnvironmentVariable("NEMOTRON_MODEL").trimmed();
  if (c.model == QString::fromLatin1(kDefaultModel) && !aliasModel.isEmpty()) c.model = aliasModel;
  const QString key = qEnvironmentVariable("SF_LLM_API_KEY").trimmed();
  const QString aliasKey = qEnvironmentVariable("MODEL_API_KEY").trimmed();
  if (!key.isEmpty()) c.apiKey = key;
  else if (!aliasKey.isEmpty()) c.apiKey = aliasKey;

  // Meterless is the default provider; an explicit SF_LLM_* selection keeps the local provider.
  c.meterless = !explicitLocal && meterlessConfigured();
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

std::shared_ptr<LlmClient> makeLlmClient(const LlmConfig& config, QString* error, std::shared_ptr<MeterlessClient>* meterlessOut) {
  // Seam: the provider choice made in AiOptions.fromEnvironment → Meterless with one automatic retry on
  // the local provider (never when the runtime rejects the token or the model is unknown), or the plain
  // OpenAI-compatible local client, or the offline mock.
  if (config.isMock()) {
    auto mock = std::make_shared<MockLlmClient>();
    if (!config.mockScript.isEmpty()) loadMockScript(*mock, config.mockScript, error);
    return mock;
  }
  if (config.meterless) {
    auto meterless = std::make_shared<MeterlessClient>(); // reads METERLESS_* itself
    if (meterlessOut) *meterlessOut = meterless;
    auto local = std::make_shared<HttpLlmClient>(config.endpoint, config.model);
    local->setApiKey(config.apiKey);
    local->setTimeoutMs(config.timeoutMs);
    return std::make_shared<FallbackLlmClient>(meterless, local);
  }
  auto http = std::make_shared<HttpLlmClient>(config.endpoint, config.model);
  http->setApiKey(config.apiKey);
  http->setTimeoutMs(config.timeoutMs);
  return http;
}

QJsonObject FallbackLlmClient::complete(const QString& systemPrompt,
                                        const std::vector<ChatMessage>& messages,
                                        const QJsonArray& tools,
                                        int slotId,
                                        Lane lane) {
  if (!primary_) return QJsonObject{{QStringLiteral("error"), QStringLiteral("no LLM client")}};
  QJsonObject resp = primary_->complete(systemPrompt, messages, tools, slotId, lane);
  const QString code = meterlessErrorCode(resp);
  // Never fall back on provider_auth or model_not_found (Part A item 5): the user must see those errors.
  if (code != QStringLiteral("provider_unreachable") && code != QStringLiteral("rate_limited")) return resp;
  if (!fallback_) return resp;

  QJsonObject retried = fallback_->complete(systemPrompt, messages, tools, slotId);
  if (retried.contains(QStringLiteral("error"))) return resp; // the local provider is down too: show the primary error
  const QString notice = QStringLiteral("Meterless %1 — answered on the local provider.").arg(code);
  retried.insert(QStringLiteral("service"), fallbackName_);
  retried.insert(QStringLiteral("fallback_from"), primaryName_);
  retried.insert(QStringLiteral("fallback_notice"), notice);
  fallbackCount_++;
  lastFallback_ = fallbackName_;
  if (noticeSink_) noticeSink_(notice);
  logMeterlessTrace(QJsonObject{
    {QStringLiteral("event"), QStringLiteral("fallback")},
    {QStringLiteral("service"), fallbackName_},
    {QStringLiteral("fallback_from"), primaryName_},
    {QStringLiteral("reason"), code},
  });
  return retried;
}

bool FallbackLlmClient::reachable(QString* error) const {
  QString why;
  if (error == nullptr) error = &why;
  if (primary_ && primary_->reachable(error)) return true;
  QString why2;
  if (fallback_ && fallback_->reachable(&why2)) {
    if (error) error->clear();
    return true;
  }
  if (error && error->isEmpty()) *error = QStringLiteral("no LLM client");
  return false;
}

bool probeLlmEndpoint(const LlmConfig& config, QString* error) {
  if (config.isMock()) return true;
  if (config.meterless) {
    // TCP to the Meterless base URL's host; the full discovery check lives on the client
    // (a /healthz-style TCP success alone is never treated as proof the model works).
    QString base = qEnvironmentVariable("METERLESS_BASE_URL").trimmed();
    if (base.isEmpty()) base = QStringLiteral("http://127.0.0.1:8080"); // unknown runtime; TCP probe gives a definite answer below
    const QUrl url(base);
    const int port = url.port(url.scheme() == QLatin1String("https") ? 443 : 80);
    return probeHttpEndpoint(QStringLiteral("%1://%2:%3").arg(url.scheme(), url.host()).arg(port), config.probeMs, error);
  }
  return probeHttpEndpoint(config.endpoint, config.probeMs, error);
}

} // namespace sf::agent
