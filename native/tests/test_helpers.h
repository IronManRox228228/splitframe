#pragma once

// Shared fixtures for the core test suites; mirrors packages/editor-core/src/test-helpers.ts.

#include "core/apply.h"
#include "core/history.h"
#include "core/ops.h"
#include "core/schema.h"
#include "core/timeline_doc.h"

#include <QJsonDocument>
#include <QRegularExpression>
#include <QString>
#include <QTest>
#include <functional>

namespace sf::test {

inline TimelineDoc makeDoc() {
  return createEmptyDoc({.id = newId(QStringLiteral("prj")), .name = QStringLiteral("Test Project"), .fps = 30});
}

inline const Track& trackOfKind(const TimelineDoc& doc, TrackKind kind) {
  for (const Track& t : doc.tracks)
    if (t.kind == kind) return t;
  throw std::runtime_error("no " + enumName(kind).toStdString() + " track");
}

using Extra = std::function<void(ItemInit&)>;

inline Item videoItem(const TimelineDoc& doc, Frame start, Frame duration, const Extra& extra = {}) {
  ItemInit init;
  init.id = newId(QStringLiteral("itm"));
  init.trackId = trackOfKind(doc, TrackKind::Video).id;
  init.startFrame = start;
  init.durationFrames = duration;
  init.assetId = QStringLiteral("ast_video");
  init.sourceInFrame = 0;
  if (extra) extra(init);
  return createItem(ItemType::Video, init);
}

inline Item audioItem(const TimelineDoc& doc, Frame start, Frame duration, const Extra& extra = {}) {
  ItemInit init;
  init.id = newId(QStringLiteral("itm"));
  init.trackId = trackOfKind(doc, TrackKind::Audio).id;
  init.startFrame = start;
  init.durationFrames = duration;
  init.assetId = QStringLiteral("ast_music");
  init.sourceInFrame = 0;
  if (extra) extra(init);
  return createItem(ItemType::Audio, init);
}

inline TextStyle plainStyle() {
  TextStyle s;
  s.fontFamily = QStringLiteral("Inter");
  s.fontSize = 64;
  s.color = QStringLiteral("#ffffff");
  return s;
}

inline Item textItem(const TimelineDoc& doc, Frame start, Frame duration, const QString& text = QStringLiteral("Hello")) {
  ItemInit init;
  init.id = newId(QStringLiteral("itm"));
  init.trackId = trackOfKind(doc, TrackKind::Text).id;
  init.startFrame = start;
  init.durationFrames = duration;
  init.props = TextProps{text, plainStyle()};
  return createItem(ItemType::Text, init);
}

inline Op addItem(const Item& item) { return ItemAdd{item, false}; }

inline TimelineDoc docWith(const TimelineDoc& doc, const std::vector<Item>& items) {
  std::vector<Op> ops;
  for (const Item& i : items) ops.push_back(addItem(i));
  return applyOps(doc, ops).doc;
}

inline const Item& itemById(const TimelineDoc& doc, const QString& id) { return requireItem(doc, id); }

inline QString dumpJson(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented)); }
inline QString dump(const Item& i) { return dumpJson(toJson(i)); }
inline QString dump(const std::vector<Item>& v) {
  QJsonArray a;
  for (const Item& i : v) a.append(toJson(i));
  return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Indented));
}
inline QString dump(const std::vector<Track>& v) {
  QJsonArray a;
  for (const Track& t : v) a.append(toJson(t));
  return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Indented));
}
inline QString dump(const std::vector<Marker>& v) {
  QJsonArray a;
  for (const Marker& t : v) a.append(toJson(t));
  return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Indented));
}
inline QString dump(const TimelineDoc& d) { return dumpJson(toJson(d)); }

inline std::vector<Item> byId(std::vector<Item> items) {
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.id < b.id; });
  return items;
}

// Equivalent of expect(a).toEqual(b) for model types: prints both sides as JSON on failure.
#define SF_COMPARE(actual, expected)                                                                      \
  do {                                                                                                    \
    if (!((actual) == (expected)))                                                                        \
      QFAIL(qPrintable(QStringLiteral("not equal\n--- actual\n%1\n--- expected\n%2")                      \
                           .arg(::sf::test::dump(actual), ::sf::test::dump(expected))));                  \
  } while (0)

// expect(() => expr).toThrow(ExType)
#define SF_THROWS(expr, ExType)                                                                           \
  do {                                                                                                    \
    bool threw_ = false;                                                                                  \
    try {                                                                                                 \
      (void)(expr);                                                                                       \
    } catch (const ExType&) {                                                                             \
      threw_ = true;                                                                                      \
    }                                                                                                     \
    QVERIFY2(threw_, "expected " #ExType " from " #expr);                                                 \
  } while (0)

// expect(() => expr).toThrow(/pattern/i): any std::exception whose message matches
#define SF_THROWS_MATCH(expr, pattern)                                                                    \
  do {                                                                                                    \
    bool threw_ = false;                                                                                  \
    QString what_;                                                                                        \
    try {                                                                                                 \
      (void)(expr);                                                                                       \
    } catch (const std::exception& e_) {                                                                  \
      threw_ = true;                                                                                      \
      what_ = QString::fromUtf8(e_.what());                                                               \
    }                                                                                                     \
    QVERIFY2(threw_, "expected an exception from " #expr);                                                \
    QVERIFY2(QRegularExpression(QStringLiteral(pattern), QRegularExpression::CaseInsensitiveOption)       \
                 .match(what_)                                                                            \
                 .hasMatch(),                                                                             \
             qPrintable(QStringLiteral("message \"%1\" does not match " pattern).arg(what_)));            \
  } while (0)

// expect(() => expr).toThrow()
#define SF_THROWS_ANY(expr) SF_THROWS(expr, std::exception)

#define SF_NO_THROW(expr)                                                                                 \
  do {                                                                                                    \
    try {                                                                                                 \
      (void)(expr);                                                                                       \
    } catch (const std::exception& e_) {                                                                  \
      QFAIL(qPrintable(QStringLiteral("unexpected exception from " #expr ": %1").arg(QString::fromUtf8(e_.what())))); \
    }                                                                                                     \
  } while (0)

} // namespace sf::test
