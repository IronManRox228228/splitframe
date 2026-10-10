#include "engine/mcp.h"

#include "engine/cmd_util.h"
#include "engine/rpc.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>
#include <QUrlQuery>

namespace sf::engine {

namespace {

QString compact(const QJsonValue& v) {
  if (v.isObject()) return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
  if (v.isArray()) return QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact));
  return QString::fromUtf8(QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact));
}

QJsonObject textBlock(const QString& text) { return QJsonObject{{q("type"), q("text")}, {q("text"), text}}; }

QJsonObject rpcResult(const QJsonValue& id, const QJsonObject& result) { return QJsonObject{{q("jsonrpc"), q("2.0")}, {q("id"), id}, {q("result"), result}}; }

} // namespace

QStringList McpServer::supportedVersions() { return {QStringLiteral("2025-11-25"), QStringLiteral("2025-06-18"), QStringLiteral("2025-03-26"), QStringLiteral("2024-11-05")}; }

QString McpServer::toolName(const QString& commandName) {
  QString n = commandName;
  return n.replace(QLatin1Char('.'), QLatin1Char('_'));
}

McpServer::McpServer(Engine& engine, Sender send, QObject* parent) : QObject(parent), engine_(engine), send_(std::move(send)) {
  connect(&engine_, &Engine::event, this, [this](const QString& name, const QJsonObject& data, qint64) { onEvent(name, data); });
}

void McpServer::notify(const QString& method, const QJsonObject& params) {
  send_(QJsonObject{{q("jsonrpc"), q("2.0")}, {q("method"), method}, {q("params"), params}});
}

void McpServer::onEvent(const QString& name, const QJsonObject& data) {
  if (logging_) {
    QJsonObject d = data;
    d.insert(q("event"), name);
    notify(q("notifications/message"), {{q("level"), q("info")}, {q("logger"), q("splitframe")}, {q("data"), d}});
  }
  if (name == QLatin1String("ops.applied") || name == QLatin1String("doc.changed") || name == QLatin1String("media.imported") || name == QLatin1String("session.opened")) {
    for (const QString& uri : subscribed_) notify(q("notifications/resources/updated"), {{q("uri"), uri}});
  }
  if (name == QLatin1String("export.progress") || name == QLatin1String("export.finished")) {
    const QString job = data.value(q("jobId")).toString();
    const auto it = progressTokens_.find(job);
    if (it == progressTokens_.end()) return;
    const bool done = name == QLatin1String("export.finished");
    const double total = data.value(q("totalFrames")).toDouble(1);
    notify(q("notifications/progress"), {{q("progressToken"), it->second}, {q("progress"), done ? total : data.value(q("frame")).toDouble()}, {q("total"), total},
                                         {q("message"), done ? data.value(q("state")).toString() : data.value(q("stage")).toString()}});
    if (done) progressTokens_.erase(it);
  }
}

QJsonObject McpServer::toolsList() const {
  QJsonArray tools;
  for (const auto& c : engine_.commands()) {
    QJsonObject annotations{{q("readOnlyHint"), !c->mutates}, {q("openWorldHint"), false}};
    if (c->mutates) annotations.insert(q("destructiveHint"), false); // edits are undoable (history.undo)
    QJsonObject tool{{q("name"), toolName(c->name)}, {q("title"), c->name}, {q("description"), c->description}, {q("inputSchema"), c->inputSchema},
                     {q("annotations"), annotations}, {q("_meta"), QJsonObject{{q("splitframe/domain"), c->domain}, {q("splitframe/command"), c->name}}}};
    tools.append(tool);
  }
  return QJsonObject{{q("tools"), tools}};
}

