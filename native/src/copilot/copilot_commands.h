#pragma once

#include "copilot/branch_manager.h"
#include "copilot/lint_engine.h"
#include "copilot/review_engine.h"
#include "copilot/suggestion_engine.h"
#include "engine/engine.h"
#include "index/catalog.h"

namespace sf::copilot {

void registerCopilotCommands(engine::Engine* engine,
                             LintEngine* lintEngine,
                             SuggestionEngine* suggestionEngine,
                             BranchManager* branchManager,
                             ReviewEngine* reviewEngine,
                             index::Catalog* catalog = nullptr);

} // namespace sf::copilot
