#pragma once

#include "core/error.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace sf {

// Name tables for string enums; specialised next to each enum in schema.h.
template <class E> struct EnumTable;

struct IntBounds {
  std::optional<std::int64_t> min;
  std::optional<std::int64_t> max;
};

struct NumBounds {
  std::optional<double> min;
  std::optional<double> max;
  bool exclusiveMin = false; // zod .positive(): > 0
};

QString childPath(const QString& parent, const QString& key);
QString indexPath(const QString& parent, qsizetype i);

// Cursor into a JSON document that remembers its path, so every failure names the bad field.
// Getters throw SchemaError; the opt*/...Or variants treat a missing key as absent/default, and
// anything else (including null) as a type error, exactly like zod's optional() and default().
class Rd {
 public:
  explicit Rd(QJsonValue value, QString path = {}) : value_(std::move(value)), path_(std::move(path)) {}

  const QString& path() const { return path_; }
  bool missing() const { return value_.isUndefined(); }
  const QJsonValue& raw() const { return value_; }

  [[noreturn]] void fail(const QString& issue) const { throw SchemaError(path_, issue); }

  // children; field() on a non-object throws, on a missing key yields a missing Rd
  Rd field(const QString& key) const;
  std::vector<Rd> items() const;                       // array elements
  std::vector<std::pair<QString, Rd>> entries() const; // object members, in key order
  void requireObject() const;

  QString str() const;
  std::int64_t integer(IntBounds b = {}) const;
  double num(NumBounds b = {}) const;
  bool boolean() const;
  template <class E> E enumeration() const;

  std::optional<QString> optStr() const { return missing() ? std::nullopt : std::optional<QString>(str()); }
  std::optional<std::int64_t> optInteger(IntBounds b = {}) const {
    return missing() ? std::nullopt : std::optional<std::int64_t>(integer(b));
  }
  std::optional<double> optNum(NumBounds b = {}) const {
    return missing() ? std::nullopt : std::optional<double>(num(b));
  }
  std::optional<bool> optBool() const { return missing() ? std::nullopt : std::optional<bool>(boolean()); }
  template <class E> std::optional<E> optEnum() const {
    return missing() ? std::nullopt : std::optional<E>(enumeration<E>());
  }

  QString strOr(const QString& def) const { return missing() ? def : str(); }
  std::int64_t integerOr(std::int64_t def, IntBounds b = {}) const { return missing() ? def : integer(b); }
  double numOr(double def, NumBounds b = {}) const { return missing() ? def : num(b); }
  bool boolOr(bool def) const { return missing() ? def : boolean(); }
  template <class E> E enumOr(E def) const { return missing() ? def : enumeration<E>(); }

  // Maps f over a present value; absent stays absent.
  template <class F> auto opt(F&& f) const -> std::optional<decltype(f(*this))> {
    if (missing()) return std::nullopt;
    return f(*this);
  }
  // Maps f over every element of a present array; absent yields an empty list (a zod default([])).
  template <class F> auto listOr(F&& f) const -> std::vector<decltype(f(*this))> {
    std::vector<decltype(f(*this))> out;
    if (missing()) return out;
    for (const Rd& e : items()) out.push_back(f(e));
    return out;
  }
  // Same, but the array itself is required.
  template <class F> auto list(F&& f) const -> std::vector<decltype(f(*this))> {
    std::vector<decltype(f(*this))> out;
    for (const Rd& e : items()) out.push_back(f(e));
    return out;
  }

  static QString typeName(const QJsonValue& v);

 private:
  [[noreturn]] void wrongType(const char* expected) const;

  QJsonValue value_;
  QString path_;
};

template <class E> QString enumName(E e) {
  for (const auto& [v, name] : EnumTable<E>::values)
    if (v == e) return QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size()));
  return {};
}

template <class E> E Rd::enumeration() const {
  if (missing()) fail(QStringLiteral("Required"));
  if (value_.isString()) {
    const QString s = value_.toString();
    for (const auto& [e, name] : EnumTable<E>::values)
      if (s == QLatin1StringView(name.data(), static_cast<qsizetype>(name.size()))) return e;
  }
  QStringList names;
  for (const auto& entry : EnumTable<E>::values) names << QStringLiteral("'%1'").arg(enumName(entry.first));
  const QString got = value_.isString() ? QStringLiteral("'%1'").arg(value_.toString()) : typeName(value_);
  fail(QStringLiteral("Invalid enum value. Expected %1, received %2").arg(names.join(QStringLiteral(" | ")), got));
}

} // namespace sf
