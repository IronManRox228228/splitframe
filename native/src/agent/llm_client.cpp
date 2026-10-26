#include "agent/llm_client.h"

#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

namespace sf::agent {

// Strips <think>...</think> and markdown ```think...``` fences, plus separate reasoning/reasoning_content fields.
QJsonObject stripReasoning(const QJsonObject& message) {
  QJsonObject out = message;
  out.remove(QStringLiteral("reasoning"));
  out.remove(QStringLiteral("reasoning_content"));

  QString content = out.value(QStringLiteral("content")).toString();

  // Strip XML-style <think>...</think> tags (including unclosed <think>)
  static const QRegularExpression xmlFence(QStringLiteral("<think>[\\s\\S]*?</think>"), QRegularExpression::DotMatchesEverythingOption);
  content.remove(xmlFence);
  const qsizetype xmlOpen = content.indexOf(QStringLiteral("<think>"));
  if (xmlOpen >= 0) content.truncate(xmlOpen);

  // Strip markdown ```think...``` fences (including unclosed ```think)
  static const QRegularExpression mdFence(QStringLiteral("```think[\\s\\S]*?```"), QRegularExpression::DotMatchesEverythingOption);
  content.remove(mdFence);
  const qsizetype mdOpen = content.indexOf(QStringLiteral("```think"));
  if (mdOpen >= 0) content.truncate(mdOpen);

  content = content.trimmed();
  out.insert(QStringLiteral("content"), content);
  return out;
}

// Maps response error representation to one of:
// provider_unreachable, provider_auth, model_not_found, rate_limited, provider_error.
QString llmErrorCode(const QJsonObject& resp) {
  static const QStringList codes{
    QStringLiteral("provider_unreachable"),
    QStringLiteral("provider_auth"),
    QStringLiteral("model_not_found"),
    QStringLiteral("rate_limited"),
    QStringLiteral("provider_error")
  };
  const QJsonValue err = resp.value(QStringLiteral("error"));
  QString code;
  if (err.isObject()) {
    code = err.toObject().value(QStringLiteral("code")).toString();
  } else if (err.isString()) {
    code = err.toString();
  }
  if (code.isEmpty() && resp.contains(QStringLiteral("code"))) {
    code = resp.value(QStringLiteral("code")).toString();
  }
  return codes.contains(code) ? code : QString();
}

QString llmErrorMessage(const QJsonObject& resp) {
  const QJsonValue err = resp.value(QStringLiteral("error"));
  QString msg = err.isObject() ? err.toObject().value(QStringLiteral("message")).toString() : err.toString();
  msg = msg.simplified();
  static const QRegularExpression keyLike(QStringLiteral("(?i)(bearer\\s+|sk-|key[=:\\s]+|authorization:?\\s*)[A-Za-z0-9._~+/=-]{8,}"));
  msg.replace(keyLike, QStringLiteral("[hidden]"));
  if (msg.size() > 200) msg = msg.left(197) + QStringLiteral("...");
  return msg;
}

// ---- MockLlmClient ----

void MockLlmClient::setScriptedResponse(const QString& triggerText, QJsonObject response) {
  scripted_[triggerText] = std::move(response);
}

void MockLlmClient::setDefaultResponse(QJsonObject response) {
  defaultResponse_ = std::move(response);
}

void MockLlmClient::enqueueResponse(QJsonObject response) {
  queued_.push_back(std::move(response));
}

QJsonObject MockLlmClient::complete(const QString& systemPrompt,
                                    const std::vector<ChatMessage>& messages,
                                    const QJsonArray& tools,
                                    int slotId,
                                    Lane lane) {
  lastLaneCalled_ = lane;
  calls_.push_back(CallRecord{
    .systemPrompt = systemPrompt,
    .messages = messages,
    .tools = tools,
    .slotId = slotId,
    .lane = (laneOverride_ != Lane::Fast) ? laneOverride_ : lane
  });

  auto processResp = [this](QJsonObject r) -> QJsonObject {
    const QJsonValue err = r.value(QStringLiteral("error"));
    if (err.isObject() || err.isString()) {
      lastObservedModel_.clear();
    } else if (r.contains(QStringLiteral("model"))) {
      lastObservedModel_ = r.value(QStringLiteral("model")).toString();
    }
    return stripReasoning(r);
  };

  if (!queued_.empty()) {
    QJsonObject next = std::move(queued_.front());
    queued_.erase(queued_.begin());
    return processResp(next);
  }

  QString combined;
  for (const auto& m : messages) combined += m.content + QLatin1Char(' ');

  for (const auto& [trigger, resp] : scripted_) {
    if (combined.contains(trigger, Qt::CaseInsensitive) || systemPrompt.contains(trigger, Qt::CaseInsensitive)) {
      return processResp(resp);
    }
  }

  return processResp(defaultResponse_);
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

bool isLoopbackEndpoint(const QString& endpoint) {
  const QString host = QUrl(endpoint.trimmed()).host();
  if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0) return true;
  return QHostAddress(host).isLoopback();
}

bool probeHttpEndpoint(const QString& endpoint, int timeoutMs, QString* error) {
  const QString trimmed = endpoint.trimmed();
  if (trimmed.isEmpty() || trimmed.compare(QLatin1String("mock"), Qt::CaseInsensitive) == 0) return true;
  const QUrl url(trimmed);
  if (!url.isValid() || url.host().isEmpty()) {
    if (error) *error = QStringLiteral("invalid LLM endpoint \"%1\"").arg(endpoint);
    return false;
  }
  const quint16 port = static_cast<quint16>(url.port(url.scheme() == QLatin1String("https") ? 443 : 80));
  QTcpSocket socket;
  socket.connectToHost(url.host(), port);
  if (!socket.waitForConnected(timeoutMs)) {
    if (error) *error = QStringLiteral("LLM endpoint %1 is not reachable (%2)").arg(endpoint, socket.errorString());
    socket.abort();
    return false;
  }
  socket.disconnectFromHost();
  return true;
}

HttpLlmClient::HttpLlmClient(QString endpoint, QString model)
  : endpoint_(std::move(endpoint)), model_(std::move(model)) {}

bool HttpLlmClient::reachable(QString* error) const {
  return probeHttpEndpoint(endpoint_, 2000, error);
}

QJsonObject HttpLlmClient::complete(const QString& systemPrompt,
                                    const std::vector<ChatMessage>& messages,
                                    const QJsonArray& tools,
                                    int slotId,
                                    Lane lane) {
  lastLaneCalled_ = lane;
  const QString activeModel = modelForLane(lane);
  QJsonObject req;
  req.insert(QStringLiteral("model"), activeModel);

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

  // slot_id is a llama-server extension; hosted APIs (Gemini, OpenAI) reject unknown fields with HTTP 400
  if (slotId >= 0 && isLoopbackEndpoint(endpoint_)) {
    req.insert(QStringLiteral("slot_id"), slotId);
  }

  QNetworkAccessManager manager;
  QNetworkRequest netReq;
  netReq.setUrl(QUrl(endpoint_));
  netReq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  if (!apiKey_.isEmpty()) {
    netReq.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(apiKey_).toUtf8());
  }

