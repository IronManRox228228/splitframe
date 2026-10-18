// ComfyUI synchronous HTTP client: node schema retrieval and system stats query.

#include "comfy/comfy_client.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>

namespace sf::comfy {

namespace {

QUrl makeEndpoint(const QUrl& base, const QString& path) {
  QString b = base.toString();
  if (!b.endsWith(QLatin1Char('/'))) b += QLatin1Char('/');
  return QUrl(b + path);
}

std::optional<QJsonObject> getJson(const QUrl& url, const QUrl& base, int timeoutMs, QString* error) {
  QNetworkAccessManager manager;
  QNetworkRequest netReq(url);
  QNetworkReply* reply = manager.get(netReq);

  QEventLoop loop;
  QTimer timer;
  timer.setSingleShot(true);
  QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

  timer.start(timeoutMs);
  loop.exec();

  if (timer.isActive()) {
    timer.stop();
  } else {
    reply->abort();
    reply->deleteLater();
    if (error) {
      *error = QStringLiteral("ComfyUI not reachable at %1 (request timed out after %2 ms)").arg(base.toString()).arg(timeoutMs);
    }
    return std::nullopt;
  }

  if (reply->error() != QNetworkReply::NoError) {
    const QString errStr = reply->errorString();
    if (error) {
      *error = QStringLiteral("ComfyUI not reachable at %1 (%2)").arg(base.toString(), errStr);
    }
    reply->deleteLater();
    return std::nullopt;
  }

  const QByteArray respData = reply->readAll();
  reply->deleteLater();

  QJsonParseError parseErr;
  const QJsonDocument doc = QJsonDocument::fromJson(respData, &parseErr);
  if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
    if (error) {
      *error = QStringLiteral("Invalid JSON from %1: %2").arg(url.toString(), parseErr.errorString());
    }
    return std::nullopt;
  }

  return doc.object();
}

} // namespace

Client::Client(QUrl base) {
  if (base.isEmpty()) {
    const QString envUrl = qEnvironmentVariable("SF_COMFY_URL");
    if (!envUrl.isEmpty()) {
      base_ = QUrl(envUrl);
    } else {
      base_ = QUrl(QStringLiteral("http://127.0.0.1:8188"));
    }
  } else {
    base_ = std::move(base);
  }
}

QUrl Client::base() const {
  return base_;
}

std::optional<QJsonObject> Client::objectInfo(QString* error, int timeoutMs) {
  return getJson(makeEndpoint(base_, QStringLiteral("object_info")), base_, timeoutMs, error);
}

std::optional<QJsonObject> Client::systemStats(QString* error, int timeoutMs) {
  return getJson(makeEndpoint(base_, QStringLiteral("system_stats")), base_, timeoutMs, error);
}

QueueResult Client::queuePrompt(const QJsonObject& apiGraph, const QString& clientId, int timeoutMs) {
  QJsonObject req;
  req.insert(QStringLiteral("prompt"), apiGraph);
  const QString cid = clientId.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : clientId;
  req.insert(QStringLiteral("client_id"), cid);

  QNetworkAccessManager manager;
  QNetworkRequest netReq(makeEndpoint(base_, QStringLiteral("prompt")));
  netReq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  QByteArray postData = QJsonDocument(req).toJson(QJsonDocument::Compact);

  QNetworkReply* reply = manager.post(netReq, postData);

  QEventLoop loop;
  QTimer timer;
  timer.setSingleShot(true);
  QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

  timer.start(timeoutMs);
  loop.exec();

  if (!timer.isActive()) {
    reply->abort();
    reply->deleteLater();
    return QueueResult{QString(), QStringLiteral("Request to /prompt timed out after %1 ms").arg(timeoutMs), {}};
  }
  timer.stop();

  const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  const QByteArray respData = reply->readAll();
  const QString replyErr = reply->errorString();
  reply->deleteLater();

  QJsonParseError parseErr;
  QJsonDocument doc = QJsonDocument::fromJson(respData, &parseErr);
  QJsonObject docObj = doc.isObject() ? doc.object() : QJsonObject{};

  if (status == 200) {
    return QueueResult{docObj.value(QStringLiteral("prompt_id")).toString(), QString(), docObj.value(QStringLiteral("node_errors")).toObject()};
  }

  QString errMsg;
  QJsonObject nodeErrors;
  if (docObj.contains(QStringLiteral("error")) && docObj.value(QStringLiteral("error")).isObject()) {
    errMsg = docObj.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
  }
  if (docObj.contains(QStringLiteral("node_errors")) && docObj.value(QStringLiteral("node_errors")).isObject()) {
    nodeErrors = docObj.value(QStringLiteral("node_errors")).toObject();
    for (auto it = nodeErrors.begin(); it != nodeErrors.end(); ++it) {
      const QString nodeId = it.key();
      const QJsonObject ne = it.value().toObject();
      const QString classType = ne.value(QStringLiteral("class_type")).toString();
      const QJsonArray errorsArr = ne.value(QStringLiteral("errors")).toArray();
      QString firstMsg;
      if (!errorsArr.isEmpty()) {
        firstMsg = errorsArr.at(0).toObject().value(QStringLiteral("message")).toString();
      }
      if (!errMsg.isEmpty()) errMsg += QStringLiteral("; ");
      errMsg += QStringLiteral("node %1 (%2): %3").arg(nodeId, classType, firstMsg);
    }
  }

  if (errMsg.isEmpty()) {
    errMsg = QStringLiteral("Prompt submission failed (%1): %2").arg(status).arg(replyErr);
  }

  return QueueResult{QString(), errMsg, nodeErrors};
}

