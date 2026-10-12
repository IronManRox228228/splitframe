#include "agent/meterless_client.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QTimer>
#include <QUrl>

namespace sf::agent {

MeterlessClient::MeterlessClient(QString apiKey, QString endpoint, QString model)
  : apiKey_(std::move(apiKey)), endpoint_(std::move(endpoint)), model_(std::move(model)) {
  const auto env = QProcessEnvironment::systemEnvironment();
  if (apiKey_.isEmpty() && env.contains(QStringLiteral("METERLESS_API_KEY"))) {
    apiKey_ = env.value(QStringLiteral("METERLESS_API_KEY"));
  }
  if (env.contains(QStringLiteral("METERLESS_ENDPOINT"))) {
    endpoint_ = env.value(QStringLiteral("METERLESS_ENDPOINT"));
  }
  if (env.contains(QStringLiteral("METERLESS_MODEL"))) {
    model_ = env.value(QStringLiteral("METERLESS_MODEL"));
  }
}

QJsonObject MeterlessClient::complete(const QString& systemPrompt,
                                      const std::vector<ChatMessage>& messages,
                                      const QJsonArray& tools,
                                      int slotId) {
  Q_UNUSED(slotId);
  QElapsedTimer timer;
  timer.start();

  telemetry_.totalCalls++;

  if (mockHandler_) {
    QJsonObject resp = mockHandler_(systemPrompt, messages, tools);
    telemetry_.lastLatencyMs = timer.elapsed();

    if (resp.contains(QStringLiteral("usage"))) {
      const QJsonObject usage = resp.value(QStringLiteral("usage")).toObject();
      const int pt = usage.value(QStringLiteral("prompt_tokens")).toInt();
      const int ct = usage.value(QStringLiteral("completion_tokens")).toInt();
      telemetry_.promptTokens += pt;
      telemetry_.completionTokens += ct;
      telemetry_.totalTokens += (pt + ct);
    } else {
      // Estimated tokens for mock
      int estPrompt = static_cast<int>(systemPrompt.length() / 4);
      for (const auto& m : messages) estPrompt += static_cast<int>(m.content.length() / 4);
      const QString content = resp.value(QStringLiteral("content")).toString();
      int estComp = static_cast<int>(content.length() / 4);
      if (estComp == 0) estComp = 16;
      telemetry_.promptTokens += estPrompt;
      telemetry_.completionTokens += estComp;
      telemetry_.totalTokens += (estPrompt + estComp);
    }
    return resp;
  }

  // Real HTTP execution to Meterless endpoint
  QJsonObject req;
  req.insert(QStringLiteral("model"), model_);

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

  QNetworkAccessManager manager;
  QNetworkRequest netReq;
  netReq.setUrl(QUrl(endpoint_));
  netReq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  netReq.setRawHeader("User-Agent", "SplitFrame-NLE/0.1.0");

  if (!apiKey_.isEmpty()) {
    netReq.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(apiKey_).toUtf8());
  }

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

  if (timeoutTimer.isActive()) {
    timeoutTimer.stop();
  } else {
    reply->abort();
    reply->deleteLater();
    return QJsonObject{{QStringLiteral("error"), QStringLiteral("Meterless request timed out")}};
  }

  if (reply->error() != QNetworkReply::NoError) {
    QString errStr = reply->errorString();
    reply->deleteLater();
    return QJsonObject{{QStringLiteral("error"), errStr}};
  }

  const QByteArray respData = reply->readAll();
  reply->deleteLater();

  const QJsonDocument doc = QJsonDocument::fromJson(respData);
  if (!doc.isObject()) {
    return QJsonObject{{QStringLiteral("error"), QStringLiteral("Invalid JSON from Meterless")}};
  }

  const QJsonObject rootObj = doc.object();
  if (rootObj.contains(QStringLiteral("usage"))) {
    const QJsonObject usage = rootObj.value(QStringLiteral("usage")).toObject();
    const int pt = usage.value(QStringLiteral("prompt_tokens")).toInt();
    const int ct = usage.value(QStringLiteral("completion_tokens")).toInt();
    telemetry_.promptTokens += pt;
    telemetry_.completionTokens += ct;
    telemetry_.totalTokens += (pt + ct);
  }

  const QJsonArray choices = rootObj.value(QStringLiteral("choices")).toArray();
  if (!choices.isEmpty()) {
    const QJsonObject choice0 = choices.first().toObject();
    return choice0.value(QStringLiteral("message")).toObject();
  }

  return rootObj;
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
