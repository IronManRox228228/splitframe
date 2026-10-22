#include "agent/meterless_client.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

namespace sf::agent {

MeterlessClient::MeterlessClient() {
  const QString base = qEnvironmentVariable("METERLESS_BASE_URL").trimmed();
  if (!base.isEmpty()) baseUrl_ = base;
  runtimeToken_ = meterlessRuntimeToken();
  const QString mode = qEnvironmentVariable("METERLESS_MODEL").trimmed();
  if (!mode.isEmpty()) routingMode_ = mode;
  // Lane defaults (the lead's scoping note): Fast = meterless/text.fast, Deep = meterless/text.deep,
  // Judge = meterless/judge.auto. When the user set a single METERLESS_MODEL routing mode instead,
  // the lanes fall back to it (Part A item 4); explicit METERLESS_LANE_* keys always win.
  if (mode.isEmpty()) {
    laneModels_[0] = QStringLiteral("meterless/text.fast");
    laneModels_[1] = QStringLiteral("meterless/text.deep");
    laneModels_[2] = QStringLiteral("meterless/judge.auto");
  }
  const QStringList laneKeys{QStringLiteral("METERLESS_LANE_FAST"), QStringLiteral("METERLESS_LANE_DEEP"),
                             QStringLiteral("METERLESS_LANE_JUDGE")};
  for (int i = 0; i < 3; ++i) {
    const QString lane = qEnvironmentVariable(laneKeys.at(i).toLatin1().constData()).trimmed();
    if (!lane.isEmpty()) laneModels_[i] = lane;
  }
}

MeterlessClient::MeterlessClient(QString baseUrl, QString runtimeToken, QString routingMode)
  : baseUrl_(std::move(baseUrl)), runtimeToken_(std::move(runtimeToken)), routingMode_(std::move(routingMode)) {
  baseUrl_ = baseUrl_.trimmed();
  if (routingMode_.isEmpty()) routingMode_ = QStringLiteral("meterless/auto");
}

QString meterlessRuntimeToken() {
  const QString fromEnv = qEnvironmentVariable("METERLESS_RUNTIME_TOKEN").trimmed();
  if (!fromEnv.isEmpty()) return fromEnv;
  // Token source (Part A item 9): Nightshift's state file next to the runtime's app data.
  const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
  if (localAppData.isEmpty()) return {};
  QFile f(QDir::fromNativeSeparators(localAppData) + QStringLiteral("/ai.meterless.runtime/proxy-token"));
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
  return QString::fromUtf8(f.readAll()).trimmed();
}

namespace {

QString normalizeBaseUrl(QString base) {
  base = base.trimmed();
  while (base.endsWith(QLatin1Char('/'))) base.chop(1);
  return base;
}

// Cached app data dir, so trace rows share one path even if the desktop changes QStandardPaths mid-run.
QString tracePath() {
  const QString override = qEnvironmentVariable("SF_AGENT_TRACE").trimmed();
  if (!override.isEmpty()) {
    if (override.compare(QLatin1String("off"), Qt::CaseInsensitive) == 0) return {};
    return override;
  }
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  if (dir.isEmpty()) return {};
  return dir + QStringLiteral("/agent-trace.jsonl");
}

} // namespace

void logMeterlessTrace(const QJsonObject& row) {
  const QString path = tracePath();
  if (path.isEmpty()) return;
  QFile f(path);
  if (!f.open(QIODevice::Append | QIODevice::Text)) return;
  f.write(QJsonDocument(row).toJson(QJsonDocument::Compact));
  f.write("\n");
}

QJsonObject stripReasoning(const QJsonObject& message) {
  QJsonObject out = message;
  // A separate reasoning field never belongs in the answer.
  out.remove(QStringLiteral("reasoning"));
  out.remove(QStringLiteral("reasoning_content"));
  QString content = out.value(QStringLiteral("content")).toString();
  // ---- blocks: a model that reasons inside the content fences it off first.
  static const QRegularExpression fence(QStringLiteral("```think[\\s\\S]*?```"), QRegularExpression::DotMatchesEverythingOption);
  content.remove(fence);
  // Unclosed fence: the reasoning runs to the end of the content.
  const qsizetype open = content.indexOf(QStringLiteral("```think"));
  if (open >= 0) content.truncate(open);
  content = content.trimmed();
  out.insert(QStringLiteral("content"), content);
  return out;
}

QString meterlessErrorCode(const QJsonObject& resp) {
  static const QStringList codes{QStringLiteral("provider_unreachable"), QStringLiteral("provider_auth"),
                                 QStringLiteral("model_not_found"), QStringLiteral("rate_limited")};
  const QJsonValue err = resp.value(QStringLiteral("error"));
  const QString code = err.isObject() ? err.toObject().value(QStringLiteral("code")).toString() : err.toString();
  return codes.contains(code) ? code : QString();
}

QString MeterlessClient::chatUrl() const {
  const QString base = normalizeBaseUrl(baseUrl_);
  if (base.endsWith(QStringLiteral("/v1"), Qt::CaseInsensitive))
    return base + QStringLiteral("/chat/completions");
  return base + QStringLiteral("/v1/chat/completions");
}

QString MeterlessClient::modelsUrl() const {
  const QString base = normalizeBaseUrl(baseUrl_);
  if (base.endsWith(QStringLiteral("/v1"), Qt::CaseInsensitive))
    return base + QStringLiteral("/models");
  return base + QStringLiteral("/v1/models");
}

QJsonObject MeterlessClient::complete(const QString& systemPrompt,
                                      const std::vector<ChatMessage>& messages,
                                      const QJsonArray& tools,
                                      int slotId) {
  return complete(systemPrompt, messages, tools, slotId, Lane::Fast);
}

QJsonObject MeterlessClient::complete(const QString& systemPrompt,
                                      const std::vector<ChatMessage>& messages,
                                      const QJsonArray& tools,
                                      int slotId,
                                      Lane lane) {
  Q_UNUSED(slotId);
  // Lane model resolution (Part A item 4): the per-lane METERLESS_LANE_* key, falling back to the
  // single METERLESS_MODEL routing mode for all three.
  return chatRequest(modelForLane(lane), systemPrompt, messages, tools, lane);
}

QJsonObject MeterlessClient::chatRequest(const QString& model,
                                         const QString& systemPrompt,
                                         const std::vector<ChatMessage>& messages,
                                         const QJsonArray& tools,
                                         Lane lane) {
  QElapsedTimer timer;
  timer.start();

  telemetry_.totalCalls++;

  if (mockHandler_) {
    QJsonObject resp = mockHandler_(systemPrompt, messages, tools);
    const QJsonValue err = resp.value(QStringLiteral("error"));
    if (err.isObject() || err.isString()) lastObservedModel_.clear();
    else {
      // Observed model, not the requested id (item 9).
      lastObservedModel_ = resp.value(QStringLiteral("model")).toString();
      resp = stripReasoning(resp);
    }
    if (const QJsonValue usage = resp.value(QStringLiteral("usage")); usage.isObject()) {
      const QJsonObject u = usage.toObject();
      const int pt = u.value(QStringLiteral("prompt_tokens")).toInt();
      const int ct = u.value(QStringLiteral("completion_tokens")).toInt();
      telemetry_.promptTokens += pt;
      telemetry_.completionTokens += ct;
      telemetry_.totalTokens += (pt + ct);
    }
    telemetry_.lastLatencyMs = timer.elapsed();
    return resp;
  }

  if (!configured()) {
    return QJsonObject{{QStringLiteral("error"), QJsonObject{
      {QStringLiteral("code"), QStringLiteral("provider_unreachable")},
      {QStringLiteral("message"), QStringLiteral("Meterless is not configured: set METERLESS_BASE_URL")}
    }}};
  }

  QJsonObject req;
  req.insert(QStringLiteral("model"), model);

  QJsonArray msgArr;
  if (!systemPrompt.isEmpty()) {
    msgArr.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                              {QStringLiteral("content"), systemPrompt}});
  }
  for (const auto& m : messages) {
    msgArr.append(m.toJson());
  }
  req.insert(QStringLiteral("messages"), msgArr);

  if (!tools.isEmpty()) {
    req.insert(QStringLiteral("tools"), tools);
  }

  // Enough room for reasoning plus the answer (Part A item 9).
  req.insert(QStringLiteral("max_tokens"), maxTokens_);

  QNetworkAccessManager manager;
  QNetworkRequest netReq;
  netReq.setUrl(QUrl(chatUrl()));
  netReq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  netReq.setRawHeader("User-Agent", "SplitFrame-NLE/0.1.0");

  // Runtime contract (Part A item 9): Bearer + meterless-app always; meterless-scope for chat.
  if (!runtimeToken_.isEmpty()) {
    netReq.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(runtimeToken_).toUtf8());
  }
  netReq.setRawHeader("meterless-app", "splitframe");
  netReq.setRawHeader("meterless-scope", "user");

  const QByteArray postData = QJsonDocument(req).toJson(QJsonDocument::Compact);
  QNetworkReply* reply = manager.post(netReq, postData);

  QEventLoop loop;
  QTimer timeoutTimer;
  timeoutTimer.setSingleShot(true);
  QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

  timeoutTimer.start(timeoutMs_);
  loop.exec();

  telemetry_.lastLatencyMs = timer.elapsed();

  const auto errorOut = [](const QString& code, const QString& message) {
    return QJsonObject{{QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code},
                                                            {QStringLiteral("message"), message}}}};
  };

  if (timeoutTimer.isActive()) {
    timeoutTimer.stop();
  } else {
    reply->abort();
    reply->deleteLater();
    return errorOut(QStringLiteral("provider_unreachable"), QStringLiteral("Meterless request timed out"));
  }

  const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  const QByteArray respData = reply->readAll();
  const QString transportError = reply->error() != QNetworkReply::NoError ? reply->errorString() : QString();
  reply->deleteLater();

  if (!transportError.isEmpty() && status == 0) {
    return errorOut(QStringLiteral("provider_unreachable"), transportError);
  }

  const QJsonDocument doc = QJsonDocument::fromJson(respData);
  const QJsonObject body = doc.isObject() ? doc.object() : QJsonObject();

  // Clear codes (Part A item 6): honor the runtime's own error code when present, else map the HTTP
  // status to the provider codes.
  if (body.contains(QStringLiteral("error"))) {
    const QJsonObject berr = body.value(QStringLiteral("error")).toObject();
    const QString bodyCode = berr.value(QStringLiteral("code")).toString();
    QString code = meterlessErrorCode(QJsonObject{{QStringLiteral("error"), bodyCode}});
    if (code.isEmpty()) {
      if (bodyCode == QStringLiteral("model_not_found") || status == 404) code = QStringLiteral("model_not_found");
      else if (status == 401 || status == 403) code = QStringLiteral("provider_auth");
      else if (status == 429) code = QStringLiteral("rate_limited");
      else code = QStringLiteral("provider_unreachable");
    }
    return errorOut(code, body.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString());
  }
  if (status == 401 || status == 403) return errorOut(QStringLiteral("provider_auth"), QStringLiteral("the runtime rejected the token"));
  if (status == 429) return errorOut(QStringLiteral("rate_limited"), QStringLiteral("the runtime is rate limiting"));
  if (status == 404) return errorOut(QStringLiteral("model_not_found"), QStringLiteral("unknown model or endpoint"));
  if (!transportError.isEmpty() || status < 200 || status >= 300)
    return errorOut(QStringLiteral("provider_unreachable"), transportError.isEmpty() ? QStringLiteral("HTTP %1").arg(status) : transportError);

  if (body.contains(QStringLiteral("usage"))) {
    const QJsonObject usage = body.value(QStringLiteral("usage")).toObject();
    const int pt = usage.value(QStringLiteral("prompt_tokens")).toInt();
    const int ct = usage.value(QStringLiteral("completion_tokens")).toInt();
    telemetry_.promptTokens += pt;
    telemetry_.completionTokens += ct;
    telemetry_.totalTokens += (pt + ct);
  }

  const QJsonArray choices = body.value(QStringLiteral("choices")).toArray();
  if (choices.isEmpty()) {
    return errorOut(QStringLiteral("provider_unreachable"), QStringLiteral("Meterless sent no choices"));
  }
  const QJsonObject message = choices.first().toObject().value(QStringLiteral("message")).toObject();
  // Nightshift may substitute the model, so record the model field of the RESPONSE (item 9), and
  // log it so agent-trace.jsonl shows which model actually answered.
  lastObservedModel_ = body.value(QStringLiteral("model")).toString();
  logMeterlessTrace(QJsonObject{
    {QStringLiteral("event"), QStringLiteral("llm_response")},
    {QStringLiteral("provider"), QStringLiteral("meterless")},
    {QStringLiteral("lane"), laneName(lane)},
    {QStringLiteral("requested"), model},
    {QStringLiteral("model"), lastObservedModel_},
  });
  return stripReasoning(message);
}

