// ComfyUI workflow graph: API-format JSON serialization, in-memory representation, and atomic batch edits.

#include "comfy/comfy_graph.h"

namespace sf::comfy {

std::optional<Graph> Graph::fromApiJson(const QJsonObject& o, QString* error) {
  Graph g;
  for (auto it = o.begin(); it != o.end(); ++it) {
    const QString nodeId = it.key();
    if (!it.value().isObject()) {
      if (error) *error = QStringLiteral("Node '%1' is not an object").arg(nodeId);
      return std::nullopt;
    }
    const QJsonObject nodeObj = it.value().toObject();
    if (!nodeObj.contains(QStringLiteral("class_type")) || !nodeObj.value(QStringLiteral("class_type")).isString()) {
      if (error) *error = QStringLiteral("Node '%1' is missing class_type").arg(nodeId);
      return std::nullopt;
    }

    GraphNode node;
    node.classType = nodeObj.value(QStringLiteral("class_type")).toString();
    if (nodeObj.contains(QStringLiteral("_meta")) && nodeObj.value(QStringLiteral("_meta")).isObject()) {
      node.title = nodeObj.value(QStringLiteral("_meta")).toObject().value(QStringLiteral("title")).toString();
    }

    if (nodeObj.contains(QStringLiteral("inputs")) && nodeObj.value(QStringLiteral("inputs")).isObject()) {
      const QJsonObject inObj = nodeObj.value(QStringLiteral("inputs")).toObject();
      for (auto init = inObj.begin(); init != inObj.end(); ++init) {
        const QString inName = init.key();
        const QJsonValue inVal = init.value();
        if (inVal.isArray()) {
          const QJsonArray arr = inVal.toArray();
          if (arr.size() == 2 && arr.at(0).isString()) {
            if (!arr.at(1).isDouble()) {
              if (error) *error = QStringLiteral("Malformed link in node '%1' input '%2'").arg(nodeId, inName);
              return std::nullopt;
            }
            const int outIdx = arr.at(1).toInt(-1);
            if (outIdx < 0) {
              if (error) *error = QStringLiteral("Malformed link in node '%1' input '%2': negative index").arg(nodeId, inName);
              return std::nullopt;
            }
            node.inputs[inName] = Link{arr.at(0).toString(), outIdx};
          } else {
            node.inputs[inName] = inVal;
          }
        } else {
          node.inputs[inName] = inVal;
        }
      }
    }
    g.nodes_[nodeId] = std::move(node);
  }
  return g;
}

QJsonObject Graph::toApiJson() const {
  QJsonObject root;
  for (const auto& [id, node] : nodes_) {
    QJsonObject nodeObj;
    nodeObj.insert(QStringLiteral("class_type"), node.classType);

    QJsonObject inputsObj;
    for (const auto& [inName, inVal] : node.inputs) {
      if (std::holds_alternative<Link>(inVal)) {
        const Link& l = std::get<Link>(inVal);
        QJsonArray linkArr;
        linkArr.append(l.fromNode);
        linkArr.append(l.outputIndex);
        inputsObj.insert(inName, linkArr);
      } else {
        inputsObj.insert(inName, std::get<QJsonValue>(inVal));
      }
    }
    nodeObj.insert(QStringLiteral("inputs"), inputsObj);

    if (!node.title.isEmpty()) {
      nodeObj.insert(QStringLiteral("_meta"), QJsonObject{{QStringLiteral("title"), node.title}});
    }
    root.insert(id, nodeObj);
  }
  return root;
}

const GraphNode* Graph::node(const QString& id) const {
  const auto it = nodes_.find(id);
  if (it != nodes_.end()) return &it->second;
  return nullptr;
}

QString Graph::nextId() const {
  qint64 maxId = 0;
  for (const auto& [id, _] : nodes_) {
    bool ok = false;
    const qint64 val = id.toLongLong(&ok);
    if (ok && val > maxId) {
      maxId = val;
    }
  }
  return QString::number(maxId + 1);
}

QString Graph::addNode(const QString& classType, const QString& id, const QString& title) {
  if (id.isEmpty()) return QStringLiteral("Node id cannot be empty");
  if (nodes_.find(id) != nodes_.end()) return QStringLiteral("Node '%1' already exists").arg(id);
  nodes_[id] = GraphNode{classType, title, {}};
  return {};
}

QString Graph::removeNode(const QString& id) {
  const auto it = nodes_.find(id);
  if (it == nodes_.end()) return QStringLiteral("Node '%1' not found").arg(id);
  nodes_.erase(it);

  for (auto& [_, n] : nodes_) {
    for (auto init = n.inputs.begin(); init != n.inputs.end(); ) {
      if (std::holds_alternative<Link>(init->second) && std::get<Link>(init->second).fromNode == id) {
        init = n.inputs.erase(init);
      } else {
        ++init;
      }
    }
  }
  return {};
}

QString Graph::setInput(const QString& id, const QString& input, const QJsonValue& literal) {
  const auto it = nodes_.find(id);
  if (it == nodes_.end()) return QStringLiteral("Node '%1' not found").arg(id);
  if (literal.isArray()) {
    const QJsonArray arr = literal.toArray();
    if (arr.size() == 2 && arr.at(0).isString()) {
      return QStringLiteral("Input value looks like a link; use connect instead");
    }
  }
  it->second.inputs[input] = literal;
  return {};
}

QString Graph::connect(const QString& fromId, int outputIndex, const QString& toId, const QString& input) {
  if (outputIndex < 0) return QStringLiteral("outputIndex cannot be negative");
  if (nodes_.find(fromId) == nodes_.end()) return QStringLiteral("Source node '%1' not found").arg(fromId);
  const auto it = nodes_.find(toId);
  if (it == nodes_.end()) return QStringLiteral("Target node '%1' not found").arg(toId);
  it->second.inputs[input] = Link{fromId, outputIndex};
  return {};
}

QString Graph::disconnect(const QString& toId, const QString& input) {
  const auto it = nodes_.find(toId);
  if (it == nodes_.end()) return QStringLiteral("Node '%1' not found").arg(toId);
  const auto init = it->second.inputs.find(input);
  if (init == it->second.inputs.end()) return QStringLiteral("Input '%1' not found on node '%2'").arg(input, toId);
  if (!std::holds_alternative<Link>(init->second)) return QStringLiteral("Input '%1' on node '%2' is not a link").arg(input, toId);
  it->second.inputs.erase(init);
  return {};
}

QString Graph::clearInput(const QString& id, const QString& input) {
  const auto it = nodes_.find(id);
  if (it == nodes_.end()) return QStringLiteral("Node '%1' not found").arg(id);
  it->second.inputs.erase(input);
  return {};
}

OpResult applyOps(const Graph& g, const QJsonArray& ops) {
  Graph copy = g;
  QStringList createdIds;
  std::map<int, QString> opCreatedId;

  for (int i = 0; i < ops.size(); ++i) {
    if (!ops.at(i).isObject()) {
      return OpResult{false, QStringLiteral("op %1: op is not an object").arg(i), g, {}};
    }
    const QJsonObject op = ops.at(i).toObject();
    const QString kind = op.value(QStringLiteral("op")).toString();

    auto resolveId = [&](const QString& rawId, QString* err) -> QString {
      if (rawId.startsWith(QLatin1Char('$'))) {
        bool ok = false;
        const int refIdx = rawId.mid(1).toInt(&ok);
        if (!ok || refIdx < 0 || refIdx >= i || !opCreatedId.count(refIdx)) {
          if (err) *err = QStringLiteral("invalid reference '%1'").arg(rawId);
          return {};
        }
        return opCreatedId[refIdx];
      }
      return rawId;
    };

    QString err;
    if (kind == QStringLiteral("add")) {
      const QString classType = op.value(QStringLiteral("classType")).toString();
      const QString title = op.value(QStringLiteral("title")).toString();
      QString id;
      if (op.contains(QStringLiteral("id"))) {
        id = resolveId(op.value(QStringLiteral("id")).toString(), &err);
      } else {
        id = copy.nextId();
      }
      if (err.isEmpty()) {
        err = copy.addNode(classType, id, title);
        if (err.isEmpty()) {
          opCreatedId[i] = id;
          createdIds.append(id);
        }
      }
    } else if (kind == QStringLiteral("remove")) {
      QString rawId = op.value(QStringLiteral("id")).toString();
      if (rawId.isEmpty()) rawId = op.value(QStringLiteral("nodeId")).toString();
      const QString id = resolveId(rawId, &err);
      if (err.isEmpty()) err = copy.removeNode(id);
    } else if (kind == QStringLiteral("set")) {
      QString rawId = op.value(QStringLiteral("id")).toString();
      if (rawId.isEmpty()) rawId = op.value(QStringLiteral("nodeId")).toString();
      const QString id = resolveId(rawId, &err);
      const QString input = op.value(QStringLiteral("input")).toString();
      const QJsonValue val = op.value(QStringLiteral("value"));
      if (err.isEmpty()) err = copy.setInput(id, input, val);
    } else if (kind == QStringLiteral("connect")) {
      const QString from = resolveId(op.value(QStringLiteral("from")).toString(), &err);
      const int output = op.value(QStringLiteral("output")).toInt(0);
      const QString to = err.isEmpty() ? resolveId(op.value(QStringLiteral("to")).toString(), &err) : QString();
      const QString input = op.value(QStringLiteral("input")).toString();
      if (err.isEmpty()) err = copy.connect(from, output, to, input);
    } else if (kind == QStringLiteral("disconnect")) {
      QString rawId = op.value(QStringLiteral("id")).toString();
      if (rawId.isEmpty()) rawId = op.value(QStringLiteral("to")).toString();
      if (rawId.isEmpty()) rawId = op.value(QStringLiteral("nodeId")).toString();
      const QString id = resolveId(rawId, &err);
      const QString input = op.value(QStringLiteral("input")).toString();
      if (err.isEmpty()) err = copy.disconnect(id, input);
    } else if (kind == QStringLiteral("clear")) {
      QString rawId = op.value(QStringLiteral("id")).toString();
      if (rawId.isEmpty()) rawId = op.value(QStringLiteral("nodeId")).toString();
      const QString id = resolveId(rawId, &err);
      const QString input = op.value(QStringLiteral("input")).toString();
      if (err.isEmpty()) err = copy.clearInput(id, input);
    } else {
      err = QStringLiteral("unknown op kind '%1'").arg(kind);
    }

    if (!err.isEmpty()) {
      return OpResult{false, QStringLiteral("op %1: %2").arg(i).arg(err), g, {}};
    }
  }

  return OpResult{true, {}, std::move(copy), createdIds};
}

} // namespace sf::comfy
