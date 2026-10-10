#pragma once

// Local HTTP API: JSON-RPC 2.0 over POST /rpc and a Server-Sent Events stream on GET /events, on 127.0.0.1 only.
//
// Hand-written minimal HTTP/1.1 (Qt HttpServer is not part of the install): Content-Length bodies, keep-alive, no chunked
// uploads (411/501), header block <= 16 KB, body <= maxBody, idle connections dropped after idleMs, at most 64 connections.
//
// Security (a malicious web page can reach 127.0.0.1 too):
//  - every request needs `Authorization: Bearer <token>`; the token is random per server start and written to a per-user
//    file (QStandardPaths::AppConfigLocation, "SplitFrame" app: api-token). Compared in constant time.
//  - the peer must be a loopback address; the Host header must name 127.0.0.1 / localhost / [::1] (DNS rebinding);
//    an Origin header, when present, must be this server's own http://127.0.0.1:<port> or http://localhost:<port>.
//  - no CORS headers are ever sent.
// SSE: GET /events[?events=ops.applied,export.progress] streams every engine event as `id: <seq>\nevent: <name>\ndata: <json>`.

#include "engine/engine.h"
#include "engine/rpc.h"

#include <QHash>
#include <QTcpServer>
#include <QTimer>

namespace sf::engine {

class ApiServer : public QObject {
  Q_OBJECT

public:
  struct Options {
    quint16 port = 0;        // 0: any free port
    QString tokenFile;       // default: defaultTokenFile()
    QString endpointFile;    // default: next to the token file ("api.json": {host, port, pid}); written on start, removed on stop
    QString token;           // default: fresh random (written to tokenFile)
    qint64 maxBody = 8 << 20;
    int idleMs = 30000;
    int maxConnections = 64;
  };

  ApiServer(Engine& engine, Options options, QObject* parent = nullptr);
  ~ApiServer() override;

  bool start(QString* error = nullptr);
  void stop();
  bool running() const { return server_.isListening(); }
  quint16 port() const { return server_.serverPort(); }
  const QString& token() const { return token_; }
  const QString& tokenFile() const { return options_.tokenFile; }

  static QString defaultTokenFile();
  static QString generateToken();

private:
  class Connection;
  void onNewConnection();
  void broadcast(const QString& name, const QJsonObject& data, qint64 seq);

  Engine& engine_;
  Options options_;
  QString token_;
  QTcpServer server_;
  JsonRpc rpc_;
  QList<Connection*> connections_;
  QTimer heartbeat_;
};

} // namespace sf::engine