  QByteArray postData = QJsonDocument(req).toJson(QJsonDocument::Compact);
  QNetworkReply* reply = manager.post(netReq, postData);

  QEventLoop loop;
  QTimer timer;
  timer.setSingleShot(true);
  QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

  QTimer cancelPoll; // Stop must not wait out a slow model: poll the harness's flag while the reply is pending
  QObject::connect(&cancelPoll, &QTimer::timeout, &loop, [this, &loop] {
    if (cancelRequested()) loop.quit();
  });
  cancelPoll.start(100);

  if (timeoutMs_ > 0) timer.start(timeoutMs_);
  loop.exec();
  cancelPoll.stop();

  const auto errorOut = [this](const QString& code, const QString& message) {
    lastObservedModel_.clear();
    return QJsonObject{
      {QStringLiteral("error"), QJsonObject{
        {QStringLiteral("code"), code},
        {QStringLiteral("message"), message}
      }},
      {QStringLiteral("code"), code}
    };
  };

  if (cancelRequested() && !reply->isFinished()) {
    timer.stop();
    reply->abort();
    reply->deleteLater();
    return errorOut(QStringLiteral("cancelled"), QStringLiteral("Stopped by the user"));
  }
  timer.stop();
  if (!reply->isFinished()) {
    // Timeout (only reachable when timeoutMs_ > 0 started the timer)
    reply->abort();
    reply->deleteLater();
    return errorOut(QStringLiteral("provider_unreachable"), QStringLiteral("Request timed out"));
  }

  const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  const QByteArray respData = reply->readAll();
  const QString transportError = reply->error() != QNetworkReply::NoError ? reply->errorString() : QString();
  reply->deleteLater();

