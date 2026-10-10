#pragma once

#include "engine/engine.h"
#include "index/index_manager.h"

namespace sf::index {

// Registers index commands into the headless engine command surface.
void registerIndexCommands(engine::Engine* engine, IndexManager* manager);

} // namespace sf::index
