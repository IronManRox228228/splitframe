#pragma once

// JSON-RPC 2.0 over the engine: every engine command is a method (params = the command's argument object).
// Transport-agnostic: the HTTP server and the stdio loop both feed handleText().
//
// Error codes: -32700 parse error, -32600 invalid request, -32601 method not found (unknown command),
// -32602 invalid params; anything else is -32000 with error.data = {code, message?, details?} carrying the engine's error.
// Positional (array) params are not supported: commands take named arguments.

#include "engine/engine.h"

#include <QByteArray>
#include <QJsonObject>

#include <optional>

namespace sf::engine {

class JsonRpc {
public:
  explicit JsonRpc(Engine& engine) : engine_(engine) {}

  // Response body for a request or batch; empty when everything was a notification.
  QByteArray handleText(const QByteArray& body);
  // One request object. nullopt for a notification (no id).
  std::optional<QJsonObject> handleRequest(const QJsonValue& request);

  static QJsonObject errorResponse(const QJsonValue& id, int code, const QString& message, const QJsonValue& data = QJsonValue(QJsonValue::Undefined));
  static int rpcCodeFor(const QString& engineCode);

private:
  Engine& engine_;
};

} // namespace sf::engine
