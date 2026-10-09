#pragma once

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

// Deep JSON comparison for golden tests: parsed values, not text, so key order and "1" vs "1.0"
// never matter. Returns an empty string when equal, else the first difference with its path.
namespace sf::test {

inline QString compareJson(const QJsonValue& a, const QJsonValue& b, const QString& path = QStringLiteral("$")) {
  if (a.isDouble() && b.isDouble()) {
    return a.toDouble() == b.toDouble() ? QString()
                                        : QStringLiteral("%1: %2 != %3").arg(path).arg(a.toDouble(), 0, 'g', 17).arg(b.toDouble(), 0, 'g', 17);
  }
  if (a.type() != b.type()) {
    return QStringLiteral("%1: type %2 != %3").arg(path).arg(static_cast<int>(a.type())).arg(static_cast<int>(b.type()));
  }
  switch (a.type()) {
    case QJsonValue::Object: {
      const QJsonObject oa = a.toObject();
      const QJsonObject ob = b.toObject();
      for (auto it = oa.begin(); it != oa.end(); ++it) {
        if (!ob.contains(it.key())) return QStringLiteral("%1.%2: only in actual").arg(path, it.key());
        const QString d = compareJson(it.value(), ob.value(it.key()), path + QLatin1Char('.') + it.key());
        if (!d.isEmpty()) return d;
      }
      for (auto it = ob.begin(); it != ob.end(); ++it)
        if (!oa.contains(it.key())) return QStringLiteral("%1.%2: only in expected").arg(path, it.key());
      return {};
    }
    case QJsonValue::Array: {
      const QJsonArray xa = a.toArray();
      const QJsonArray xb = b.toArray();
      if (xa.size() != xb.size()) return QStringLiteral("%1: length %2 != %3").arg(path).arg(xa.size()).arg(xb.size());
      for (qsizetype i = 0; i < xa.size(); ++i) {
        const QString d = compareJson(xa.at(i), xb.at(i), QStringLiteral("%1[%2]").arg(path).arg(i));
        if (!d.isEmpty()) return d;
      }
      return {};
    }
    case QJsonValue::String: return a.toString() == b.toString() ? QString() : QStringLiteral("%1: \"%2\" != \"%3\"").arg(path, a.toString(), b.toString());
    case QJsonValue::Bool: return a.toBool() == b.toBool() ? QString() : QStringLiteral("%1: bool differs").arg(path);
    default: return {};
  }
}

inline QJsonValue readJsonFile(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) qFatal("cannot open fixture %s", qPrintable(path));
  QJsonParseError err;
  const QJsonDocument d = QJsonDocument::fromJson(f.readAll(), &err);
  if (err.error != QJsonParseError::NoError) qFatal("bad fixture %s: %s", qPrintable(path), qPrintable(err.errorString()));
  return d.isArray() ? QJsonValue(d.array()) : QJsonValue(d.object());
}

} // namespace sf::test
