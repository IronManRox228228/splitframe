#pragma once

// Errors are values across the engine boundary: a command returns a Result, never throws.

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace sf::engine {

// Stable error codes: invalid_params, unknown_command, no_session, not_found, edit_rejected, io_error,
// unsupported, unavailable, busy, cancelled, internal.
struct Error {
  QString code;
  QString message;
  QJsonValue details; // undefined when absent

  QJsonObject toJson() const {
    QJsonObject o{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}};
    if (!details.isUndefined() && !details.isNull()) o.insert(QStringLiteral("details"), details);
    return o;
  }
};

struct Result {
  bool ok = true;
  QJsonValue value = QJsonObject{};
  Error error;

  static Result success(QJsonValue v = QJsonObject{}) {
    Result r;
    r.value = std::move(v);
    return r;
  }
  static Result failure(QString code, QString message, QJsonValue details = QJsonValue(QJsonValue::Undefined)) {
    Result r;
    r.ok = false;
    r.error = {std::move(code), std::move(message), std::move(details)};
    return r;
  }
  // {"ok":true,"result":...} or {"ok":false,"error":{code,message,details?}}
  QJsonObject toJson() const {
    return ok ? QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("result"), value}}
              : QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), error.toJson()}};
  }
};

} // namespace sf::engine
