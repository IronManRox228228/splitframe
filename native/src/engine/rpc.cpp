#include "engine/rpc.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace sf::engine {

QJsonObject JsonRpc::errorResponse(const QJsonValue& id, int code, const QString& message, const QJsonValue& data) {
  QJsonObject err{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}};
  if (!data.isUndefined()) err.insert(QStringLiteral("data"), data);
  return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id.isUndefined() ? QJsonValue(QJsonValue::Null) : id}, {QStringLiteral("error"), err}};
}

int JsonRpc::rpcCodeFor(const QString& code) {
  if (code == QLatin1String("invalid_params")) return -32602;
  if (code == QLatin1String("unknown_command")) return -32601;
  return -32000;
}

std::optional<QJsonObject> JsonRpc::handleRequest(const QJsonValue& request) {
  if (!request.isObject()) return errorResponse(QJsonValue::Null, -32600, QStringLiteral("Invalid Request: expected an object."));
  const QJsonObject req = request.toObject();
  const bool hasId = req.contains(QStringLiteral("id"));
  const QJsonValue id = req.value(QStringLiteral("id"));
  const auto reply = [&](const QJsonObject& o) -> std::optional<QJsonObject> { return hasId ? std::optional<QJsonObject>(o) : std::nullopt; };
  if (req.value(QStringLiteral("jsonrpc")).toString() != QLatin1String("2.0") || !req.value(QStringLiteral("method")).isString() ||
      (hasId && !(id.isString() || id.isDouble() || id.isNull())))
    return errorResponse(hasId ? id : QJsonValue(QJsonValue::Null), -32600, QStringLiteral("Invalid Request: need jsonrpc \"2.0\", a string method and a string/number id."));
  const QJsonValue params = req.value(QStringLiteral("params"));
  if (!params.isUndefined() && !params.isObject() && !params.isNull())
    return reply(errorResponse(id, -32602, QStringLiteral("Invalid params: pass named arguments as an object.")));

  const Result r = engine_.call(req.value(QStringLiteral("method")).toString(), params.toObject());
  if (!hasId) return std::nullopt;
  if (!r.ok) {
    QJsonObject data{{QStringLiteral("code"), r.error.code}};
    if (!r.error.details.isUndefined()) data.insert(QStringLiteral("details"), r.error.details);
    return errorResponse(id, rpcCodeFor(r.error.code), r.error.message, data);
  }
  return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id}, {QStringLiteral("result"), r.value}};
}

QByteArray JsonRpc::handleText(const QByteArray& body) {
  QJsonParseError pe;
  const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
  if (pe.error != QJsonParseError::NoError || doc.isNull())
    return QJsonDocument(errorResponse(QJsonValue::Null, -32700, QStringLiteral("Parse error: %1").arg(pe.errorString()))).toJson(QJsonDocument::Compact);
  if (doc.isArray()) {
    if (doc.array().isEmpty()) return QJsonDocument(errorResponse(QJsonValue::Null, -32600, QStringLiteral("Invalid Request: empty batch."))).toJson(QJsonDocument::Compact);
    QJsonArray out;
    for (const QJsonValue& v : doc.array())
      if (const auto r = handleRequest(v)) out.append(*r);
    return out.isEmpty() ? QByteArray() : QJsonDocument(out).toJson(QJsonDocument::Compact);
  }
  const auto r = handleRequest(doc.object());
  return r ? QJsonDocument(*r).toJson(QJsonDocument::Compact) : QByteArray();
}

} // namespace sf::engine
