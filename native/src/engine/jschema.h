#pragma once

// Tiny JSON-Schema subset: builders for command input schemas and a validator for incoming arguments.
// Supported keywords: type (string or list), enum, minimum, maximum, minLength, maxLength, minItems, maxItems,
// items, properties, required, additionalProperties (bool). Enough to describe and check every command.

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <initializer_list>
#include <optional>
#include <utility>

namespace sf::engine::js {

inline QJsonObject typed(const char* type, const QString& desc) {
  QJsonObject o{{QStringLiteral("type"), QString::fromLatin1(type)}};
  if (!desc.isEmpty()) o.insert(QStringLiteral("description"), desc);
  return o;
}
inline QJsonObject str(const QString& desc = {}) { return typed("string", desc); }
inline QJsonObject boolean(const QString& desc = {}) { return typed("boolean", desc); }
inline QJsonObject number(const QString& desc = {}) { return typed("number", desc); }
inline QJsonObject integer(const QString& desc = {}) { return typed("integer", desc); }
inline QJsonObject integer(const QString& desc, qint64 min, std::optional<qint64> max = std::nullopt) {
  QJsonObject o = typed("integer", desc);
  o.insert(QStringLiteral("minimum"), static_cast<double>(min));
  if (max) o.insert(QStringLiteral("maximum"), static_cast<double>(*max));
  return o;
}
inline QJsonObject number(const QString& desc, double min, std::optional<double> max = std::nullopt) {
  QJsonObject o = typed("number", desc);
  o.insert(QStringLiteral("minimum"), min);
  if (max) o.insert(QStringLiteral("maximum"), *max);
  return o;
}
inline QJsonObject choice(const QStringList& values, const QString& desc = {}) {
  QJsonObject o = typed("string", desc);
  QJsonArray a;
  for (const QString& v : values) a.append(v);
  o.insert(QStringLiteral("enum"), a);
  return o;
}
inline QJsonObject array(const QJsonObject& items, const QString& desc = {}) {
  QJsonObject o = typed("array", desc);
  o.insert(QStringLiteral("items"), items);
  return o;
}
// Free-form object (not checked beyond being an object).
inline QJsonObject any(const QString& desc = {}) { return typed("object", desc); }
// Anything at all.
inline QJsonObject anything(const QString& desc = {}) {
  QJsonObject o;
  if (!desc.isEmpty()) o.insert(QStringLiteral("description"), desc);
  return o;
}
inline QJsonObject obj(std::initializer_list<std::pair<QString, QJsonObject>> props, const QStringList& required = {}, bool additional = false) {
  QJsonObject p;
  for (const auto& [k, v] : props) p.insert(k, v);
  QJsonObject o{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("properties"), p}, {QStringLiteral("additionalProperties"), additional}};
  if (!required.isEmpty()) {
    QJsonArray r;
    for (const QString& s : required) r.append(s);
    o.insert(QStringLiteral("required"), r);
  }
  return o;
}

// nullopt when valid, else "arguments.items[2]: must be >= 0".
std::optional<QString> validate(const QJsonValue& value, const QJsonObject& schema, const QString& path = QStringLiteral("arguments"));

} // namespace sf::engine::js
