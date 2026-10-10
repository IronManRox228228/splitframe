#pragma once

// Peer bus: code-level communication channel connecting the three specialist agents.
// Enforces separate contexts (no transcript dumping), typed requests and responses,
// recursion depth limits (<= 3), cycle detection, and records an auditable team thread.

#include "agent/types.h"

#include <QDateTime>
#include <QObject>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace sf::agent {

using PeerHandler = std::function<PeerResponse(const PeerRequest&)>;

class PeerBus : public QObject {
  Q_OBJECT

public:
  static constexpr int kMaxChainDepth = 3;

  explicit PeerBus(QObject* parent = nullptr);
  ~PeerBus() override = default;

  // Dispatch a typed request to the target specialist.
  PeerResponse send(PeerRequest req);

  // Register a handler for a specific request kind on a specialist.
  void registerHandler(SpecialistKind to, const QString& kind, PeerHandler handler);

  // Register a fallback handler for any unhandled request kind on a specialist.
  void setDefaultHandler(SpecialistKind to, PeerHandler handler);

  // Inter-agent audit log (the team thread).
  std::vector<PeerLogEntry> teamThread() const;
  QJsonArray teamThreadJson() const;
  void clearLog();

signals:
  void messageLogged(const PeerLogEntry& entry);

private:
  mutable std::recursive_mutex mutex_;
  std::map<std::pair<SpecialistKind, QString>, PeerHandler> handlers_;
  std::map<SpecialistKind, PeerHandler> defaultHandlers_;
  std::vector<SpecialistKind> activeStack_;
  std::vector<PeerLogEntry> log_;
};

} // namespace sf::agent
