#pragma once

// ComfyUI node schemas parsed from object_info: input specs, output specs and tool schema generation.

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include <optional>
#include <unordered_map>
#include <vector>

namespace sf::comfy {

enum class InputKind { Link, Int, Float, String, Boolean, Combo };

struct InputSpec {
  QString name;
  bool required = true;      // from "required" (true) or "optional" (false); skip "hidden"
  InputKind kind = InputKind::Link;
  QStringList linkTypes;     // Link only: split on ',' and trimmed; {"*"} for wildcard
  QStringList options;       // Combo only
  std::optional<double> min, max, step;
  QJsonValue defaultValue = QJsonValue::Undefined;   // Undefined when absent
  bool multiline = false;    // String only
  QJsonObject raw;           // the options object as given
};

struct OutputSpec { QString type; QString name; bool isList = false; };

struct NodeSchema {
  QString classType, displayName, category, description;
  bool outputNode = false;
  std::vector<InputSpec> inputs;   // order: input_order.required then input_order.optional; if input_order is missing, JSON key order
  std::vector<OutputSpec> outputs;
  const InputSpec* input(const QString& name) const;
};

class SchemaSet {
public:
  static SchemaSet parse(const QJsonObject& objectInfo, QStringList* warnings = nullptr); // skips nodes it can't parse and adds a warning per node
  const NodeSchema* find(const QString& classType) const;
  QStringList classTypes() const; // sorted
  int size() const;
  bool empty() const;
private:
  std::unordered_map<QString, NodeSchema> nodes_; // QString hashing: use std::hash<QString> from Qt 6
};

// JSON Schema (draft-07 subset) for setting this node's literal inputs, for agent tools: {"type":"object","properties":{...},"required":[...]}.
// Int: integer + min/max; Float: number + min/max; String: string; Boolean: boolean; Combo: string + enum. Link inputs are left out
// (links are made with connect ops). "required" lists required literal inputs that have no default.
QJsonObject toolSchema(const NodeSchema& n);

} // namespace sf::comfy
