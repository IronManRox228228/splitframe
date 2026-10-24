#pragma once

// Comfy Router synchronous HTTP client: direct partner-model calls on the official Router API.
//
// Contract (docs.comfy.org/development/comfy-router documentation, fetched Oct 2026):
//   base       https://api.comfy.org (override: COMFY_ROUTER_BASE_URL)
//   auth       X-API-Key: <api key>            (COMFY_API_KEY env, else QSettings comfyRouter/apiKey)
//   catalog    GET  /v2/models?limit&cursor    -> {data, has_more, next_cursor, limit}
//   one model  GET  /v2/models/{provider}/{model}
//   schema     GET  /v2/models/{provider}/{model}/openapi.json (ETag / If-None-Match, 304)
//   submit     POST /v2/models/{provider}/{model}/requests  (+ Idempotency-Key) -> 201 {request_id, ...}
//   status     GET  /v2/models/{provider}/{model}/requests/{id}/status -> {status, queue_position, error_type?}
//   collect    GET  /v2/models/{provider}/{model}/requests/{id} -> 200 native output / 202 status body
//   cancel     PUT  /v2/models/{provider}/{model}/requests/{id}/cancel
// Every error response carries an HTTP status, an `X-Comfy-Error-Type` bucket, and an
// `X-Comfy-Request-Id`. The API key is never written to log strings or error messages.

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QUrl>

#include <vector>

namespace sf::comfy {

// Raw HTTP facts every Router response carries.
struct RouterReply {
  int status = 0;        // 0 = transport failure (see `error`)
  QString errorType;     // X-Comfy-Error-Type bucket; empty on success
  QString requestId;     // X-Comfy-Request-Id (Router request id) when present
  QString retryAfter;    // Retry-After header value (seconds) when present
  QString etag;          // ETag response header (schema route) when present
  QString idempotencyKey;// Idempotency-Key we sent on submit (empty otherwise)
  QJsonObject body;      // parsed when the body was a JSON object
  QByteArray rawBody;    // full body bytes (JSON, binary, or an HTML proxy error page)
  QString error;         // transport failure detail; empty on a completed HTTP exchange

  bool ok() const { return error.isEmpty() && status >= 200 && status < 300; }
  bool transportFailed() const { return !error.isEmpty(); }
};

class RouterClient {
public:
  // Empty base -> COMFY_ROUTER_BASE_URL env -> https://api.comfy.org.
  explicit RouterClient(QUrl base = {});
  QUrl base() const;

  // COMFY_API_KEY env first, then QSettings("comfyRouter/apiKey"). Empty when unconfigured.
  static QString resolveApiKey();

  // "provider/model" -> {provider, model}; nullopt when the id is not exactly provider/model.
  static bool splitModelId(const QString& modelId, QString* provider, QString* model);

  RouterReply listModels(int limit, const QString& cursor, int timeoutMs = 15000);
  RouterReply getModel(const QString& modelId, int timeoutMs = 15000);
  // `etag` (from a previous reply's headers) is sent as If-None-Match; a 304 has empty rawBody.
  RouterReply getSchema(const QString& modelId, const QString& etag, int timeoutMs = 30000);
  RouterReply submitRequest(const QString& modelId, const QJsonObject& body, int timeoutMs = 30000);
  RouterReply requestStatus(const QString& modelId, const QString& requestId, int timeoutMs = 15000);
  RouterReply collectRequest(const QString& modelId, const QString& requestId, int timeoutMs = 120000);
  RouterReply cancelRequest(const QString& modelId, const QString& requestId, int timeoutMs = 15000);

  // Downloads any asset URL a model result carries (result URLs expire; download promptly).
  static bool downloadUrl(const QUrl& url, const QString& destPath, QString* error = nullptr,
                          int timeoutMs = 120000);

  QUrl base_;
};

} // namespace sf::comfy
