#pragma once

// ComfyUI graph validator: schema conformance, link type checking, cycle detection and output node presence.

#include "comfy/comfy_graph.h"
#include "comfy/comfy_schema.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <vector>

namespace sf::comfy {

struct Issue {
  QString nodeId;
  QString input;
  QString code;
  QString message;
};

// codes (exact strings): unknown_class, unknown_input, missing_required, bad_link, type_mismatch, bad_enum, out_of_range,
// bad_literal_type, cycle, no_output_node
std::vector<Issue> validate(const Graph& g, const SchemaSet& s);
QJsonArray issuesToJson(const std::vector<Issue>& issues);

} // namespace sf::comfy
