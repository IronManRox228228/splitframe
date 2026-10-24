// Comfy Router synchronous HTTP client: direct partner-model calls on the official Router API.
// Sync-with-event-loop pattern, the same as comfy_client.cpp. Engine commands map the HTTP
// facts in RouterReply to user-facing error codes; this layer stays protocol-shaped.

#include "comfy/comfy_router_client.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <QtGlobal>

namespace sf::comfy {

namespace {

QUrl makeUrl(const QUrl& base, const QString& path) {
  QString b = base.toString();
  if (!b.endsWith(QLatin1Char('/'))) b += QLatin1Char('/');
  return QUrl(b + path);
}

// base + "v2/models[/{id}[/suffix]]" + query
QUrl routerUrl(const QUrl& base, const QString& path, const QUrlQuery& query) {
  QUrl url = makeUrl(base, path);
  if (!query.isEmpty()) url.setQuery(query);
  return url;
}

// One synchronous request with the standard Router headers. Transport failures set
// reply->error and leave status 0; HTTP exchanges (any status) fill the response facts.
RouterReply send(const QUrl& url, const char* verb, const QJsonObject& jsonBody,
                 const QString& idempotencyKey, const QString& etag, int timeoutMs) {
  RouterReply rep;

  const QString apiKey = RouterClient::resolveApiKey();
  if (apiKey.isEmpty()) {
    rep.error = QStringLiteral("no API key: set COMFY_API_KEY (or store comfyRouter/apiKey)");
    return rep;
  }

  QNetworkAccessManager manager;
  QNetworkRequest netReq(url);
  netReq.setRawHeader("X-API-Key", apiKey.toUtf8());
  netReq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  netReq.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                      QNetworkRequest::NoLessSafeRedirectPolicy);
  if (!idempotencyKey.isEmpty()) {
    netReq.setRawHeader("Idempotency-Key", idempotencyKey.toUtf8());
  }
  if (!etag.isEmpty()) {
    netReq.setRawHeader("If-None-Match", etag.toUtf8());
  }

  QByteArray postData;
  if (qstrcmp(verb, "GET") != 0) {
    postData = QJsonDocument(jsonBody).toJson(QJsonDocument::Compact);
  }

  QNetworkReply* reply = nullptr;
  if (qstrcmp(verb, "GET") == 0) {
    reply = manager.get(netReq);
  } else if (qstrcmp(verb, "POST") == 0) {
    reply = manager.post(netReq, postData);
  } else if (qstrcmp(verb, "PUT") == 0) {
    reply = manager.put(netReq, postData);
  } else {
    rep.error = QStringLiteral("internal: unsupported HTTP verb");
    return rep;
  }

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
    rep.error = QStringLiteral("request to %1 timed out after %2 ms").arg(url.toString()).arg(timeoutMs);
    return rep;
  }
  timer.stop();

  rep.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  rep.errorType = QString::fromUtf8(reply->rawHeader("X-Comfy-Error-Type"));
  rep.requestId = QString::fromUtf8(reply->rawHeader("X-Comfy-Request-Id"));
  rep.retryAfter = QString::fromUtf8(reply->rawHeader("Retry-After"));
  rep.etag = QString::fromUtf8(reply->rawHeader("ETag"));
  rep.idempotencyKey = idempotencyKey;
  rep.rawBody = reply->readAll();
  if (reply->error() != QNetworkReply::NoError && rep.status == 0) {
    // HTTP error statuses are still completed exchanges; only transport errors land here.
    rep.error = reply->errorString();
  }
  reply->deleteLater();

  QJsonParseError pe;
  QJsonDocument doc = QJsonDocument::fromJson(rep.rawBody, &pe);
  if (pe.error == QJsonParseError::NoError && doc.isObject()) {
    rep.body = doc.object();
  }
  return rep;
}

} // namespace

