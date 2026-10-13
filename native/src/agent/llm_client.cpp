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

namespace {

// OpenAI-compatible servers want tools as {type:"function", function:{name, description, parameters}} and
// names matching ^[a-zA-Z0-9_-]+$. Engine commands are dotted ("item.color.set"), so dots travel as "__"
// and are mapped back on the reply.
QString wireName(const QString& name) { return QString(name).replace(QLatin1Char('.'), QStringLiteral("__")); }
QString engineName(const QString& name) { return QString(name).replace(QStringLiteral("__"), QStringLiteral(".")); }

QJsonArray wireToolCalls(const QJsonArray& calls, QString (*rename)(const QString&)) {
  QJsonArray out;
  for (const QJsonValue& v : calls) {
    QJsonObject tc = v.toObject();
    QJsonObject fn = tc.value(QStringLiteral("function")).toObject();
    fn.insert(QStringLiteral("name"), rename(fn.value(QStringLiteral("name")).toString()));
    tc.insert(QStringLiteral("function"), fn);
    if (!tc.contains(QStringLiteral("type"))) tc.insert(QStringLiteral("type"), QStringLiteral("function"));
    out.append(tc);
  }
  return out;
}

} // namespace

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
    QJsonObject mj = m.toJson();
    if (mj.contains(QStringLiteral("tool_calls")))
      mj.insert(QStringLiteral("tool_calls"), wireToolCalls(mj.value(QStringLiteral("tool_calls")).toArray(), wireName));
    if (mj.contains(QStringLiteral("name"))) mj.insert(QStringLiteral("name"), wireName(mj.value(QStringLiteral("name")).toString()));
    msgArr.append(mj);
  }
  req.insert(QStringLiteral("messages"), msgArr);

  if (!tools.isEmpty()) {
    QJsonArray wireTools;
    for (const QJsonValue& v : tools) {
      const QJsonObject t = v.toObject();
      if (t.value(QStringLiteral("type")).toString() == QStringLiteral("function")) { wireTools.append(t); continue; }
      wireTools.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")},
                                   {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), wireName(t.value(QStringLiteral("name")).toString())},
                                                                            {QStringLiteral("description"), t.value(QStringLiteral("description"))},
                                                                            {QStringLiteral("parameters"), t.value(QStringLiteral("parameters"))}}}});
    }
    req.insert(QStringLiteral("tools"), wireTools);
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
    if (const QByteArray body = reply->readAll(); !body.isEmpty()) errStr += QStringLiteral(": ") + QString::fromUtf8(body.left(400));
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
    QJsonObject message = choices.first().toObject().value(QStringLiteral("message")).toObject();
    if (message.contains(QStringLiteral("tool_calls")))
      message.insert(QStringLiteral("tool_calls"), wireToolCalls(message.value(QStringLiteral("tool_calls")).toArray(), engineName));
    return message;
  }

  return docObj;
}

} // namespace sf::agent
