// ComfyUI graph validator: schema conformance, link type checking, cycle detection and output node presence.

#include "comfy/comfy_validate.h"

#include <cmath>
#include <functional>
#include <map>

namespace sf::comfy {

std::vector<Issue> validate(const Graph& g, const SchemaSet& s) {
  std::vector<Issue> issues;

  // 1 to 5: check each node in the graph
  for (const auto& [nodeId, node] : g.nodes()) {
    const NodeSchema* schema = s.find(node.classType);
    if (!schema) {
      issues.push_back(Issue{nodeId, QString(), QStringLiteral("unknown_class"),
        QStringLiteral("Unknown node class '%1'").arg(node.classType)});
      continue;
    }

    // 2. unknown_input
    for (const auto& [inName, _] : node.inputs) {
      if (!schema->input(inName)) {
        issues.push_back(Issue{nodeId, inName, QStringLiteral("unknown_input"),
          QStringLiteral("%1.%2: unknown input").arg(schema->classType, inName)});
      }
    }

    // 3. missing_required
    for (const auto& ispec : schema->inputs) {
      if (ispec.required) {
        if (!node.inputs.count(ispec.name)) {
          // This counts only if it is a Link kind, or a literal kind with no defaultValue.
          if (ispec.kind == InputKind::Link || ispec.defaultValue.isUndefined()) {
            issues.push_back(Issue{nodeId, ispec.name, QStringLiteral("missing_required"),
              QStringLiteral("%1.%2: required input is missing").arg(schema->classType, ispec.name)});
          }
        }
      }
    }

    // 4 & 5: check values
    for (const auto& [inName, inVal] : node.inputs) {
      const InputSpec* ispec = schema->input(inName);
      if (!ispec) continue; // already reported in unknown_input

      if (std::holds_alternative<Link>(inVal)) {
        const Link& link = std::get<Link>(inVal);
        if (ispec->kind != InputKind::Link) {
          issues.push_back(Issue{nodeId, inName, QStringLiteral("bad_link"),
            QStringLiteral("%1.%2: input is not a link input").arg(schema->classType, inName)});
          continue;
        }

        const GraphNode* srcNode = g.node(link.fromNode);
        if (!srcNode) {
          issues.push_back(Issue{nodeId, inName, QStringLiteral("bad_link"),
            QStringLiteral("%1.%2: source node '%3' not found").arg(schema->classType, inName, link.fromNode)});
          continue;
        }

        const NodeSchema* srcSchema = s.find(srcNode->classType);
        if (!srcSchema) {
          issues.push_back(Issue{nodeId, inName, QStringLiteral("bad_link"),
            QStringLiteral("%1.%2: source node '%3' has unknown class '%4'").arg(schema->classType, inName, link.fromNode, srcNode->classType)});
          continue;
        }

        if (link.outputIndex < 0 || link.outputIndex >= static_cast<int>(srcSchema->outputs.size())) {
          issues.push_back(Issue{nodeId, inName, QStringLiteral("bad_link"),
            QStringLiteral("%1.%2: source node '%3' has no output %4").arg(schema->classType, inName, link.fromNode).arg(link.outputIndex)});
          continue;
        }

        const QString& outType = srcSchema->outputs[link.outputIndex].type;
        bool match = false;
        if (outType == QLatin1String("*") || ispec->linkTypes.contains(QLatin1String("*"))) {
          match = true;
        } else {
          for (const QString& lt : ispec->linkTypes) {
            if (lt.compare(outType, Qt::CaseInsensitive) == 0) {
              match = true;
              break;
            }
          }
        }

        if (!match) {
          issues.push_back(Issue{nodeId, inName, QStringLiteral("type_mismatch"),
            QStringLiteral("%1.%2 expects %3, got %4 from node %5 output %6")
              .arg(schema->classType, inName, ispec->linkTypes.join(QLatin1Char('/')), outType, link.fromNode).arg(link.outputIndex)});
        }
      } else {
        const QJsonValue& lit = std::get<QJsonValue>(inVal);
        if (ispec->kind == InputKind::Link) {
          issues.push_back(Issue{nodeId, inName, QStringLiteral("bad_literal_type"),
            QStringLiteral("%1.%2: expected link, got literal value").arg(schema->classType, inName)});
          continue;
        }

        switch (ispec->kind) {
        case InputKind::Int: {
          if (!lit.isDouble() || std::floor(lit.toDouble()) != lit.toDouble()) {
            issues.push_back(Issue{nodeId, inName, QStringLiteral("bad_literal_type"),
              QStringLiteral("%1.%2: expected integer, got %3").arg(schema->classType, inName, lit.isString() ? QStringLiteral("string") : QStringLiteral("non-integer"))});
          } else {
            const double val = lit.toDouble();
            if ((ispec->min && val < *ispec->min) || (ispec->max && val > *ispec->max)) {
              issues.push_back(Issue{nodeId, inName, QStringLiteral("out_of_range"),
                QStringLiteral("%1.%2: %3 is out of range [%4, %5]").arg(schema->classType, inName).arg(val).arg(ispec->min.value_or(0)).arg(ispec->max.value_or(0))});
            }
          }
          break;
        }
        case InputKind::Float: {
          if (!lit.isDouble()) {
            issues.push_back(Issue{nodeId, inName, QStringLiteral("bad_literal_type"),
              QStringLiteral("%1.%2: expected number, got non-number").arg(schema->classType, inName)});
          } else {
            const double val = lit.toDouble();
            if ((ispec->min && val < *ispec->min) || (ispec->max && val > *ispec->max)) {
              issues.push_back(Issue{nodeId, inName, QStringLiteral("out_of_range"),
                QStringLiteral("%1.%2: %3 is out of range [%4, %5]").arg(schema->classType, inName).arg(val).arg(ispec->min.value_or(0)).arg(ispec->max.value_or(0))});
            }
          }
          break;
        }
        case InputKind::String: {
          if (!lit.isString()) {
            issues.push_back(Issue{nodeId, inName, QStringLiteral("bad_literal_type"),
              QStringLiteral("%1.%2: expected string, got non-string").arg(schema->classType, inName)});
          }
          break;
        }
        case InputKind::Boolean: {
          if (!lit.isBool()) {
            issues.push_back(Issue{nodeId, inName, QStringLiteral("bad_literal_type"),
              QStringLiteral("%1.%2: expected boolean, got non-boolean").arg(schema->classType, inName)});
          }
          break;
        }
        case InputKind::Combo: {
          if (!lit.isString()) {
            issues.push_back(Issue{nodeId, inName, QStringLiteral("bad_literal_type"),
              QStringLiteral("%1.%2: expected string for combo, got non-string").arg(schema->classType, inName)});
          } else {
            const QString strVal = lit.toString();
            if (!ispec->options.contains(strVal)) {
              QStringList opts = ispec->options;
              if (opts.size() > 10) opts = opts.mid(0, 10);
              issues.push_back(Issue{nodeId, inName, QStringLiteral("bad_enum"),
                QStringLiteral("%1.%2: '%3' is not an option (%4)").arg(schema->classType, inName, strVal, opts.join(QStringLiteral(", ")))});
            }
          }
          break;
        }
        case InputKind::Link:
          break;
        }
      }
    }
  }

  // 6. cycle
  enum class VisitState { Unvisited, Visiting, Visited };
  std::map<QString, VisitState> visitState;
  QString cycleNode;
  bool cycleFound = false;

  std::function<void(const QString&)> dfs = [&](const QString& u) {
    visitState[u] = VisitState::Visiting;
    const GraphNode* gn = g.node(u);
    if (gn) {
      for (const auto& [_, inVal] : gn->inputs) {
        if (std::holds_alternative<Link>(inVal)) {
          const QString& v = std::get<Link>(inVal).fromNode;
          if (visitState[v] == VisitState::Visiting) {
            cycleFound = true;
            cycleNode = u;
            return;
          }
          if (visitState[v] == VisitState::Unvisited && g.node(v) != nullptr) {
            dfs(v);
            if (cycleFound) return;
          }
        }
      }
    }
    visitState[u] = VisitState::Visited;
  };

  for (const auto& [nodeId, _] : g.nodes()) {
    if (visitState[nodeId] == VisitState::Unvisited) {
      dfs(nodeId);
      if (cycleFound) break;
    }
  }

  if (cycleFound) {
    issues.push_back(Issue{cycleNode, QString(), QStringLiteral("cycle"),
      QStringLiteral("Cycle detected involving node '%1'").arg(cycleNode)});
  }

  // 7. no_output_node
  bool hasOutput = false;
  for (const auto& [_, gn] : g.nodes()) {
    const NodeSchema* ns = s.find(gn.classType);
    if (ns && ns->outputNode) {
      hasOutput = true;
      break;
    }
  }
  if (!hasOutput) {
    issues.push_back(Issue{QString(), QString(), QStringLiteral("no_output_node"),
      QStringLiteral("Graph contains no output node")});
  }

  return issues;
}

QJsonArray issuesToJson(const std::vector<Issue>& issues) {
  QJsonArray arr;
  for (const auto& is : issues) {
    arr.append(QJsonObject{
      {QStringLiteral("nodeId"), is.nodeId},
      {QStringLiteral("input"), is.input},
      {QStringLiteral("code"), is.code},
      {QStringLiteral("message"), is.message}
    });
  }
  return arr;
}

} // namespace sf::comfy
