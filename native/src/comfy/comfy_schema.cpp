// ComfyUI node schemas parsed from object_info: input specs, output specs and tool schema generation.

#include "comfy/comfy_schema.h"

#include <QSet>

#include <algorithm>

namespace sf::comfy {

namespace {

std::optional<InputSpec> parseInputSpec(const QString& name, bool required, const QJsonValue& val) {
  if (!val.isArray()) return std::nullopt;
  const QJsonArray arr = val.toArray();
  if (arr.isEmpty()) return std::nullopt;

  InputSpec spec;
  spec.name = name;
  spec.required = required;

  const QJsonValue first = arr.at(0);
  if (first.isArray()) {
    // Combo, old form: [[ "a", "b" ], {options}]
    spec.kind = InputKind::Combo;
    for (const auto& item : first.toArray()) {
      spec.options.append(item.toString());
    }
    if (arr.size() > 1 && arr.at(1).isObject()) {
      spec.raw = arr.at(1).toObject();
      if (spec.raw.contains(QStringLiteral("default"))) {
        spec.defaultValue = spec.raw.value(QStringLiteral("default"));
      }
    }
    return spec;
  }

  if (!first.isString()) return std::nullopt;
  const QString typeStr = first.toString();

  if (arr.size() > 1 && arr.at(1).isObject()) {
    spec.raw = arr.at(1).toObject();
    if (spec.raw.contains(QStringLiteral("min"))) {
      spec.min = spec.raw.value(QStringLiteral("min")).toDouble();
    }
    if (spec.raw.contains(QStringLiteral("max"))) {
      spec.max = spec.raw.value(QStringLiteral("max")).toDouble();
    }
    if (spec.raw.contains(QStringLiteral("step"))) {
      spec.step = spec.raw.value(QStringLiteral("step")).toDouble();
    }
    if (spec.raw.contains(QStringLiteral("default"))) {
      spec.defaultValue = spec.raw.value(QStringLiteral("default"));
    }
    if (spec.raw.contains(QStringLiteral("multiline"))) {
      spec.multiline = spec.raw.value(QStringLiteral("multiline")).toBool(false);
    }
  }

  if (typeStr == QStringLiteral("INT")) {
    spec.kind = InputKind::Int;
  } else if (typeStr == QStringLiteral("FLOAT")) {
    spec.kind = InputKind::Float;
  } else if (typeStr == QStringLiteral("STRING")) {
    spec.kind = InputKind::String;
  } else if (typeStr == QStringLiteral("BOOLEAN")) {
    spec.kind = InputKind::Boolean;
  } else if (typeStr == QStringLiteral("COMBO")) {
    spec.kind = InputKind::Combo;
    const QJsonArray optArr = spec.raw.value(QStringLiteral("options")).toArray();
    for (const auto& item : optArr) {
      spec.options.append(item.toString());
    }
  } else {
    spec.kind = InputKind::Link;
    const QStringList parts = typeStr.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& p : parts) {
      const QString trimmed = p.trimmed();
      if (!trimmed.isEmpty()) spec.linkTypes.append(trimmed);
    }
    if (spec.linkTypes.isEmpty() && typeStr == QStringLiteral("*")) {
      spec.linkTypes.append(QStringLiteral("*"));
    }
  }

  return spec;
}

} // namespace

const InputSpec* NodeSchema::input(const QString& name) const {
  for (const auto& in : inputs) {
    if (in.name == name) return &in;
  }
  return nullptr;
}