QSet<QString> Client::queue(QString* netError, int timeoutMs) {
  QSet<QString> promptIds;
  auto objOpt = getJson(makeEndpoint(base_, QStringLiteral("queue")), base_, timeoutMs, netError);
  if (!objOpt) return promptIds;

  const QJsonObject& obj = *objOpt;
  for (const QString& k : {QStringLiteral("queue_running"), QStringLiteral("queue_pending")}) {
    const QJsonArray arr = obj.value(k).toArray();
    for (const QJsonValue& v : arr) {
      if (v.isArray()) {
        const QJsonArray item = v.toArray();
        if (item.size() >= 2 && item.at(1).isString()) {
          promptIds.insert(item.at(1).toString());
        }
      } else if (v.isString()) {
        promptIds.insert(v.toString());
      }
    }
  }
  return promptIds;
}

HistoryResult Client::history(const QString& promptId, QString* netError, int timeoutMs) {
  QNetworkAccessManager manager;
  QNetworkRequest netReq(makeEndpoint(base_, QStringLiteral("history/") + promptId));
  QNetworkReply* reply = manager.get(netReq);

  QEventLoop loop;
  QTimer timer;
  timer.setSingleShot(true);
  QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

  timer.start(timeoutMs);
  loop.exec();

  if (!timer.isActive()) {
    reply->abort();
    reply->deleteLater();
    if (netError) *netError = QStringLiteral("Request to /history timed out after %1 ms").arg(timeoutMs);
    return HistoryResult{false, false, {}, {}};
  }
  timer.stop();

  if (reply->error() != QNetworkReply::NoError) {
    if (netError) *netError = reply->errorString();
    reply->deleteLater();
    return HistoryResult{false, false, {}, {}};
  }

  const QByteArray respData = reply->readAll();
  reply->deleteLater();

  QJsonParseError parseErr;
  QJsonDocument doc = QJsonDocument::fromJson(respData, &parseErr);
  if (!doc.isObject() || !doc.object().contains(promptId)) {
    return HistoryResult{false, false, {}, {}};
  }

  QJsonObject pobj = doc.object().value(promptId).toObject();
  QJsonObject statusObj = pobj.value(QStringLiteral("status")).toObject();
  const QString statusStr = statusObj.value(QStringLiteral("status_str")).toString();

  HistoryResult res;
  res.finished = true;
  res.success = (statusStr == QStringLiteral("success"));

  if (!res.success) {
    QStringList msgs;
    QJsonArray msgArr = statusObj.value(QStringLiteral("messages")).toArray();
    for (const auto& m : msgArr) {
      if (m.isString()) msgs.append(m.toString());
      else msgs.append(QString::fromUtf8(QJsonDocument(m.toObject()).toJson(QJsonDocument::Compact)));
    }
    res.error = msgs.isEmpty() ? (statusStr.isEmpty() ? QStringLiteral("ComfyUI execution failed") : statusStr)
                               : msgs.join(QStringLiteral("; "));
  }

  QJsonObject outputsObj = pobj.value(QStringLiteral("outputs")).toObject();
  for (auto it = outputsObj.begin(); it != outputsObj.end(); ++it) {
    const QString nodeId = it.key();
    const QJsonObject nodeOutputs = it.value().toObject();
    for (auto oit = nodeOutputs.begin(); oit != nodeOutputs.end(); ++oit) {
      const QString key = oit.key();
      if (oit.value().isArray()) {
        const QJsonArray arr = oit.value().toArray();
        for (const auto& item : arr) {
          if (!item.isObject()) continue;
          const QJsonObject itemObj = item.toObject();
          if (itemObj.contains(QStringLiteral("filename")) && itemObj.value(QStringLiteral("filename")).isString()) {
            const QString type = itemObj.value(QStringLiteral("type")).toString();
            if (type == QStringLiteral("temp")) continue;
            OutputFile out;
            out.nodeId = nodeId;
            out.key = key;
            out.filename = itemObj.value(QStringLiteral("filename")).toString();
            out.subfolder = itemObj.value(QStringLiteral("subfolder")).toString();
            out.type = type;
            res.outputs.push_back(std::move(out));
          }
        }
      }
    }
  }

  return res;
}