QJsonObject McpServer::toolsCall(const QJsonObject& params, const QJsonValue&, QJsonObject* protocolError) {
  const QString name = params.value(q("name")).toString();
  const CommandSpec* cmd = name.isEmpty() ? nullptr : engine_.find(name);
  if (!cmd) {
    *protocolError = JsonRpc::errorResponse(QJsonValue::Null, -32602, QStringLiteral("Unknown tool: %1").arg(name));
    return {};
  }
  const QJsonValue argsV = params.value(q("arguments"));
  QJsonObject args = argsV.isObject() ? argsV.toObject() : QJsonObject();
  if (!argsV.isUndefined() && !argsV.isObject() && !argsV.isNull()) {
    return QJsonObject{{q("content"), QJsonArray{textBlock(QStringLiteral("arguments must be an object."))}}, {q("isError"), true}};
  }
  const Result r = engine_.call(cmd->name, args);
  if (!r.ok) {
    return QJsonObject{{q("content"), QJsonArray{textBlock(compact(r.error.toJson()))}}, {q("structuredContent"), QJsonObject{{q("error"), r.error.toJson()}}}, {q("isError"), true}};
  }
  QJsonObject val = r.value.toObject();
  QJsonArray content;
  if (val.contains(q("base64")) && val.contains(q("mimeType"))) {
    content.append(QJsonObject{{q("type"), q("image")}, {q("data"), val.value(q("base64"))}, {q("mimeType"), val.value(q("mimeType"))}});
    val.remove(q("base64"));
  }
  content.prepend(textBlock(compact(val)));
  const QJsonValue token = params.value(q("_meta")).toObject().value(q("progressToken"));
  if (cmd->name == QLatin1String("export.start") && !token.isUndefined()) progressTokens_[val.value(q("jobId")).toString()] = token;
  return QJsonObject{{q("content"), content}, {q("structuredContent"), val}, {q("isError"), false}};
}

QJsonObject McpServer::resourcesRead(const QString& uri, QJsonObject* protocolError) {
  const QUrl url(uri);
  if (url.scheme() != QLatin1String("splitframe")) {
    *protocolError = JsonRpc::errorResponse(QJsonValue::Null, -32002, QStringLiteral("Resource not found: %1").arg(uri), QJsonObject{{q("uri"), uri}});
    return {};
  }
  const QString kind = url.host();
  Result r;
  QString mime = q("application/json");
  if (kind == QLatin1String("project")) r = engine_.call(q("project.info"));
  else if (kind == QLatin1String("timeline")) r = engine_.call(q("timeline.get"), {{q("mode"), QUrlQuery(url).queryItemValue(q("mode")) == QLatin1String("detail") ? q("detail") : q("summary")}});
  else if (kind == QLatin1String("assets")) r = engine_.call(q("assets.list"));
  else if (kind == QLatin1String("frame")) {
    bool ok = false;
    const qint64 frame = url.path().mid(1).toLongLong(&ok);
    if (!ok) r = Result::failure(q("invalid_params"), QStringLiteral("splitframe://frame/{frame}: frame must be an integer."));
    else {
      r = engine_.call(q("frame.render"), {{q("frame"), static_cast<double>(frame)}, {q("inline"), true}, {q("maxWidth"), 640}, {q("maxHeight"), 640}});
      mime = q("image/png");
    }
  } else {
    *protocolError = JsonRpc::errorResponse(QJsonValue::Null, -32002, QStringLiteral("Resource not found: %1").arg(uri), QJsonObject{{q("uri"), uri}});
    return {};
  }
  if (!r.ok) {
    *protocolError = JsonRpc::errorResponse(QJsonValue::Null, r.error.code == QLatin1String("no_session") ? -32002 : -32603, r.error.message, r.error.toJson());
    return {};
  }
  QJsonObject entry{{q("uri"), uri}, {q("mimeType"), mime}};
  if (mime == QLatin1String("image/png")) entry.insert(q("blob"), r.value.toObject().value(q("base64")));
  else entry.insert(q("text"), compact(r.value));
  return QJsonObject{{q("contents"), QJsonArray{entry}}};
}

