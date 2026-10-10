#pragma once

// Newline-delimited messages over the process's stdin / stdout (MCP stdio and the JSON-RPC stdio mode).
// Reads on a helper thread (raw handle reads, so it also works in the GUI-subsystem splitframe.exe when the
// parent redirected the pipes) and delivers each line on the owner's thread; writes are serialised.

#include <QByteArray>
#include <QObject>

#include <atomic>
#include <memory>

namespace sf::engine {

class StdioTransport : public QObject {
  Q_OBJECT

public:
  explicit StdioTransport(QObject* parent = nullptr);
  ~StdioTransport() override;

  void start(); // begins reading
  // Writes `message` (must not contain a raw newline) followed by "\n".
  void send(const QByteArray& message);

signals:
  void lineReceived(const QByteArray& line);
  void closed(); // stdin reached end of file

private:
  struct Shared;
  std::shared_ptr<Shared> shared_;
};

} // namespace sf::engine