SchemaSet SchemaSet::parse(const QJsonObject& objectInfo, QStringList* warnings) {
  SchemaSet set;
  for (auto it = objectInfo.begin(); it != objectInfo.end(); ++it) {
    const QString nodeKey = it.key();
    if (!it.value().isObject()) {
      if (warnings) {
        warnings->append(QStringLiteral("Node '%1' value is not an object").arg(nodeKey));
      }
      continue;
    }
    const QJsonObject val = it.value().toObject();

    NodeSchema schema;
    schema.classType = nodeKey;
    schema.displayName = val.value(QStringLiteral("display_name")).toString(nodeKey);
    schema.category = val.value(QStringLiteral("category")).toString();
    schema.description = val.value(QStringLiteral("description")).toString();
    schema.outputNode = val.value(QStringLiteral("output_node")).toBool(false);

    const QJsonObject inputObj = val.value(QStringLiteral("input")).toObject();
    const QJsonObject reqObj = inputObj.value(QStringLiteral("required")).toObject();
    const QJsonObject optObj = inputObj.value(QStringLiteral("optional")).toObject();

    const bool hasInputOrder = val.contains(QStringLiteral("input_order")) && val.value(QStringLiteral("input_order")).isObject();
    if (hasInputOrder) {
      const QJsonObject inputOrderObj = val.value(QStringLiteral("input_order")).toObject();
      const QJsonArray reqOrder = inputOrderObj.value(QStringLiteral("required")).toArray();
      const QJsonArray optOrder = inputOrderObj.value(QStringLiteral("optional")).toArray();
      QSet<QString> seenReq, seenOpt;

      for (const auto& nameVal : reqOrder) {
        const QString name = nameVal.toString();
        if (reqObj.contains(name)) {
          if (auto s = parseInputSpec(name, true, reqObj.value(name))) {
            schema.inputs.push_back(std::move(*s));
            seenReq.insert(name);
          }
        }
      }
      for (auto rit = reqObj.begin(); rit != reqObj.end(); ++rit) {
        if (!seenReq.contains(rit.key())) {
          if (auto s = parseInputSpec(rit.key(), true, rit.value())) {
            schema.inputs.push_back(std::move(*s));
          }
        }
      }
      for (const auto& nameVal : optOrder) {
        const QString name = nameVal.toString();
        if (optObj.contains(name)) {
          if (auto s = parseInputSpec(name, false, optObj.value(name))) {
            schema.inputs.push_back(std::move(*s));
            seenOpt.insert(name);
          }
        }
      }
      for (auto oit = optObj.begin(); oit != optObj.end(); ++oit) {
        if (!seenOpt.contains(oit.key())) {
          if (auto s = parseInputSpec(oit.key(), false, oit.value())) {
            schema.inputs.push_back(std::move(*s));
          }
        }
      }
    } else {
      for (auto rit = reqObj.begin(); rit != reqObj.end(); ++rit) {
        if (auto s = parseInputSpec(rit.key(), true, rit.value())) {
          schema.inputs.push_back(std::move(*s));
        }
      }
      for (auto oit = optObj.begin(); oit != optObj.end(); ++oit) {
        if (auto s = parseInputSpec(oit.key(), false, oit.value())) {
          schema.inputs.push_back(std::move(*s));
        }
      }
    }

    const QJsonArray outTypes = val.value(QStringLiteral("output")).toArray();
    const QJsonArray outNames = val.value(QStringLiteral("output_name")).toArray();
    const QJsonArray outIsList = val.value(QStringLiteral("output_is_list")).toArray();

    for (int i = 0; i < outTypes.size(); ++i) {
      OutputSpec outSpec;
      outSpec.type = outTypes.at(i).toString();
      if (i < outNames.size() && outNames.at(i).isString()) {
        outSpec.name = outNames.at(i).toString();
      } else {
        outSpec.name = outSpec.type;
      }
      if (i < outIsList.size() && outIsList.at(i).isBool()) {
        outSpec.isList = outIsList.at(i).toBool();
      }
      schema.outputs.push_back(std::move(outSpec));
    }

    set.nodes_[nodeKey] = std::move(schema);
  }
  return set;
}

const NodeSchema* SchemaSet::find(const QString& classType) const {
  const auto it = nodes_.find(classType);
  if (it != nodes_.end()) return &it->second;
  return nullptr;
}

QStringList SchemaSet::classTypes() const {
  QStringList list;
  list.reserve(static_cast<qsizetype>(nodes_.size()));
  for (const auto& [k, _] : nodes_) {
    list.append(k);
  }
  list.sort();
  return list;
}

int SchemaSet::size() const {
  return static_cast<int>(nodes_.size());
}

bool SchemaSet::empty() const {
  return nodes_.empty();
}

QJsonObject toolSchema(const NodeSchema& n) {
  QJsonObject properties;
  QStringList requiredFields;

  for (const auto& in : n.inputs) {
    if (in.kind == InputKind::Link) continue;

    QJsonObject prop;
    switch (in.kind) {
    case InputKind::Int:
      prop.insert(QStringLiteral("type"), QStringLiteral("integer"));
      if (in.min) prop.insert(QStringLiteral("minimum"), *in.min);
      if (in.max) prop.insert(QStringLiteral("maximum"), *in.max);
      break;
    case InputKind::Float:
      prop.insert(QStringLiteral("type"), QStringLiteral("number"));
      if (in.min) prop.insert(QStringLiteral("minimum"), *in.min);
      if (in.max) prop.insert(QStringLiteral("maximum"), *in.max);
      break;
    case InputKind::String:
      prop.insert(QStringLiteral("type"), QStringLiteral("string"));
      break;
    case InputKind::Boolean:
      prop.insert(QStringLiteral("type"), QStringLiteral("boolean"));
      break;
    case InputKind::Combo: {
      prop.insert(QStringLiteral("type"), QStringLiteral("string"));
      QJsonArray enumArr;
      for (const QString& opt : in.options) enumArr.append(opt);
      prop.insert(QStringLiteral("enum"), enumArr);
      break;
    }
    case InputKind::Link:
      break;
    }

    if (!in.defaultValue.isUndefined()) {
      prop.insert(QStringLiteral("default"), in.defaultValue);
    }

    properties.insert(in.name, prop);

    if (in.required && in.defaultValue.isUndefined()) {
      requiredFields.append(in.name);
    }
  }

  requiredFields.sort();
  QJsonArray reqArr;
  for (const QString& r : requiredFields) reqArr.append(r);

  QJsonObject root;
  root.insert(QStringLiteral("type"), QStringLiteral("object"));
  root.insert(QStringLiteral("properties"), properties);
  root.insert(QStringLiteral("required"), reqArr);
  return root;
}

} // namespace sf::comfy
