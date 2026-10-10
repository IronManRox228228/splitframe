#include "engine/jschema.h"

#include <cmath>

namespace sf::engine::js {

namespace {

QString typeOf(const QJsonValue& v) {
  switch (v.type()) {
    case QJsonValue::Null: return QStringLiteral("null");
    case QJsonValue::Bool: return QStringLiteral("boolean");
    case QJsonValue::Double: return QStringLiteral("number");
    case QJsonValue::String: return QStringLiteral("string");
    case QJsonValue::Array: return QStringLiteral("array");
    case QJsonValue::Object: return QStringLiteral("object");
    default: return QStringLiteral("undefined");
  }
}

bool matches(const QJsonValue& v, const QString& type) {
  if (type == QLatin1String("integer")) return v.isDouble() && std::floor(v.toDouble()) == v.toDouble();
  if (type == QLatin1String("number")) return v.isDouble();
  return typeOf(v) == type;
}

} // namespace

std::optional<QString> validate(const QJsonValue& value, const QJsonObject& schema, const QString& path) {
  if (schema.isEmpty()) return std::nullopt;
  const QJsonValue t = schema.value(QStringLiteral("type"));
  if (!t.isUndefined()) {
    QStringList types;
    if (t.isString()) types << t.toString();
    else for (const QJsonValue& e : t.toArray()) types << e.toString();
    bool ok = false;
    for (const QString& ty : types) ok = ok || matches(value, ty);
    if (!ok) return QStringLiteral("%1: expected %2, got %3").arg(path, types.join(QStringLiteral(" | ")), typeOf(value));
  }
  const QJsonValue en = schema.value(QStringLiteral("enum"));
  if (en.isArray()) {
    bool found = false;
    QStringList names;
    for (const QJsonValue& e : en.toArray()) {
      found = found || e == value;
      names << e.toString();
    }
    if (!found) return QStringLiteral("%1: must be one of %2").arg(path, names.join(QStringLiteral(", ")));
  }
  if (value.isDouble()) {
    const double d = value.toDouble();
    if (schema.contains(QStringLiteral("minimum")) && d < schema.value(QStringLiteral("minimum")).toDouble())
      return QStringLiteral("%1: must be >= %2").arg(path).arg(schema.value(QStringLiteral("minimum")).toDouble());
    if (schema.contains(QStringLiteral("maximum")) && d > schema.value(QStringLiteral("maximum")).toDouble())
      return QStringLiteral("%1: must be <= %2").arg(path).arg(schema.value(QStringLiteral("maximum")).toDouble());
  }
  if (value.isString()) {
    const qsizetype n = value.toString().size();
    if (schema.contains(QStringLiteral("minLength")) && n < schema.value(QStringLiteral("minLength")).toInt())
      return QStringLiteral("%1: must have at least %2 characters").arg(path).arg(schema.value(QStringLiteral("minLength")).toInt());
    if (schema.contains(QStringLiteral("maxLength")) && n > schema.value(QStringLiteral("maxLength")).toInt())
      return QStringLiteral("%1: must have at most %2 characters").arg(path).arg(schema.value(QStringLiteral("maxLength")).toInt());
  }
  if (value.isArray()) {
    const QJsonArray a = value.toArray();
    if (schema.contains(QStringLiteral("minItems")) && a.size() < schema.value(QStringLiteral("minItems")).toInt())
      return QStringLiteral("%1: needs at least %2 items").arg(path).arg(schema.value(QStringLiteral("minItems")).toInt());
    if (schema.contains(QStringLiteral("maxItems")) && a.size() > schema.value(QStringLiteral("maxItems")).toInt())
      return QStringLiteral("%1: allows at most %2 items").arg(path).arg(schema.value(QStringLiteral("maxItems")).toInt());
    const QJsonObject items = schema.value(QStringLiteral("items")).toObject();
    if (!items.isEmpty())
      for (qsizetype i = 0; i < a.size(); ++i)
        if (auto e = validate(a.at(i), items, QStringLiteral("%1[%2]").arg(path).arg(i))) return e;
  }
  if (value.isObject()) {
    const QJsonObject o = value.toObject();
    for (const QJsonValue& r : schema.value(QStringLiteral("required")).toArray())
      if (!o.contains(r.toString())) return QStringLiteral("%1.%2: required").arg(path, r.toString());
    const QJsonObject props = schema.value(QStringLiteral("properties")).toObject();
    const bool extraOk = schema.value(QStringLiteral("additionalProperties")).toBool(true);
    for (auto it = o.begin(); it != o.end(); ++it) {
      const auto p = props.find(it.key());
      if (p == props.end()) {
        if (!extraOk && schema.contains(QStringLiteral("properties"))) return QStringLiteral("%1: unknown property \"%2\"").arg(path, it.key());
        continue;
      }
      if (auto e = validate(it.value(), p->toObject(), QStringLiteral("%1.%2").arg(path, it.key()))) return e;
    }
  }
  return std::nullopt;
}

} // namespace sf::engine::js
