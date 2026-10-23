#pragma once

// Everything the AI commands need, owned next to an Engine and registered into it: the agent harness (agent.*), the copilot
// engines (copilot.*, branch.*) and the footage index (index.*). One instance per Engine; every entry point (the GUI's engine,
// splitframe-cli, the MCP server, the local API) creates it the same way, so the command names are identical everywhere.
//
// - Nothing starts until a command asks: the footage catalog is opened on the first index.* call (a per-project file next to the
//   project, a temp file for an unsaved one), the branch manager binds to the session's project on first use.
// - agent.turn answers `unavailable` (never hangs) when the configured LLM endpoint can't be reached.

#include "agent/agent_harness.h"
#include "agent/llm_config.h"
#include "copilot/branch_manager.h"
#include "copilot/lint_engine.h"
#include "copilot/review_engine.h"
#include "copilot/suggestion_engine.h"
#include "engine/engine.h"
#include "engine/headless.h"
#include "index/index_manager.h"

#include <QObject>

#include <memory>
#include <set>

namespace sf::ai {

struct AiOptions {
  agent::LlmConfig llm = agent::LlmConfig::fromEnvironment();
  QString catalogPath; // fixed catalog database instead of the per-project one (tests)
};

class AiServices : public QObject {
  Q_OBJECT

public:
  explicit AiServices(engine::Engine& engine, const AiOptions& options = {}, QObject* parent = nullptr);
  ~AiServices() override;

  agent::AgentHarness* harness() { return harness_.get(); }
  index::IndexManager* indexManager() { return &index_; }
  copilot::BranchManager* branchManager() { return &branches_; }
  const agent::LlmConfig& llmConfig() const { return options_.llm; }

public:
  // Replaces the model (tests, later a smarter client). The harness and the footage index use it from then on.
  void setLlmClient(std::shared_ptr<agent::LlmClient> client);

  // <project dir>/<project base name>.splitframe-index.sqlite; a temp file for a project without a file.
  static QString catalogPathFor(const QString& projectPath, const QString& sessionId);

private:
  void guardAgentTurn();
  void lazyBindings();
  void registerProviderCommands();
  engine::Session* sessionFor(engine::CallContext& ctx, const QJsonObject& args);
  void bindBranches(engine::Session* s);
  void ensureCatalog(engine::Session* s, bool create);

  engine::Engine& engine_;
  AiOptions options_;
  QString configError_;
  std::shared_ptr<agent::RecordingLlmClient> llm_;
  std::unique_ptr<agent::AgentHarness> harness_;
  index::IndexManager index_;
  copilot::LintEngine lint_;
  copilot::SuggestionEngine suggest_;
  copilot::BranchManager branches_;
  copilot::ReviewEngine review_;
  editor::Project* branchProject_ = nullptr;
  std::set<editor::Project*> watched_;
  QString openCatalog_;
  bool catalogIsTemp_ = false;
};

// For runMcp / runServe, which build their own Engine: registers the AI commands and keeps them alive with it.
engine::EngineSetup engineSetup(const AiOptions& options = {});

} // namespace sf::ai