QJsonObject MeterlessClient::discoverModels() const {
  if (!configured()) {
    return QJsonObject{{QStringLiteral("ok"), false},
                       {QStringLiteral("error"), QStringLiteral("provider_unreachable")}};
  }
  QNetworkAccessManager manager;
  QNetworkRequest netReq;
  netReq.setUrl(QUrl(modelsUrl()));
  netReq.setRawHeader("User-Agent", "SplitFrame-NLE/0.1.0");
  if (!runtimeToken_.isEmpty()) {
    netReq.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(runtimeToken_).toUtf8());
  }
  netReq.setRawHeader("meterless-app", "splitframe");

  QNetworkReply* reply = manager.get(netReq);
  QEventLoop loop;
  QTimer timeoutTimer;
  timeoutTimer.setSingleShot(true);
  QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  timeoutTimer.start(probeMs_);
  loop.exec();

  const auto fail = [](const QString& code) {
    return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), code}};
  };

  if (timeoutTimer.isActive()) {
    timeoutTimer.stop();
  } else {
    reply->abort();
    reply->deleteLater();
    return fail(QStringLiteral("provider_unreachable"));
  }

  const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  const QByteArray respData = reply->readAll();
  const QString transportError = reply->error() != QNetworkReply::NoError ? reply->errorString() : QString();
  reply->deleteLater();

  if (status == 401 || status == 403) return fail(QStringLiteral("provider_auth"));
  if (status == 429) return fail(QStringLiteral("rate_limited"));
  if (status == 404) return fail(QStringLiteral("model_not_found"));
  if (!transportError.isEmpty() || status == 0 || status < 200 || status >= 300)
    return fail(QStringLiteral("provider_unreachable"));

  const QJsonDocument doc = QJsonDocument::fromJson(respData);
  if (!doc.isObject()) return fail(QStringLiteral("provider_unreachable"));
  const QJsonArray data = doc.object().value(QStringLiteral("data")).toArray();
  QJsonArray models;
  for (const QJsonValue& v : data) models.append(v.toObject().value(QStringLiteral("id")).toString());
  return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("models"), models}};
}

