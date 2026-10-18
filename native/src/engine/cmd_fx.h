#pragma once

// Effect operations builder shared with variations.sheet.

#include "engine/cmd_util.h"

namespace sf::engine {

Result buildEffectAddOps(CallContext& c, const QString& itemId, const QString& effectId, const QJsonObject& params, QJsonArray* ops, int index = -1);

} // namespace sf::engine
