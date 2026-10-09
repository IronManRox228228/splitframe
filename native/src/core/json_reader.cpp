#include "core/json_reader.h"

#include <cmath>

namespace sf {

QString childPath(const QString& parent, const QString& key) {
  return parent.isEmpty() ? key : parent + QLatin1Char('.') + key;
}

QString indexPath(const QString& parent, qsizetype i) { return QStringLiteral("%1[%2]").arg(parent).arg(i); }

QString Rd::typeName(const QJsonValue& v) {
  switch (v.type()) {
    case QJsonValue::Null: return QStringLiteral("null");
    case QJsonValue::Bool: return QStringLiteral("boolean");
    case QJsonValue::Double: return QStringLiteral("number");
    case QJsonValue::String: return QStringLiteral("string");
    case QJsonValue::Array: return QStringLiteral("array");
    case QJsonValue::Object: return QStringLiteral("object");
    case QJsonValue::Undefined: return QStringLiteral("undefined");
  }
  return QStringLiteral("unknown");
}

void Rd::wrongType(const char* expected) const {
  if (missing()) fail(QStringLiteral("Required"));
  fail(QStringLiteral("Expected %1, received %2").arg(QLatin1String(expected), typeName(value_)));
}

void Rd::requireObject() const {
  if (!value_.isObject()) wrongType("object");
}

Rd Rd::field(const QString& key) const {
  requireObject();
  return Rd(value_.toObject().value(key), childPath(path_, key));
}

std::vector<Rd> Rd::items() const {
  if (!value_.isArray()) wrongType("array");
  const QJsonArray a = value_.toArray();
  std::vector<Rd> out;
  out.reserve(static_cast<std::size_t>(a.size()));
  for (qsizetype i = 0; i < a.size(); ++i) out.emplace_back(a.at(i), indexPath(path_, i));
  return out;
}

std::vector<std::pair<QString, Rd>> Rd::entries() const {
  requireObject();
  const QJsonObject o = value_.toObject();
  std::vector<std::pair<QString, Rd>> out;
  for (auto it = o.begin(); it != o.end(); ++it) out.emplace_back(it.key(), Rd(it.value(), childPath(path_, it.key())));
  return out;
}

QString Rd::str() const {
  if (!value_.isString()) wrongType("string");
  return value_.toString();
}

bool Rd::boolean() const {
  if (!value_.isBool()) wrongType("boolean");
  return value_.toBool();
}

static QString fmt(double d) { return QString::number(d, 'g', 15); }

double Rd::num(NumBounds b) const {
  if (!value_.isDouble()) wrongType("number");
  const double d = value_.toDouble();
  if (!std::isfinite(d)) fail(QStringLiteral("Expected number, received nan"));
  if (b.min) {
    if (b.exclusiveMin ? !(d > *b.min) : !(d >= *b.min))
      fail(QStringLiteral("Number must be greater than %1%2")
               .arg(b.exclusiveMin ? QString() : QStringLiteral("or equal to "), fmt(*b.min)));
  }
  if (b.max && d > *b.max) fail(QStringLiteral("Number must be less than or equal to %1").arg(fmt(*b.max)));
  return d;
}

std::int64_t Rd::integer(IntBounds b) const {
  if (!value_.isDouble()) wrongType("number");
  const double d = value_.toDouble();
  // zod's .int() is a safe-integer check: JS cannot represent anything beyond 2^53 exactly
  if (!std::isfinite(d) || d != std::floor(d) || std::fabs(d) > 9007199254740991.0)
    fail(QStringLiteral("Expected integer, received float"));
  const auto v = static_cast<std::int64_t>(d);
  if (b.min && v < *b.min) fail(QStringLiteral("Number must be greater than or equal to %1").arg(*b.min));
  if (b.max && v > *b.max) fail(QStringLiteral("Number must be less than or equal to %1").arg(*b.max));
  return v;
}

} // namespace sf