bool MeterlessClient::reachable(QString* error) const {
  // Reachable requires a successful discovery call (Part A item 9); /healthz proves nothing.
  QString why;
  if (error == nullptr) error = &why;
  if (!configured()) {
    *error = QStringLiteral("provider_unreachable");
    return false;
  }
  QJsonObject d = discoverModels();
  if (d.value(QStringLiteral("ok")).toBool()) return true;
  *error = d.value(QStringLiteral("error")).toString();
  return false;
}

QJsonObject MeterlessClient::statusJson() {
  const QJsonObject discovery = configured() ? discoverModels()
                                             : QJsonObject{{QStringLiteral("ok"), false}};
  const QJsonArray models = discovery.value(QStringLiteral("models")).toArray();
  QJsonObject lanes;
  const QStringList laneKeys{QStringLiteral("fast"), QStringLiteral("deep"), QStringLiteral("judge")};
  for (int i = 0; i < 3; ++i) {
    lanes.insert(laneKeys.at(i), laneModels_[i].isEmpty() ? routingMode_ : laneModels_[i]);
  }
  QJsonObject out{{QStringLiteral("provider"), QStringLiteral("meterless")},
                  {QStringLiteral("baseUrl"), normalizeBaseUrl(baseUrl_)},
                  {QStringLiteral("reachable"), discovery.value(QStringLiteral("ok")).toBool()},
                  {QStringLiteral("models"), models},
                  {QStringLiteral("lanes"), lanes}};
  if (!discovery.value(QStringLiteral("error")).toString().isEmpty())
    out.insert(QStringLiteral("error"), discovery.value(QStringLiteral("error")));
  return out;
}

