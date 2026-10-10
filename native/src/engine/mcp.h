#pragma once

// Model Context Protocol server over the engine. Newline-delimited JSON-RPC 2.0 on stdio (the MCP stdio transport):
//  - initialize: protocolVersion negotiation (2025-11-25, 2025-06-18, 2025-03-26, 2024-11-05: the client's version when we
//    support it, else our latest), capabilities {tools, resources{subscribe}, logging}, serverInfo, instructions
//  - tools/list: every engine command, name with "." written "_" (clients restrict tool names to [a-zA-Z0-9_-]), the
//    command's JSON schema as inputSchema, annotations.readOnlyHint from `mutates`, _meta["splitframe/domain"]
//  - tools/call: runs the command; the result is a text content block (JSON) plus structuredContent; failures (bad
//    arguments, engine errors) are results with isError=true; an unknown tool is a protocol error (-32602). A frame.render
//    result with inline=true also carries an image content block. progressToken on export.start gets notifications/progress.
//  - resources/list, resources/templates/list, resources/read, resources/subscribe / unsubscribe:
//    splitframe://project, splitframe://timeline, splitframe://assets (JSON), splitframe://frame/{frame} (PNG blob);
//    notifications/resources/updated for subscribed URIs when the document changes
//  - logging/setLevel: engine events are then forwarded as notifications/message
//  - ping

#include "engine/engine.h"

#include <QObject>

#include <functional>
#include <optional>
#include <set>

namespace sf::engine {

class McpServer : public QObject {
  Q_OBJECT

public:
  using Sender = std::function<void(const QJsonObject&)>;
  McpServer(Engine& engine, Sender send, QObject* parent = nullptr);

  // One incoming message (request, notification or batch handled by the caller). The response for a request, nullopt otherwise.
  std::optional<QJsonObject> handle(const QJsonObject& message);
  // Convenience for transports: text in, text out ("" when there is nothing to answer).
  QByteArray handleText(const QByteArray& line);

  static QString toolName(const QString& commandName); // "timeline.get" -> "timeline_get"
  static QStringList supportedVersions();

private:
  QJsonObject toolsList() const;
  QJsonObject toolsCall(const QJsonObject& params, const QJsonValue& id, QJsonObject* protocolError);
  QJsonObject resourcesRead(const QString& uri, QJsonObject* protocolError);
  void onEvent(const QString& name, const QJsonObject& data);
  void notify(const QString& method, const QJsonObject& params);

  Engine& engine_;
  Sender send_;
  std::set<QString> subscribed_;
  std::map<QString, QJsonValue> progressTokens_; // export jobId -> progressToken
  bool logging_ = false;
  QString negotiated_;
};

} // namespace sf::engine