std::optional<QJsonObject> McpServer::handle(const QJsonObject& msg) {
  const bool hasId = msg.contains(q("id"));
  const QJsonValue id = msg.value(q("id"));
  const QString method = msg.value(q("method")).toString();
  if (msg.value(q("jsonrpc")).toString() != QLatin1String("2.0") || (!msg.contains(q("method")) && !hasId))
    return JsonRpc::errorResponse(id, -32600, QStringLiteral("Invalid Request"));
  if (!msg.contains(q("method"))) return std::nullopt; // a response to something we sent (we send none that need one)
  const QJsonObject params = msg.value(q("params")).toObject();
  auto fail = [&](int code, const QString& m) { return hasId ? std::optional<QJsonObject>(JsonRpc::errorResponse(id, code, m)) : std::nullopt; };

  if (method == QLatin1String("initialize")) {
    const QString want = params.value(q("protocolVersion")).toString();
    negotiated_ = supportedVersions().contains(want) ? want : supportedVersions().first();
    return rpcResult(id, {{q("protocolVersion"), negotiated_},
                          {q("capabilities"), QJsonObject{{q("tools"), QJsonObject{{q("listChanged"), false}}}, {q("resources"), QJsonObject{{q("subscribe"), true}, {q("listChanged"), false}}},
                                                          {q("logging"), QJsonObject{}}}},
                          {q("serverInfo"), QJsonObject{{q("name"), q("splitframe")}, {q("title"), q("SplitFrame")}, {q("version"), QCoreApplication::applicationVersion().isEmpty() ? q("0.1.0") : QCoreApplication::applicationVersion()}}},
                          {q("instructions"), QStringLiteral("SplitFrame video editor engine. Open or create a project first (project_new / project_open), read the timeline with timeline_get, edit with the "
                                                             "specific tools (clip_add, item_split, ...) or ops_apply, verify by reading back or frame_render, and use history_undo to revert. Time is in "
                                                             "integer timeline frames at the project fps. Every tool has a domain (editor | colour | audio | engine) in its _meta.")}});
  }
  if (method.startsWith(QLatin1String("notifications/"))) return std::nullopt;
  if (!hasId) return std::nullopt;
  if (method == QLatin1String("ping")) return rpcResult(id, {});
  if (method == QLatin1String("tools/list")) return rpcResult(id, toolsList());
  if (method == QLatin1String("tools/call")) {
    QJsonObject perr;
    const QJsonObject res = toolsCall(params, id, &perr);
    if (!perr.isEmpty()) {
      perr.insert(q("id"), id);
      return perr;
    }
    return rpcResult(id, res);
  }
  if (method == QLatin1String("resources/list")) {
    QJsonArray res{QJsonObject{{q("uri"), q("splitframe://project")}, {q("name"), q("project")}, {q("title"), q("Project info")}, {q("description"), q("Name, path, canvas, fps, counts, undo state")}, {q("mimeType"), q("application/json")}},
                   QJsonObject{{q("uri"), q("splitframe://timeline")}, {q("name"), q("timeline")}, {q("title"), q("Timeline")}, {q("description"), q("Tracks, items and markers (summary; add ?mode=detail for full item JSON)")}, {q("mimeType"), q("application/json")}},
                   QJsonObject{{q("uri"), q("splitframe://assets")}, {q("name"), q("assets")}, {q("title"), q("Media assets")}, {q("mimeType"), q("application/json")}}};
    return rpcResult(id, {{q("resources"), res}});
  }
  if (method == QLatin1String("resources/templates/list")) {
    return rpcResult(id, {{q("resourceTemplates"), QJsonArray{QJsonObject{{q("uriTemplate"), q("splitframe://frame/{frame}")}, {q("name"), q("frame")}, {q("title"), q("Rendered frame")},
                                                                         {q("description"), q("The timeline frame rendered as PNG (max 640 px)")}, {q("mimeType"), q("image/png")}}}}});
  }
  if (method == QLatin1String("resources/read")) {
    QJsonObject perr;
    const QJsonObject res = resourcesRead(params.value(q("uri")).toString(), &perr);
    if (!perr.isEmpty()) {
      perr.insert(q("id"), id);
      return perr;
    }
    return rpcResult(id, res);
  }
  if (method == QLatin1String("resources/subscribe") || method == QLatin1String("resources/unsubscribe")) {
    const QString uri = params.value(q("uri")).toString();
    if (uri.isEmpty()) return fail(-32602, QStringLiteral("uri required"));
    if (method == QLatin1String("resources/subscribe")) subscribed_.insert(uri);
    else subscribed_.erase(uri);
    return rpcResult(id, {});
  }
  if (method == QLatin1String("logging/setLevel")) {
    logging_ = params.value(q("level")).toString() != QLatin1String("emergency") && !params.value(q("level")).toString().isEmpty();
    return rpcResult(id, {});
  }
  return fail(-32601, QStringLiteral("Method not found: %1").arg(method));
}

QByteArray McpServer::handleText(const QByteArray& line) {
  QJsonParseError pe;
  const QJsonDocument doc = QJsonDocument::fromJson(line, &pe);
  if (pe.error != QJsonParseError::NoError || doc.isNull()) return QJsonDocument(JsonRpc::errorResponse(QJsonValue::Null, -32700, QStringLiteral("Parse error: %1").arg(pe.errorString()))).toJson(QJsonDocument::Compact);
  if (doc.isArray()) {
    QJsonArray out;
    for (const QJsonValue& v : doc.array())
      if (auto r = handle(v.toObject())) out.append(*r);
    return out.isEmpty() ? QByteArray() : QJsonDocument(out).toJson(QJsonDocument::Compact);
  }
  const auto r = handle(doc.object());
  return r ? QJsonDocument(*r).toJson(QJsonDocument::Compact) : QByteArray();
}

} // namespace sf::engine