// ---- VramAwareLlmClient ----

VramAwareLlmClient::VramAwareLlmClient(std::shared_ptr<LlmClient> localClient,
                                       std::shared_ptr<LlmClient> offloadClient)
  : localClient_(std::move(localClient)), offloadClient_(std::move(offloadClient)) {}

QJsonObject VramAwareLlmClient::complete(const QString& systemPrompt,
                                         const std::vector<ChatMessage>& messages,
                                         const QJsonArray& tools,
                                         int slotId) {
  if (vramStrained_) {
    if (offloadClient_) {
      offloadCallCount_++;
      return offloadClient_->complete(systemPrompt, messages, tools, slotId);
    }
    if (localClient_) {
      localCallCount_++;
      return localClient_->complete(systemPrompt, messages, tools, slotId);
    }
    return QJsonObject{{QStringLiteral("error"), QStringLiteral("No LLM client configured")}};
  }

  // Normal VRAM condition: try local first
  if (localClient_) {
    localCallCount_++;
    QJsonObject resp = localClient_->complete(systemPrompt, messages, tools, slotId);
    if (resp.contains(QStringLiteral("error")) && offloadClient_) {
      // Local engine failure or OOM -> fall back to Meterless cloud
      fallbackCount_++;
      offloadCallCount_++;
      return offloadClient_->complete(systemPrompt, messages, tools, slotId);
    }
    return resp;
  }

  if (offloadClient_) {
    offloadCallCount_++;
    return offloadClient_->complete(systemPrompt, messages, tools, slotId);
  }

  return QJsonObject{{QStringLiteral("error"), QStringLiteral("No LLM client configured")}};
}

} // namespace sf::agent
