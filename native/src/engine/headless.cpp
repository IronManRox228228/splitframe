#include "engine/headless.h"

#include "engine/api_server.h"
#include "engine/engine.h"
#include "engine/mcp.h"
#include "engine/rpc.h"
#include "engine/stdio.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>

#include <cstdio>

namespace sf::engine {

namespace {

bool openInitialProject(Engine& engine, const QString& path) {
  if (path.isEmpty()) {
    engine.createSession();
    return true;
  }
  const Result r = engine.call(QStringLiteral("project.open"), {{QStringLiteral("path"), path}});
  if (!r.ok) std::fprintf(stderr, "error: %s\n", qPrintable(r.error.message));
  return r.ok;
}

} // namespace

int runMcp(const QString& project) {
  Engine engine;
  if (!openInitialProject(engine, project)) return 4;
  StdioTransport io;
  McpServer mcp(engine, [&io](const QJsonObject& o) { io.send(QJsonDocument(o).toJson(QJsonDocument::Compact)); });
  QObject::connect(&io, &StdioTransport::lineReceived, &engine, [&](const QByteArray& line) {
    const QByteArray out = mcp.handleText(line);
    if (!out.isEmpty()) io.send(out);
  });
  QObject::connect(&io, &StdioTransport::closed, &engine, [] { QCoreApplication::quit(); });
  io.start();
  return QCoreApplication::exec();
}

int runServe(const ServeOptions& o) {
  Engine engine;
  if (!openInitialProject(engine, o.project)) return 4;
  if (o.stdio) {
    JsonRpc rpc(engine);
    StdioTransport io;
    QObject::connect(&io, &StdioTransport::lineReceived, &engine, [&](const QByteArray& line) {
      const QByteArray out = rpc.handleText(line);
      if (!out.isEmpty()) io.send(out);
    });
    // events are notifications: {"jsonrpc":"2.0","method":"event","params":{"event":name,"data":{...},"seq":n}}
    QObject::connect(&engine, &Engine::event, &engine, [&io](const QString& name, const QJsonObject& data, qint64 seq) {
      io.send(QJsonDocument(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("method"), QStringLiteral("event")},
                                        {QStringLiteral("params"), QJsonObject{{QStringLiteral("event"), name}, {QStringLiteral("data"), data}, {QStringLiteral("seq"), static_cast<double>(seq)}}}})
                  .toJson(QJsonDocument::Compact));
    });
    QObject::connect(&io, &StdioTransport::closed, &engine, [] { QCoreApplication::quit(); });
    io.start();
    return QCoreApplication::exec();
  }
  ApiServer::Options so;
  so.port = o.port;
  so.tokenFile = o.tokenFile;
  ApiServer server(engine, so);
  QString err;
  if (!server.start(&err)) {
    std::fprintf(stderr, "error: %s\n", qPrintable(err));
    return 1;
  }
  std::printf("listening http://127.0.0.1:%u token-file=%s\n", static_cast<unsigned>(server.port()), qPrintable(QDir::toNativeSeparators(server.tokenFile())));
  std::fflush(stdout);
  return QCoreApplication::exec();
}

} // namespace sf::engine
