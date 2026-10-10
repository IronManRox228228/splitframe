#include "agent/llm_client.h"

#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace sf::agent {

// ---- MockLlmClient ----

void MockLlmClient::setScriptedResponse(const QString& triggerText, QJsonObject response) {
  scripted_[triggerText] = std::move(response);
}

void MockLlmClient::setDefaultResponse(QJsonObject response) {
  defaultResponse_ = std::move(response);
}

QJsonObject MockLlmClient::complete(const QString& systemPrompt,
                                    const std::vector<ChatMessage>& messages,
                                    const QJsonArray& tools,
                                    int slotId) {
  calls_.push_back(CallRecord{
    .systemPrompt = systemPrompt,
    .messages = messages,
    .tools = tools,
    .slotId = slotId
  });

  QString combined;
  for (const auto& m : messages) combined += m.content + QLatin1Char(' ');

  for (const auto& [trigger, resp] : scripted_) {
    if (combined.contains(trigger, Qt::CaseInsensitive) || systemPrompt.contains(trigger, Qt::CaseInsensitive)) {
      return resp;
    }
  }

  return defaultResponse_;
}

// ---- HttpLlmClient ----

HttpLlmClient::HttpLlmClient(QString endpoint, QString model)
  : endpoint_(std::move(endpoint)), model_(std::move(model)) {}

QJsonObject HttpLlmClient::complete(const QString& systemPrompt,
                                    const std::vector<ChatMessage>& messages,
                                    const QJsonArray& tools,
                                    int slotId) {
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

  if (slotId >= 0) {
    req.insert(QStringLiteral("slot_id"), slotId);
  }

  QNetworkAccessManager manager;
  QNetworkRequest netReq;
  netReq.setUrl(QUrl(endpoint_));
  netReq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

  QByteArray postData = QJsonDocument(req).toJson(QJsonDocument::Compact);
  QNetworkReply* reply = manager.post(netReq, postData);

  QEventLoop loop;
  QTimer timer;
  timer.setSingleShot(true);
  QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

  timer.start(timeoutMs_);
  loop.exec();

  if (timer.isActive()) {
    timer.stop();
  } else {
    // Timeout
    reply->abort();
    reply->deleteLater();
    return QJsonObject{{QStringLiteral("error"), QStringLiteral("Request timed out")}};
  }

  if (reply->error() != QNetworkReply::NoError) {
    QString errStr = reply->errorString();
    reply->deleteLater();
    return QJsonObject{{QStringLiteral("error"), errStr}};
  }

  QByteArray respData = reply->readAll();
  reply->deleteLater();

  QJsonDocument doc = QJsonDocument::fromJson(respData);
  if (!doc.isObject()) {
    return QJsonObject{{QStringLiteral("error"), QStringLiteral("Invalid JSON from LLM")}};
  }

  // Parse OpenAI format: choices[0].message
  QJsonObject docObj = doc.object();
  QJsonArray choices = docObj.value(QStringLiteral("choices")).toArray();
  if (!choices.isEmpty()) {
    QJsonObject choice0 = choices.first().toObject();
    return choice0.value(QStringLiteral("message")).toObject();
  }

  return docObj;
}

} // namespace sf::agent
