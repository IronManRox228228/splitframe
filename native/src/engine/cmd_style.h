#pragma once

// Look operations builder shared with variations.sheet.

#include "engine/cmd_util.h"

namespace sf::engine {

Result buildLookOps(CallContext& c, const QJsonObject& a, QJsonArray* ops, QJsonObject* info);

} // namespace sf::engine