bool Client::download(const OutputFile& f, const QString& destPath, QString* error, int timeoutMs) {
  QUrlQuery query;
  query.addQueryItem(QStringLiteral("filename"), f.filename);
  query.addQueryItem(QStringLiteral("subfolder"), f.subfolder);
  query.addQueryItem(QStringLiteral("type"), f.type);

  QUrl url = makeEndpoint(base_, QStringLiteral("view"));
  url.setQuery(query);

  QNetworkAccessManager manager;
  QNetworkRequest netReq(url);
  QNetworkReply* reply = manager.get(netReq);

  QEventLoop loop;
  QTimer timer;
  timer.setSingleShot(true);
  QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

  timer.start(timeoutMs);
  loop.exec();

  if (!timer.isActive()) {
    reply->abort();
    reply->deleteLater();
    if (error) *error = QStringLiteral("Download timed out after %1 ms").arg(timeoutMs);
    return false;
  }
  timer.stop();

  if (reply->error() != QNetworkReply::NoError) {
    if (error) *error = reply->errorString();
    reply->deleteLater();
    return false;
  }

  const QByteArray bytes = reply->readAll();
  reply->deleteLater();

  QFileInfo fi(destPath);
  fi.dir().mkpath(QStringLiteral("."));

  QFile outFile(destPath);
  if (!outFile.open(QIODevice::WriteOnly)) {
    if (error) *error = QStringLiteral("Cannot open %1 for writing: %2").arg(destPath, outFile.errorString());
    return false;
  }
  if (outFile.write(bytes) != bytes.size()) {
    if (error) *error = QStringLiteral("Failed to write all bytes to %1").arg(destPath);
    return false;
  }
  return true;
}

std::optional<QString> Client::uploadImage(const QString& localPath, QString* error, int timeoutMs) {
  QFileInfo fi(localPath);
  if (!fi.exists()) {
    if (error) *error = QStringLiteral("File not found: %1").arg(localPath);
    return std::nullopt;
  }

  QHttpMultiPart multiPart(QHttpMultiPart::FormDataType);

  QHttpPart overwritePart;
  overwritePart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant(QStringLiteral("form-data; name=\"overwrite\"")));
  overwritePart.setBody("true");
  multiPart.append(overwritePart);

  QHttpPart imagePart;
  imagePart.setHeader(QNetworkRequest::ContentDispositionHeader,
    QVariant(QStringLiteral("form-data; name=\"image\"; filename=\"%1\"").arg(fi.fileName())));
  QFile file(localPath);
  if (!file.open(QIODevice::ReadOnly)) {
    if (error) *error = QStringLiteral("Cannot open %1: %2").arg(localPath, file.errorString());
    return std::nullopt;
  }
  imagePart.setBody(file.readAll());
  multiPart.append(imagePart);

  QNetworkAccessManager manager;
  QNetworkRequest netReq(makeEndpoint(base_, QStringLiteral("upload/image")));
  QNetworkReply* reply = manager.post(netReq, &multiPart);

  QEventLoop loop;
  QTimer timer;
  timer.setSingleShot(true);
  QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

  timer.start(timeoutMs);
  loop.exec();

  if (!timer.isActive()) {
    reply->abort();
    reply->deleteLater();
    if (error) *error = QStringLiteral("Upload timed out after %1 ms").arg(timeoutMs);
    return std::nullopt;
  }
  timer.stop();

  if (reply->error() != QNetworkReply::NoError) {
    if (error) *error = reply->errorString();
    reply->deleteLater();
    return std::nullopt;
  }

  const QByteArray respData = reply->readAll();
  reply->deleteLater();

  QJsonParseError parseErr;
  QJsonDocument doc = QJsonDocument::fromJson(respData, &parseErr);
  if (!doc.isObject()) {
    if (error) *error = QStringLiteral("Invalid JSON from /upload/image: %1").arg(parseErr.errorString());
    return std::nullopt;
  }

  const QJsonObject obj = doc.object();
  const QString name = obj.value(QStringLiteral("name")).toString();
  const QString subfolder = obj.value(QStringLiteral("subfolder")).toString();
  if (!subfolder.isEmpty()) {
    QString res = subfolder;
    if (!res.endsWith(QLatin1Char('/'))) res += QLatin1Char('/');
    res += name;
    return res;
  }
  return name;
}

bool Client::interrupt(QString* error, int timeoutMs) {
  QNetworkAccessManager manager;
  QNetworkRequest netReq(makeEndpoint(base_, QStringLiteral("interrupt")));
  netReq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  QNetworkReply* reply = manager.post(netReq, QByteArray());

  QEventLoop loop;
  QTimer timer;
  timer.setSingleShot(true);
  QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

  const int waitMs = timeoutMs > 0 ? timeoutMs : 2000;
  timer.start(waitMs);
  loop.exec();

  if (!timer.isActive()) {
    reply->abort();
    reply->deleteLater();
    if (error) *error = QStringLiteral("Interrupt request timed out after %1 ms").arg(waitMs);
    return false;
  }
  timer.stop();

  if (reply->error() != QNetworkReply::NoError) {
    if (error) *error = reply->errorString();
    reply->deleteLater();
    return false;
  }
  reply->deleteLater();
  return true;
}

} // namespace sf::comfy
