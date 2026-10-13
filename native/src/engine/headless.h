#pragma once

// Entry points that run the engine until stdin closes / the process is stopped. Need a QCoreApplication (a QGuiApplication
// when frames are rendered) to exist; shared by splitframe-cli and `splitframe.exe --mcp`.

#include <QString>

#include <functional>
#include <memory>

namespace sf::engine {

class Engine;

// Runs right after the headless entry points create their Engine (the AI commands register themselves here: sf_engine does not
// link them). The returned object is kept alive until the engine is destroyed.
using EngineSetup = std::function<std::shared_ptr<void>(Engine&)>;

// MCP server on stdio (see mcp.h). `project` (optional) is opened first. Exit code 0 on clean EOF, 4 when the project can't be opened.
int runMcp(const QString& project, const EngineSetup& setup = {});

struct ServeOptions {
  QString project;   // optional project to open
  quint16 port = 0;  // 0: any free port
  QString tokenFile; // default: ApiServer::defaultTokenFile()
  bool stdio = false; // JSON-RPC over stdio instead of HTTP
  EngineSetup setup;  // optional: registers extra commands
};
// Local API: HTTP (POST /rpc, GET /events, bearer token) or, with stdio, newline-delimited JSON-RPC on stdin/stdout.
// HTTP mode prints one line to stdout once listening: "listening http://127.0.0.1:<port> token-file=<path>".
int runServe(const ServeOptions& options);

} // namespace sf::engine
