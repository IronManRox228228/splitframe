#pragma once

// The agent harness: coordinates the three specialists (editor, colourist, audio),
// the thin router, the peer bus, and exposes commands into the engine.

#include "agent/llm_client.h"
#include "agent/peer_bus.h"
#include "agent/router.h"
#include "agent/specialist.h"
#include "agent/types.h"
#include "engine/engine.h"

#include <QJsonObject>
#include <QObject>

#include <map>
#include <memory>

namespace sf::agent {

class AgentHarness : public QObject {
  Q_OBJECT

public:
  explicit AgentHarness(engine::Engine* engine,
                        std::shared_ptr<LlmClient> llm = nullptr,
                        QObject* parent = nullptr);
  ~AgentHarness() override = default;

  AgentMode mode() const { return mode_; }
  void setMode(AgentMode m) { mode_ = m; }

  void setLlmClient(std::shared_ptr<LlmClient> llm);
  std::shared_ptr<LlmClient> llmClient() const { return llm_; }

  PeerBus* peerBus() const { return bus_.get(); }
  Router* router() const { return router_.get(); }
  Specialist* specialist(SpecialistKind kind) const;

  // Process a user turn across the router and specialists.
  QJsonObject executeTurn(const QString& userMessage, const QString& sessionId = {});

  // Register agent commands into the engine (agent.turn, agent.mode.get/set, agent.team_thread, etc.)
  void registerCommands(engine::Engine& engine);

signals:
  void turnCompleted(const QJsonObject& turnResult);

private:
  engine::Engine* engine_ = nullptr;
  std::shared_ptr<LlmClient> llm_;
  std::unique_ptr<PeerBus> bus_;
  std::unique_ptr<Router> router_;
  std::map<SpecialistKind, std::unique_ptr<Specialist>> specialists_;
  AgentMode mode_ = AgentMode::Default;
};

} // namespace sf::agent
