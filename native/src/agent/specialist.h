#pragma once

// The specialist agent: runs in its own context (separate memory and prompt),
// holds strict domain ownership over engine commands, and accesses other domains
// exclusively via the Peer Bus.

#include "agent/llm_client.h"
#include "agent/peer_bus.h"
#include "agent/types.h"
#include "engine/engine.h"
#include "engine/result.h"

#include <QJsonObject>
#include <QString>

#include <memory>
#include <vector>

namespace sf::agent {

class Specialist : public QObject {
  Q_OBJECT

public:
  Specialist(SpecialistKind kind,
             engine::Engine* engine,
             PeerBus* bus,
             std::shared_ptr<LlmClient> llm = nullptr,
             QObject* parent = nullptr);
  ~Specialist() override = default;

  SpecialistKind kind() const { return kind_; }
  QString name() const { return specialistName(kind_); }
  QString domain() const;
  int slotId() const { return slotId_; }
  void setSlotId(int id) { slotId_ = id; }

  void setLlmClient(std::shared_ptr<LlmClient> llm) { llm_ = std::move(llm); }
  std::shared_ptr<LlmClient> llmClient() const { return llm_; }

  QString systemPrompt() const;

  const std::vector<ChatMessage>& history() const { return history_; }
  void clearHistory() { history_.clear(); }
  void addMessage(ChatMessage msg) { history_.push_back(std::move(msg)); }

  // Produces the specialist's scoped view of the project state.
  QJsonObject scopedViewJson(const QString& sessionId = {}) const;

  // Tools available to this specialist: its domain + engine commands.
  QJsonArray availableTools() const;

  // Executes an engine command with strict domain check and origin tag.
  engine::Result executeCommand(const QString& cmdName,
                                QJsonObject args = {},
                                const QString& sessionId = {});

  // Sends a typed peer request across the Peer Bus to another specialist.
  PeerResponse sendPeerRequest(SpecialistKind to,
                               const QString& kind,
                               const QJsonObject& args = {},
                               const QJsonObject& constraints = {},
                               const QString& reason = {},
                               bool await = true);

  // Handles an incoming peer request from another specialist.
  virtual PeerResponse handlePeerRequest(const PeerRequest& req, const QString& sessionId = {});

  // Executes a full user turn directed to this specialist.
  virtual QJsonObject processTurn(const QString& userMessage, const QString& sessionId = {});

private:
  SpecialistKind kind_;
  engine::Engine* engine_ = nullptr;
  PeerBus* bus_ = nullptr;
  std::shared_ptr<LlmClient> llm_;
  int slotId_ = -1;
  std::vector<ChatMessage> history_;
};

} // namespace sf::agent
