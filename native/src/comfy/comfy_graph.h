#pragma once

// ComfyUI workflow graph: API-format JSON serialization, in-memory representation, and atomic batch edits.

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include <map>
#include <optional>
#include <variant>

namespace sf::comfy {

struct Link {
  QString fromNode;
  int outputIndex = 0;
  bool operator==(const Link&) const = default;
};

using InputValue = std::variant<QJsonValue, Link>; // literal or link

struct GraphNode {
  QString classType;
  QString title;                        // _meta.title, may be empty
  std::map<QString, InputValue> inputs; // sorted by name for deterministic output
};

class Graph {
public:
  static std::optional<Graph> fromApiJson(const QJsonObject& o, QString* error = nullptr); // error on: non-object node, missing class_type, malformed link
  QJsonObject toApiJson() const;               // exact inverse; _meta.title only when title non-empty
  const std::map<QString, GraphNode>& nodes() const { return nodes_; }
  const GraphNode* node(const QString& id) const;
  QString nextId() const;                      // smallest integer > every numeric id, as a string ("1" for an empty graph)

  // Ops. Each returns an empty string on success, or an error message and leaves the graph unchanged.
  QString addNode(const QString& classType, const QString& id, const QString& title = {}); // id must be new and non-empty
  QString removeNode(const QString& id);        // also erases every input in other nodes that links to it
  QString setInput(const QString& id, const QString& input, const QJsonValue& literal); // literal must not be an array of size 2 whose first item is a string (that is a link)
  QString connect(const QString& fromId, int outputIndex, const QString& toId, const QString& input); // both nodes must exist; outputIndex >= 0
  QString disconnect(const QString& toId, const QString& input); // input must currently be a link
  QString clearInput(const QString& id, const QString& input);

private:
  std::map<QString, GraphNode> nodes_;
};

// Batch of ops as JSON, applied atomically to a copy; on any error the original is untouched and the error names the op index.
// Op shapes:
//  {"op":"add","classType":"KSampler","id":"10"?, "title":"..."?}   (id defaults to nextId(); the resulting id is returned in createdIds)
//  {"op":"remove","id":"10"}
//  {"op":"set","id":"10","input":"steps","value":30}
//  {"op":"connect","from":"4","output":0,"to":"10","input":"model"}
//  {"op":"disconnect","id":"10","input":"model"}
//  {"op":"clear","id":"10","input":"steps"}
// "from"/"to"/"id" may be the string "$N" meaning the id created by op index N of this batch (N must be an earlier "add").
struct OpResult { bool ok = false; QString error; Graph graph; QStringList createdIds; };
OpResult applyOps(const Graph& g, const QJsonArray& ops);

} // namespace sf::comfy