RouterClient::RouterClient(QUrl base) {
  if (base.isEmpty()) {
    const QString envUrl = qEnvironmentVariable("COMFY_ROUTER_BASE_URL");
    if (!envUrl.isEmpty()) {
      base_ = QUrl(envUrl);
    } else {
      base_ = QUrl(QStringLiteral("https://api.comfy.org"));
    }
  } else {
    base_ = std::move(base);
  }
}

QUrl RouterClient::base() const {
  return base_;
}

QString RouterClient::resolveApiKey() {
  const QString env = qEnvironmentVariable("COMFY_API_KEY");
  if (!env.isEmpty()) return env;
  QSettings settings;
  return settings.value(QStringLiteral("comfyRouter/apiKey")).toString();
}

bool RouterClient::splitModelId(const QString& modelId, QString* provider, QString* model) {
  const int slash = modelId.indexOf(QLatin1Char('/'));
  if (slash <= 0 || slash == modelId.size() - 1) return false;
  if (modelId.indexOf(QLatin1Char('/'), slash + 1) != -1) return false;
  if (provider) *provider = modelId.left(slash);
  if (model) *model = modelId.mid(slash + 1);
  return true;
}

RouterReply RouterClient::listModels(int limit, const QString& cursor, int timeoutMs) {
  QUrlQuery query;
  if (limit > 0) query.addQueryItem(QStringLiteral("limit"), QString::number(limit));
  if (!cursor.isEmpty()) query.addQueryItem(QStringLiteral("cursor"), cursor);
  return send(routerUrl(base_, QStringLiteral("v2/models"), query), "GET", {}, {}, {}, timeoutMs);
}

RouterReply RouterClient::getModel(const QString& modelId, int timeoutMs) {
  return send(routerUrl(base_, QStringLiteral("v2/models/") + modelId, {}), "GET", {}, {}, {},
              timeoutMs);
}

RouterReply RouterClient::getSchema(const QString& modelId, const QString& etag, int timeoutMs) {
  return send(routerUrl(base_, QStringLiteral("v2/models/") + modelId + QStringLiteral("/openapi.json"), {}),
              "GET", {}, {}, etag, timeoutMs);
}

RouterReply RouterClient::submitRequest(const QString& modelId, const QJsonObject& body,
                                        int timeoutMs) {
  const QString key = QUuid::createUuid().toString(QUuid::WithoutBraces);
  return send(routerUrl(base_, QStringLiteral("v2/models/") + modelId + QStringLiteral("/requests"), {}),
              "POST", body, key, {}, timeoutMs);
}

RouterReply RouterClient::requestStatus(const QString& modelId, const QString& requestId,
                                        int timeoutMs) {
  return send(routerUrl(base_, QStringLiteral("v2/models/") + modelId +
                                 QStringLiteral("/requests/") + requestId + QStringLiteral("/status"), {}),
              "GET", {}, {}, {}, timeoutMs);
}

RouterReply RouterClient::collectRequest(const QString& modelId, const QString& requestId,
                                         int timeoutMs) {
  return send(routerUrl(base_, QStringLiteral("v2/models/") + modelId +
                                 QStringLiteral("/requests/") + requestId, {}),
              "GET", {}, {}, {}, timeoutMs);
}

RouterReply RouterClient::cancelRequest(const QString& modelId, const QString& requestId,
                                        int timeoutMs) {
  return send(routerUrl(base_, QStringLiteral("v2/models/") + modelId +
                                 QStringLiteral("/requests/") + requestId + QStringLiteral("/cancel"), {}),
              "PUT", {}, {}, {}, timeoutMs);
}

bool RouterClient::downloadUrl(const QUrl& url, const QString& destPath, QString* error,
                               int timeoutMs) {
  QNetworkAccessManager manager;
  QNetworkRequest netReq(url);
  netReq.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                      QNetworkRequest::NoLessSafeRedirectPolicy);
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

} // namespace sf::comfy
