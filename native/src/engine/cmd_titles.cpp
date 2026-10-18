// Title templates (motion-graphics animated titles): title.list and title.apply.
// Pure data templates instantiated with user text as ONE undo step.

#include "engine/cmd_util.h"
#include "render/effects.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <vector>

namespace sf::engine {

namespace {

struct LayerDef {
  QString kind; // "shape" or "text"
  QString id;
  QString repeat;
  int repeatMin = 1;
  int repeatMax = 3;
  double stepSec = 0.15;
  double ySpacing = 0.09;
  QString shiftRule;
  QJsonObject shape;
  QString text;
  QJsonObject style;
  QJsonObject transform;
  QJsonObject keyframes;
};

struct EffectDef {
  QString layer;
  QString effectId;
  QJsonObject params;
};

struct TemplateDef {
  QString name;
  QString description;
  QString aspect = QStringLiteral("any");
  QStringList whenToUse;
  QStringList avoidWhen;
  QJsonObject fields;
  double durationSec = 4.0;
  std::vector<LayerDef> layers;
  std::vector<EffectDef> effects;
  QJsonObject raw;
};

Easing parseEasing(const QString& name) {
  if (name == QLatin1String("hold")) return Easing::Hold;
  if (name == QLatin1String("easeIn") || name == QLatin1String("easeInCubic")) return Easing::EaseIn;
  if (name == QLatin1String("easeOut") || name == QLatin1String("easeOutCubic") || name == QLatin1String("easeOutBack")) return Easing::EaseOut;
  if (name == QLatin1String("easeInOut") || name == QLatin1String("easeInOutCubic")) return Easing::EaseInOut;
  return Easing::Linear;
}

ShapeKind parseShapeKind(const QString& s) {
  if (s == QLatin1String("ellipse")) return ShapeKind::Ellipse;
  if (s == QLatin1String("triangle")) return ShapeKind::Triangle;
  return ShapeKind::Rect;
}

TextAlign parseTextAlign(const QString& s) {
  if (s == QLatin1String("left")) return TextAlign::Left;
  if (s == QLatin1String("right")) return TextAlign::Right;
  return TextAlign::Center;
}

double stretchTime(double t, double origDur, double newDur) {
  if (std::abs(origDur - newDur) < 1e-6 || origDur <= 0.0) return t;
  const double t1 = 0.25 * origDur;
  const double t2 = 0.75 * origDur;
  if (t <= t1) return t;
  if (t >= t2) return t + (newDur - origDur);
  const double u = (t - t1) / (t2 - t1);
  return t1 + u * (newDur - 0.5 * origDur);
}

std::vector<TemplateDef> loadTemplates() {
  std::vector<TemplateDef> list;
  QFile f(QStringLiteral(":/sf/style/titles.json"));
  if (!f.open(QIODevice::ReadOnly)) return list;
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
  if (!doc.isObject()) return list;

  for (const QJsonValue& itemVal : doc.object().value(q("items")).toArray()) {
    const QJsonObject obj = itemVal.toObject();
    TemplateDef t;
    t.name = obj.value(q("name")).toString();
    t.description = obj.value(q("description")).toString();
    t.aspect = obj.value(q("aspect")).toString(QStringLiteral("any"));
    for (const QJsonValue& w : obj.value(q("whenToUse")).toArray()) t.whenToUse.append(w.toString());
    for (const QJsonValue& a : obj.value(q("avoidWhen")).toArray()) t.avoidWhen.append(a.toString());
    t.fields = obj.value(q("fields")).toObject();
    t.durationSec = obj.value(q("durationSec")).toDouble(4.0);
    t.raw = obj;

    for (const QJsonValue& lv : obj.value(q("layers")).toArray()) {
      const QJsonObject lo = lv.toObject();
      LayerDef ld;
      ld.kind = lo.value(q("kind")).toString();
      ld.id = lo.value(q("id")).toString();
      ld.repeat = lo.value(q("repeat")).toString();
      ld.repeatMin = lo.value(q("repeatMin")).toInt(1);
      ld.repeatMax = lo.value(q("repeatMax")).toInt(3);
      ld.stepSec = lo.value(q("stepSec")).toDouble(0.15);
      ld.ySpacing = lo.value(q("ySpacing")).toDouble(0.09);
      ld.shiftRule = lo.value(q("shiftRule")).toString();
      ld.shape = lo.value(q("shape")).toObject();
      ld.text = lo.value(q("text")).toString();
      ld.style = lo.value(q("style")).toObject();
      ld.transform = lo.value(q("transform")).toObject();
      ld.keyframes = lo.value(q("keyframes")).toObject();
      t.layers.push_back(std::move(ld));
    }

    for (const QJsonValue& ev : obj.value(q("effects")).toArray()) {
      const QJsonObject eo = ev.toObject();
      EffectDef ed;
      ed.layer = eo.value(q("layer")).toString();
      ed.effectId = eo.value(q("effectId")).toString();
      ed.params = eo.value(q("params")).toObject();
      t.effects.push_back(std::move(ed));
    }

    list.push_back(std::move(t));
  }
  return list;
}

const std::vector<TemplateDef>& templates() {
  static const std::vector<TemplateDef> list = loadTemplates();
  return list;
}

const TemplateDef* findTemplate(const QString& name) {
  for (const TemplateDef& t : templates()) {
    if (t.name == name) return &t;
  }
  return nullptr;
}

// ---------------------------------------------------------------- title.list

Result titleListCmd(CallContext&, const QJsonObject& a) {
  const QString tag = a.value(q("tag")).toString().trimmed().toLower();
  const QString aspect = a.value(q("aspect")).toString().trimmed().toLower();

  QJsonArray outArr;
  for (const TemplateDef& t : templates()) {
    if (!tag.isEmpty()) {
      bool matched = false;
      for (const QString& w : t.whenToUse) {
        if (w.toLower() == tag || w.toLower().contains(tag)) {
          matched = true;
          break;
        }
      }
      if (!matched) continue;
    }

    if (!aspect.isEmpty() && aspect != QLatin1String("any")) {
      if (t.aspect != QLatin1String("any") && t.aspect != aspect) continue;
    }

    outArr.append(QJsonObject{
      {q("name"), t.name},
      {q("description"), t.description},
      {q("fields"), t.fields},
      {q("durationSec"), t.durationSec},
      {q("aspect"), t.aspect},
      {q("whenToUse"), QJsonArray::fromStringList(t.whenToUse)},
      {q("avoidWhen"), QJsonArray::fromStringList(t.avoidWhen)}
    });
  }

  return Result::success(QJsonObject{{q("titles"), outArr}});
}

// ---------------------------------------------------------------- title.apply

Result titleApplyCmd(CallContext& c, const QJsonObject& a) {
  const editor::Project& proj = *c.session->project;
  const TimelineDoc& d = proj.doc();

  const QString name = a.value(q("name")).toString();
  const TemplateDef* tmpl = findTemplate(name);
  if (!tmpl) {
    return notFound(QStringLiteral("Title template \"%1\" not found.").arg(name),
                    QStringLiteral("Call title.list to view available title templates."));
  }

  const double fps = static_cast<double>(d.project.fps > 0 ? d.project.fps : 30);
  const double cw = static_cast<double>(d.project.width);
  const double ch = static_cast<double>(d.project.height);

  const Frame startFrame = std::max<Frame>(0, a.value(q("atFrame")).toInteger(0));
  const double origDurSec = tmpl->durationSec;
  const double durSec = a.contains(q("durationSec")) ? a.value(q("durationSec")).toDouble() : origDurSec;
  if (durSec <= 0.0) return invalid(QStringLiteral("durationSec must be greater than zero."));
  const Frame durationFrames = std::max<Frame>(1, jsRound(durSec * fps));

  // Resolve user fields & check maxChars
  QJsonArray warnings;
  const QJsonObject userFields = a.value(q("fields")).toObject();
  std::map<QString, QString> resolvedFields;

  for (auto it = tmpl->fields.begin(); it != tmpl->fields.end(); ++it) {
    const QString fKey = it.key();
    const QJsonObject fDef = it.value().toObject();
    QString val;
    if (userFields.contains(fKey)) {
      val = userFields.value(fKey).toString();
    } else {
      val = fDef.value(q("default")).toString();
    }
    if (fDef.contains(q("maxChars"))) {
      const int maxChars = fDef.value(q("maxChars")).toInt();
      if (val.size() > maxChars) {
        warnings.append(QStringLiteral("Field \"%1\" has length %2 exceeding maxChars (%3).").arg(fKey).arg(val.size()).arg(maxChars));
      }
    }
    resolvedFields[fKey] = val;
  }

  // Also include any extra user fields not in template
  for (auto it = userFields.begin(); it != userFields.end(); ++it) {
    if (resolvedFields.find(it.key()) == resolvedFields.end()) {
      resolvedFields[it.key()] = it.value().toString();
    }
  }

  // Determine track
  std::vector<Op> ops;
  QString trackId = a.value(q("trackId")).toString();
  if (trackId.isEmpty()) {
    trackId = newId(q("trk"));
    TrackAdd ta;
    ta.trackId = trackId;
    ta.name = tmpl->name;
    ta.kind = TrackKind::Text;
    ops.push_back(Op(ta));
  } else {
    const Track* trk = getTrack(d, trackId);
    if (!trk) return notFound(QStringLiteral("Track \"%1\" not found.").arg(trackId));
    if (trk->locked) {
      return Result::failure(q("edit_rejected"), QStringLiteral("Track \"%1\" is locked.").arg(trk->name));
    }
    if (!trackAllowsItem(trk->kind, ItemType::Text) || !trackAllowsItem(trk->kind, ItemType::Shape)) {
      return invalid(QStringLiteral("Track \"%1\" (%2) cannot hold title items. Use a text or graphics track.").arg(trk->name, enumName(trk->kind)));
    }
  }

  // Generate layers
  std::vector<LayerDef> activeLayers;
  for (const LayerDef& layer : tmpl->layers) {
    if (!layer.repeat.isEmpty()) {
      const QString repeatKey = layer.repeat;
      const QString wordsVal = resolvedFields.find(repeatKey) != resolvedFields.end()
                                   ? resolvedFields[repeatKey]
                                   : tmpl->fields.value(repeatKey).toObject().value(q("default")).toString();
      const QStringList wordsList = wordsVal.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
      const int count = std::clamp<int>(static_cast<int>(wordsList.size()), layer.repeatMin, layer.repeatMax);
      for (int i = 0; i < count; ++i) {
        LayerDef ld = layer;
        ld.id.replace(QStringLiteral("{i}"), QString::number(i));
        if (ld.id == layer.id) {
          ld.id = QStringLiteral("%1_%2").arg(layer.id).arg(i);
        }
        ld.text.replace(QStringLiteral("{i}"), QString::number(i));
        ld.text.replace(QStringLiteral("{word}"), wordsList[static_cast<size_t>(i)]);

        const double baseY = layer.transform.value(q("y")).toDouble(0.50);
        const double yOffset = baseY + (static_cast<double>(i) - (count - 1) / 2.0) * layer.ySpacing;
        ld.transform.insert(q("y"), yOffset);

        const double dt = static_cast<double>(i) * layer.stepSec;
        QJsonObject shiftedKeyframes;
        for (auto kit = layer.keyframes.begin(); kit != layer.keyframes.end(); ++kit) {
          QJsonArray origArr = kit.value().toArray();
          QJsonArray newArr;
          for (int kIdx = 0; kIdx < origArr.size(); ++kIdx) {
            QJsonArray kf = origArr[kIdx].toArray();
            if (kf.size() >= 2) {
              double t = kf[0].toDouble();
              if (layer.shiftRule == QLatin1String("cascade") || !layer.shiftRule.isEmpty()) {
                if (kIdx == 0 || kIdx == 1) {
                  t += dt;
                } else if (kIdx == 2 && origArr.size() >= 4) {
                  const double tFull = origArr[1].toArray()[0].toDouble() + dt;
                  t = std::max(tFull + 0.5, origDurSec - (origDurSec - t));
                }
              }
              kf[0] = t;
            }
            newArr.append(kf);
          }
          shiftedKeyframes.insert(kit.key(), newArr);
        }
        ld.keyframes = shiftedKeyframes;
        activeLayers.push_back(std::move(ld));
      }
    } else {
      activeLayers.push_back(layer);
    }
  }

  QStringList createdItemIds;

  for (const LayerDef& layer : activeLayers) {
    ItemInit init;
    init.id = newId(q("itm"));
    init.trackId = trackId;
    init.startFrame = startFrame;
    init.durationFrames = durationFrames;
    init.labels = Labels{layer.id, std::nullopt};

    const double xFrac = layer.transform.value(q("x")).toDouble(0.5);
    const double yFrac = layer.transform.value(q("y")).toDouble(0.5);
    const QString anchor = layer.transform.value(q("anchor")).toString(QStringLiteral("center"));

    double shapeWFrac = 0.0, shapeHFrac = 0.0;
    if (layer.kind == QLatin1String("shape")) {
      shapeWFrac = layer.shape.value(q("width")).toDouble(0.3);
      shapeHFrac = layer.shape.value(q("height")).toDouble(0.1);
      const double rFrac = layer.shape.value(q("radius")).toDouble(0.0);

      ShapeProps sp;
      sp.shape = parseShapeKind(layer.shape.value(q("shape")).toString(QStringLiteral("rect")));
      sp.fill = layer.shape.value(q("fill")).toString(QStringLiteral("#FFFFFF"));
      if (layer.shape.contains(q("stroke"))) sp.stroke = layer.shape.value(q("stroke")).toString();
      sp.strokeWidth = layer.shape.value(q("strokeWidth")).toDouble(0.0) * ch;
      sp.width = shapeWFrac * cw;
      sp.height = shapeHFrac * ch;
      sp.radius = rFrac * ch;
      init.props = ItemProps{sp};
    } else {
      QString textStr = layer.text;
      for (const auto& [k, v] : resolvedFields) {
        textStr.replace(QStringLiteral("{%1}").arg(k), v);
      }
      TextProps tp;
      tp.text = textStr;
      tp.style.fontSize = layer.style.value(q("fontSize")).toDouble(0.04) * ch;
      tp.style.fontWeight = layer.style.value(q("fontWeight")).toInteger(700);
      tp.style.color = layer.style.value(q("color")).toString(QStringLiteral("#FFFFFF"));
      tp.style.align = parseTextAlign(layer.style.value(q("align")).toString(QStringLiteral("center")));
      tp.style.fontFamily = layer.style.value(q("fontFamily")).toString(QStringLiteral("Geist"));
      init.props = ItemProps{tp};
    }

    double cxFrac = xFrac;
    double cyFrac = yFrac;
    if (layer.kind == QLatin1String("shape")) {
      if (anchor == QLatin1String("left")) cxFrac += shapeWFrac / 2.0;
      else if (anchor == QLatin1String("right")) cxFrac -= shapeWFrac / 2.0;
      if (anchor == QLatin1String("top")) cyFrac += shapeHFrac / 2.0;
      else if (anchor == QLatin1String("bottom")) cyFrac -= shapeHFrac / 2.0;
    }

    TransformPatch tp;
    tp.x = cxFrac * cw - cw / 2.0;
    tp.y = cyFrac * ch - ch / 2.0;
    tp.scale = layer.transform.value(q("scale")).toDouble(1.0);
    tp.opacity = layer.transform.value(q("opacity")).toDouble(1.0);
    tp.rotation = layer.transform.value(q("rotation")).toDouble(0.0);
    init.transform = tp;

    // Keyframes
    KeyframeMap kfMap;
    for (auto kit = layer.keyframes.begin(); kit != layer.keyframes.end(); ++kit) {
      const QString pName = kit.key();
      QString mappedProp = pName;
      if (pName == QLatin1String("positionX")) mappedProp = QStringLiteral("transform.x");
      else if (pName == QLatin1String("positionY")) mappedProp = QStringLiteral("transform.y");
      else if (pName == QLatin1String("scale")) mappedProp = QStringLiteral("transform.scale");
      else if (pName == QLatin1String("opacity")) mappedProp = QStringLiteral("transform.opacity");
      else if (pName == QLatin1String("rotation")) mappedProp = QStringLiteral("transform.rotation");

      KeyframeList kfList;
      for (const QJsonValue& kfv : kit.value().toArray()) {
        const QJsonArray kfArr = kfv.toArray();
        if (kfArr.size() < 2) continue;
        const double timeSec = kfArr.at(0).toDouble();
        const double rawVal = kfArr.at(1).toDouble();
        const QString easingStr = kfArr.size() >= 3 ? kfArr.at(2).toString(QStringLiteral("linear")) : QStringLiteral("linear");

        const double stretchedSec = stretchTime(timeSec, origDurSec, durSec);
        const Frame kfFrame = std::clamp<Frame>(jsRound(stretchedSec * fps), 0, durationFrames);

        double mappedVal = rawVal;
        if (mappedProp == QLatin1String("transform.x")) {
          double fracVal = rawVal;
          if (layer.kind == QLatin1String("shape")) {
            if (anchor == QLatin1String("left")) fracVal += shapeWFrac / 2.0;
            else if (anchor == QLatin1String("right")) fracVal -= shapeWFrac / 2.0;
          }
          mappedVal = fracVal * cw - cw / 2.0;
        } else if (mappedProp == QLatin1String("transform.y")) {
          double fracVal = rawVal;
          if (layer.kind == QLatin1String("shape")) {
            if (anchor == QLatin1String("top")) fracVal += shapeHFrac / 2.0;
            else if (anchor == QLatin1String("bottom")) fracVal -= shapeHFrac / 2.0;
          }
          mappedVal = fracVal * ch - ch / 2.0;
        }

        kfList.push_back(Keyframe{kfFrame, mappedVal, parseEasing(easingStr)});
      }
      if (!kfList.empty()) kfMap[mappedProp] = std::move(kfList);
    }
    init.keyframes = std::move(kfMap);

    const ItemType itType = layer.kind == QLatin1String("shape") ? ItemType::Shape : ItemType::Text;
    Item item = createItem(itType, init);

    // Apply any effects targeted to this layer
    for (const EffectDef& ed : tmpl->effects) {
      if (ed.layer == layer.id) {
        FxInstance fxi;
        fxi.id = newId(q("fx"));
        fxi.effectId = ed.effectId;
        fxi.enabled = true;
        fxi.params = ed.params;
        item.fx.push_back(std::move(fxi));
      }
    }

    createdItemIds.append(item.id);
    ops.push_back(Op(ItemAdd{std::move(item), false}));
  }

  // Apply all ops as ONE undo step
  const Result appRes = applyToSession(c, ops, QStringLiteral("title.apply: %1").arg(tmpl->name));
  if (!appRes.ok) return appRes;

  // Run safety check on the title's range
  const double startSec = static_cast<double>(startFrame) / fps;
  const double endSec = static_cast<double>(startFrame + durationFrames) / fps;

  const QJsonObject safetyArgs{
    {q("session"), c.session->id},
    {q("checks"), QJsonArray{q("text_legibility"), q("safe_zones")}},
    {q("range"), QJsonObject{{q("start"), startSec}, {q("end"), endSec}}}
  };

  const Result sRes = c.engine->call(q("safety.check"), safetyArgs);
  if (!sRes.ok) {
    return Result::failure(sRes.error.code, QStringLiteral("safety.check failed: %1").arg(sRes.error.message));
  }
  QJsonObject checkObj = sRes.value.toObject();
  if (!checkObj.value(q("pass")).toBool(true)) {
    const int viols = checkObj.value(q("violations")).toArray().size();
    warnings.append(QStringLiteral("safety.check detected %1 violation(s).").arg(viols));
  }

  QJsonArray itemsArr;
  for (const QString& id : createdItemIds) itemsArr.append(id);

  return Result::success(QJsonObject{
    {q("track"), trackId},
    {q("items"), itemsArr},
    {q("warnings"), warnings},
    {q("check"), checkObj}
  });
}

} // namespace

void registerTitleCommands(Engine& e) {
  e.add(spec("title.list", "", "style", false,
             QStringLiteral("Lists available title motion-graphics templates with metadata, fields, and duration. Numbers and timing are deterministic data."),
             js::obj({
               {q("tag"), js::str(QStringLiteral("Filter by tag matching whenToUse"))},
               {q("aspect"), js::choice({q("landscape"), q("portrait"), q("any")}, QStringLiteral("Filter by aspect ratio"))}
             }),
             titleListCmd,
             false));

  e.add(spec("title.apply", "", "style", true,
             QStringLiteral("Instantiates an animated title template on a track as ONE undo step. Replaces {field} placeholders, animates keyframes, and runs safety checks."),
             js::obj({
               {q("name"), js::str(QStringLiteral("Template name"))},
               {q("fields"), js::any(QStringLiteral("Field replacements, e.g. {title, subtitle}"))},
               {q("atFrame"), js::integer(QStringLiteral("Timeline frame to place the title at, default 0"), 0)},
               {q("trackId"), js::str(QStringLiteral("Existing text/graphics track to place layers on"))},
               {q("durationSec"), js::number(QStringLiteral("Stretched duration in seconds"), 0.1, 300.0)}
             }),
             titleApplyCmd,
             true));
}

} // namespace sf::engine
