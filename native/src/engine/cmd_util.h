#pragma once

// Helpers shared by the command implementation files (internal to sf_engine).

#include "core/apply.h"
#include "core/timeline_doc.h"
#include "engine/engine.h"
#include "engine/jschema.h"

#include <QJsonArray>
#include <QJsonObject>

#include <memory>

namespace sf::engine {

inline QString q(const char* s) { return QString::fromLatin1(s); }

inline Result invalid(const QString& message) { return Result::failure(q("invalid_params"), message); }
inline Result notFound(const QString& what, const QString& hint = {}) {
  return Result::failure(q("not_found"), hint.isEmpty() ? what : what + QStringLiteral(" ") + hint);
}

// A command with its schema. Registers `session`/`origin` on the schema (see Engine::add).
inline CommandSpec spec(const char* name, const char* alias, const char* domain, bool mutates, const QString& description, QJsonObject schema,
                        Handler handler, bool needsSession = true) {
  CommandSpec s;
  s.name = q(name);
  s.alias = q(alias);
  s.domain = q(domain);
  s.mutates = mutates;
  s.needsSession = needsSession;
  s.description = description;
  s.inputSchema = std::move(schema);
  s.handler = std::move(handler);
  return s;
}

// Parses raw op JSON (throws SchemaError, which Engine::call reports as invalid_params).
inline std::vector<Op> parseOps(const QJsonArray& arr, const QString& path = QStringLiteral("ops")) {
  std::vector<Op> ops;
  for (qsizetype i = 0; i < arr.size(); ++i) {
    Op op = parseOp(Rd(arr.at(i), indexPath(path, i)));
    validateOp(op);
    ops.push_back(std::move(op));
  }
  return ops;
}
inline Op parseOneOp(const QJsonObject& o) {
  Op op = parseOp(Rd(o, QStringLiteral("op")));
  validateOp(op);
  return op;
}

// Ids created by ops (item.add / split / clone, track.add, marker.add, effect.add, mask.add), batches flattened.
QJsonObject createdIds(const std::vector<Op>& ops);

// Applies ops to the session's project as one undo group tagged with the call's origin. On success returns
// {applied, label, created, inverse, canUndo, canRedo, undoLabel} merged with `extra`.
Result applyToSession(CallContext& c, const std::vector<Op>& ops, const QString& label, const QJsonObject& extra = {});

QJsonObject itemSummary(const TimelineDoc& doc, const Item& item);
QJsonObject trackJson(const Track& t);
QJsonObject assetBrief(const Asset& a, const editor::MediaPool* pool);

// Timeline seconds <-> frames at the session's fps.
inline Frame toFrames(double sec, const TimelineDoc& d) { return secondsToFrames(sec, static_cast<double>(d.project.fps)); }

// PNG output shared by frame.render and thumbnail.get: writes `path` (or a temp file) and/or returns base64.
struct ImageOut {
  QString path;
  bool inlineData = false;
  int maxWidth = 1280, maxHeight = 1280;
};
Result imageResult(const QImage& image, const ImageOut& out, const QString& stem, const QJsonObject& extra = {});
void addImageOutProps(QJsonObject& schema); // path, inline, maxWidth, maxHeight
ImageOut imageOutFrom(const QJsonObject& args);

// The asset table (original files, proxies are never used for stills / export) for the session's project.
render::AssetTable assetTable(const Session& s);

} // namespace sf::engine