  if (!transportError.isEmpty() && status == 0) {
    return errorOut(QStringLiteral("provider_unreachable"), transportError);
  }

  const QJsonDocument doc = QJsonDocument::fromJson(respData);
  // Gemini wraps its error object in a one-element array
  const QJsonObject body = doc.isObject() ? doc.object()
                         : (doc.isArray() && doc.array().first().isObject()) ? doc.array().first().toObject() : QJsonObject();

  // Check for error in response body or HTTP error status
  if (body.contains(QStringLiteral("error"))) {
    const QJsonValue berr = body.value(QStringLiteral("error"));
    QString bodyCode;
    QString bodyMsg;
    if (berr.isObject()) {
      const QJsonObject bo = berr.toObject();
      bodyCode = bo.value(QStringLiteral("code")).toString();
      if (bodyCode.isEmpty()) bodyCode = bo.value(QStringLiteral("type")).toString();
      if (bodyCode.isEmpty()) bodyCode = bo.value(QStringLiteral("status")).toString(); // Google: "INVALID_ARGUMENT", "NOT_FOUND"
      bodyMsg = bo.value(QStringLiteral("message")).toString();
    } else {
      bodyCode = berr.toString();
      bodyMsg = berr.toString();
    }
    QString code;
    static const QStringList kValidCodes{
      QStringLiteral("provider_unreachable"),
      QStringLiteral("provider_auth"),
      QStringLiteral("model_not_found"),
      QStringLiteral("rate_limited"),
      QStringLiteral("provider_error")
    };
    if (kValidCodes.contains(bodyCode)) {
      code = bodyCode;
    } else if (bodyCode == QStringLiteral("invalid_api_key") || bodyCode.contains(QStringLiteral("auth"), Qt::CaseInsensitive) || status == 401 || status == 403) {
      code = QStringLiteral("provider_auth");
    } else if (bodyCode == QStringLiteral("rate_limit_exceeded") || status == 429) {
      code = QStringLiteral("rate_limited");
    } else if (bodyCode == QStringLiteral("model_not_found") || status == 404) {
      code = QStringLiteral("model_not_found");
    } else if (status >= 500) {
      code = QStringLiteral("provider_error");
    } else if (status == 0) { // an HTTP error status also sets transportError, but the server did answer
      code = QStringLiteral("provider_unreachable");
    } else {
      code = QStringLiteral("provider_error");
    }
    return errorOut(code, bodyMsg.isEmpty() ? code : bodyMsg);
  }

  if (status == 401 || status == 403) {
    return errorOut(QStringLiteral("provider_auth"), QStringLiteral("Authentication failed (HTTP %1)").arg(status));
  }
  if (status == 429) {
    return errorOut(QStringLiteral("rate_limited"), QStringLiteral("Rate limited (HTTP %1)").arg(status));
  }
  if (status == 404) {
    return errorOut(QStringLiteral("model_not_found"), QStringLiteral("Model not found (HTTP 404)"));
  }
  if (status >= 500) {
    return errorOut(QStringLiteral("provider_error"), QStringLiteral("Provider server error (HTTP %1)").arg(status));
  }
  if (!transportError.isEmpty() || status < 200 || status >= 300) {
    return errorOut(QStringLiteral("provider_unreachable"), !transportError.isEmpty() ? transportError : QStringLiteral("HTTP %1").arg(status));
  }

  if (!doc.isObject()) {
    return errorOut(QStringLiteral("provider_error"), QStringLiteral("Invalid JSON from LLM"));
  }

  // Parse OpenAI format: choices[0].message
  QJsonObject docObj = doc.object();
  if (docObj.contains(QStringLiteral("model"))) {
    lastObservedModel_ = docObj.value(QStringLiteral("model")).toString();
  } else {
    lastObservedModel_ = activeModel;
  }

  QJsonArray choices = docObj.value(QStringLiteral("choices")).toArray();
  if (choices.isEmpty()) {
    return errorOut(QStringLiteral("provider_error"), QStringLiteral("No choices returned from LLM"));
  }

  QJsonObject message = choices.first().toObject().value(QStringLiteral("message")).toObject();
  if (message.contains(QStringLiteral("tool_calls"))) {
    message.insert(QStringLiteral("tool_calls"), wireToolCalls(message.value(QStringLiteral("tool_calls")).toArray(), engineName));
  }
  if (!lastObservedModel_.isEmpty()) {
    message.insert(QStringLiteral("model"), lastObservedModel_);
  }
  return stripReasoning(message);
}

} // namespace sf::agent
