#include "editor/inspector.h"

#include "core/timeline_doc.h"
#include "editor/timeline_model.h"

#include <QJsonArray>
#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <map>
#include <functional>

namespace sf::editor {

namespace {

struct EffectInfo {
  const char* type;
  const char* label;
  const char* param;
  double min, max, def;
};

// The effects the compositor draws (colour-matrix ones); one slider each.
constexpr EffectInfo kEffects[] = {
    {"brightness", "Brightness", "amount", -0.8, 0.8, 0.2},
    {"contrast", "Contrast", "amount", 0.0, 2.0, 1.3},
    {"saturation", "Saturation", "amount", 0.0, 2.0, 1.4},
    {"hueRotate", "Hue", "degrees", -180, 180, 30},
    {"grayscale", "Black & white", "amount", 0.0, 1.0, 1.0},
    {"sepia", "Sepia", "amount", 0.0, 1.0, 1.0},
};

const EffectInfo* effectInfo(const QString& type) {
  for (const EffectInfo& e : kEffects) {
    if (type == QLatin1String(e.type)) return &e;
  }
  return nullptr;
}

double paramOf(const Effect& e, const char* name, double fallback) {
  const auto it = e.params.find(QString::fromLatin1(name));
  if (it == e.params.end()) return fallback;
  const double* v = std::get_if<double>(&it->second);
  return v ? *v : fallback;
}

// Sets (or, for an empty string on an optional colour, removes) a value at a dotted path.
void setPath(QJsonObject& obj, const QStringList& path, const QJsonValue& value, bool removeIfEmptyString = false) {
  if (path.size() == 1) {
    if (removeIfEmptyString && value.isString() && value.toString().isEmpty()) obj.remove(path.first());
    else obj.insert(path.first(), value);
    return;
  }
  QJsonObject child = obj.value(path.first()).toObject();
  setPath(child, path.mid(1), value, removeIfEmptyString);
  obj.insert(path.first(), child);
}

bool hasStyle(const Item& item) { return item.type() == ItemType::Text || item.type() == ItemType::Caption; }

const TextStyle* styleOf(const Item& item) {
  if (const auto* t = std::get_if<TextProps>(&item.props)) return &t->style;
  if (const auto* c = std::get_if<CaptionProps>(&item.props)) return &c->style;
  return nullptr;
}

} // namespace

Inspector::Inspector(Project& project, QObject* parent) : QObject(parent), project_(project) {
  connect(&project_, &Project::selectionChanged, this, &Inspector::refresh);
  connect(&project_, &Project::docChanged, this, &Inspector::refresh);
  refresh();
}

QVariantList Inspector::effectCatalog() const {
  QVariantList out;
  for (const EffectInfo& e : kEffects) {
    out.push_back(QVariantMap{{QStringLiteral("type"), QString::fromLatin1(e.type)}, {QStringLiteral("label"), QString::fromLatin1(e.label)}});
  }
  return out;
}

void Inspector::refresh() {
  ids_.clear();
  for (const Item& i : project_.doc().items) {
    if (project_.isSelected(i.id)) ids_.push_back(i.id);
  }
  QVariantMap v;
  QVariantList fx;
  if (!ids_.empty()) {
    const TimelineDoc& doc = project_.doc();
    const Item& item = *getItem(doc, ids_.front());
    const double fps = static_cast<double>(doc.project.fps);
    v[QStringLiteral("type")] = enumName(item.type());
    v[QStringLiteral("name")] = itemDisplayName(item, item.assetId ? project_.asset(*item.assetId) : nullptr);
    v[QStringLiteral("startTc")] = formatTimecode(item.startFrame, fps);
    v[QStringLiteral("durationTc")] = formatTimecode(item.durationFrames, fps);
    v[QStringLiteral("x")] = item.transform.x;
    v[QStringLiteral("y")] = item.transform.y;
    v[QStringLiteral("scale")] = item.transform.scale;
    v[QStringLiteral("rotation")] = item.transform.rotation;
    v[QStringLiteral("opacity")] = item.transform.opacity;
    v[QStringLiteral("speed")] = item.speed;
    v[QStringLiteral("volume")] = item.volume;
    v[QStringLiteral("muted")] = item.muted;
    v[QStringLiteral("keyframes")] = static_cast<int>(item.keyframes.size());
    if (const auto* video = std::get_if<VideoProps>(&item.props)) {
      v[QStringLiteral("fadeIn")] = static_cast<qlonglong>(video->fadeInFrames);
      v[QStringLiteral("fadeOut")] = static_cast<qlonglong>(video->fadeOutFrames);
    } else if (const auto* audio = std::get_if<AudioProps>(&item.props)) {
      v[QStringLiteral("fadeIn")] = static_cast<qlonglong>(audio->fadeInFrames);
      v[QStringLiteral("fadeOut")] = static_cast<qlonglong>(audio->fadeOutFrames);
    }
    if (const auto* text = std::get_if<TextProps>(&item.props)) v[QStringLiteral("text")] = text->text;
    if (const TextStyle* s = styleOf(item)) {
      v[QStringLiteral("fontSize")] = s->fontSize;
      v[QStringLiteral("fontWeight")] = static_cast<qlonglong>(s->fontWeight);
      v[QStringLiteral("color")] = s->color;
      v[QStringLiteral("align")] = enumName(s->align);
      v[QStringLiteral("strokeWidth")] = s->strokeWidth;
      v[QStringLiteral("strokeColor")] = s->strokeColor.value_or(QString());
      v[QStringLiteral("backgroundColor")] = s->backgroundColor.value_or(QString());
      v[QStringLiteral("uppercase")] = s->uppercase;
    }
    if (const auto* cap = std::get_if<CaptionProps>(&item.props)) {
      v[QStringLiteral("captionMode")] = enumName(cap->mode);
      v[QStringLiteral("maxWordsPerCard")] = static_cast<qlonglong>(cap->maxWordsPerCard);
      v[QStringLiteral("highlight")] = enumName(cap->style.highlight);
      v[QStringLiteral("highlightColor")] = cap->style.highlightColor;
      v[QStringLiteral("placementY")] = cap->style.placementY;
      v[QStringLiteral("wordCount")] = static_cast<int>(cap->words.size());
    }
    if (const auto* shape = std::get_if<ShapeProps>(&item.props)) {
      v[QStringLiteral("shape")] = enumName(shape->shape);
      v[QStringLiteral("fill")] = shape->fill;
      v[QStringLiteral("stroke")] = shape->stroke.value_or(QString());
      v[QStringLiteral("strokeWidth")] = shape->strokeWidth;
      v[QStringLiteral("radius")] = shape->radius;
      v[QStringLiteral("width")] = shape->width;
      v[QStringLiteral("height")] = shape->height;
    }
    for (const Effect& e : item.effects) {
      const EffectInfo* info = effectInfo(e.type);
      if (!info) continue;
      fx.push_back(QVariantMap{{QStringLiteral("id"), e.id},
                               {QStringLiteral("type"), e.type},
                               {QStringLiteral("label"), QString::fromLatin1(info->label)},
                               {QStringLiteral("value"), paramOf(e, info->param, info->def)},
                               {QStringLiteral("min"), info->min},
                               {QStringLiteral("max"), info->max}});
    }
  }
  v[QStringLiteral("count")] = static_cast<int>(ids_.size());
  if (v == values_ && fx == effects_) return;
  values_ = std::move(v);
  effects_ = std::move(fx);
  emit changed();
}

// ---------- item edits ----------

bool Inspector::propsOp(const Item& item, const std::function<bool(QJsonObject&)>& change, std::vector<Op>& out) const {
  QJsonObject props = toJson(item.props);
  if (!change(props)) return false;
  ItemUpdate u;
  u.itemId = item.id;
  u.patch.props = QJsonValue(props);
  out.emplace_back(u);
  return true;
}

bool Inspector::opsFor(const Item& item, const QString& key, const QVariant& value, std::vector<Op>& out) const {
  const ItemType type = item.type();
  const bool media = isMediaType(type);
  const bool visual = type != ItemType::Audio;
  const auto patch = [&](const std::function<void(ItemPatch&)>& f) {
    ItemUpdate u;
    u.itemId = item.id;
    f(u.patch);
    out.emplace_back(u);
    return true;
  };
  const auto transform = [&](const std::function<void(TransformPatch&)>& f) {
    if (!visual) return false;
    return patch([&](ItemPatch& p) {
      TransformPatch t;
      f(t);
      p.transform = t;
    });
  };
  const double num = value.toDouble();

  if (key == QLatin1String("name")) {
    return patch([&](ItemPatch& p) {
      LabelsPatch l;
      l.name = std::optional<QString>(value.toString());
      p.labels = l;
    });
  }
  if (key == QLatin1String("x")) return transform([&](TransformPatch& t) { t.x = num; });
  if (key == QLatin1String("y")) return transform([&](TransformPatch& t) { t.y = num; });
  if (key == QLatin1String("scale")) return transform([&](TransformPatch& t) { t.scale = num; });
  if (key == QLatin1String("rotation")) return transform([&](TransformPatch& t) { t.rotation = num; });
  if (key == QLatin1String("opacity")) return transform([&](TransformPatch& t) { t.opacity = num; });
  if (key == QLatin1String("speed")) {
    if (!media) return false;
    out.emplace_back(ItemSetSpeed{item.id, num});
    return true;
  }
  if (key == QLatin1String("volume")) {
    if (!media) return false;
    return patch([&](ItemPatch& p) { p.volume = num; });
  }
  if (key == QLatin1String("muted")) {
    if (!media) return false;
    return patch([&](ItemPatch& p) { p.muted = value.toBool(); });
  }
  if (key == QLatin1String("fadeIn") || key == QLatin1String("fadeOut")) {
    if (!media) return false;
    const QString field = key == QLatin1String("fadeIn") ? QStringLiteral("fadeInFrames") : QStringLiteral("fadeOutFrames");
    return propsOp(item, [&](QJsonObject& o) { o.insert(field, std::max(0.0, std::round(num))); return true; }, out);
  }

  // text and caption style
  static const std::map<QString, QString> styleKeys{
      {QStringLiteral("fontSize"), QStringLiteral("fontSize")},       {QStringLiteral("fontWeight"), QStringLiteral("fontWeight")},
      {QStringLiteral("color"), QStringLiteral("color")},             {QStringLiteral("align"), QStringLiteral("align")},
      {QStringLiteral("strokeColor"), QStringLiteral("strokeColor")}, {QStringLiteral("backgroundColor"), QStringLiteral("backgroundColor")},
      {QStringLiteral("uppercase"), QStringLiteral("uppercase")},     {QStringLiteral("strokeWidth"), QStringLiteral("strokeWidth")},
      {QStringLiteral("highlight"), QStringLiteral("highlight")},     {QStringLiteral("highlightColor"), QStringLiteral("highlightColor")},
      {QStringLiteral("placementY"), QStringLiteral("placementY")}};
  if (hasStyle(item) && styleKeys.count(key) && !(type == ItemType::Text && (key == QLatin1String("highlight") || key == QLatin1String("highlightColor") || key == QLatin1String("placementY")))) {
    const bool optionalColor = key == QLatin1String("strokeColor") || key == QLatin1String("backgroundColor");
    const bool textual = key == QLatin1String("color") || key == QLatin1String("align") || optionalColor || key == QLatin1String("highlight") || key == QLatin1String("highlightColor");
    const QJsonValue v = key == QLatin1String("uppercase") ? QJsonValue(value.toBool()) : textual ? QJsonValue(value.toString()) : QJsonValue(num);
    return propsOp(item, [&](QJsonObject& o) { setPath(o, {QStringLiteral("style"), styleKeys.at(key)}, v, optionalColor); return true; }, out);
  }
  if (type == ItemType::Text && key == QLatin1String("text")) {
    const QString text = value.toString();
    if (text.trimmed().isEmpty()) return false; // the reference refuses an empty title too
    return propsOp(item, [&](QJsonObject& o) { o.insert(QStringLiteral("text"), text); return true; }, out);
  }
  if (type == ItemType::Caption) {
    if (key == QLatin1String("captionMode")) return propsOp(item, [&](QJsonObject& o) { o.insert(QStringLiteral("mode"), value.toString()); return true; }, out);
    if (key == QLatin1String("maxWordsPerCard")) return propsOp(item, [&](QJsonObject& o) { o.insert(QStringLiteral("maxWordsPerCard"), std::round(num)); return true; }, out);
  }
  if (type == ItemType::Shape) {
    static const std::map<QString, std::pair<QString, bool>> shapeKeys{
        {QStringLiteral("shape"), {QStringLiteral("shape"), true}},   {QStringLiteral("fill"), {QStringLiteral("fill"), true}},
        {QStringLiteral("stroke"), {QStringLiteral("stroke"), true}}, {QStringLiteral("strokeWidth"), {QStringLiteral("strokeWidth"), false}},
        {QStringLiteral("radius"), {QStringLiteral("radius"), false}}, {QStringLiteral("width"), {QStringLiteral("width"), false}},
        {QStringLiteral("height"), {QStringLiteral("height"), false}}};
    if (const auto it = shapeKeys.find(key); it != shapeKeys.end()) {
      const QJsonValue v = it->second.second ? QJsonValue(value.toString()) : QJsonValue(num);
      return propsOp(item, [&](QJsonObject& o) { setPath(o, {it->second.first}, v, key == QLatin1String("stroke")); return true; }, out);
    }
  }
  return false;
}

QString Inspector::labelFor(const QString& key) const {
  static const std::map<QString, QString> names{{QStringLiteral("x"), QStringLiteral("Position")},     {QStringLiteral("y"), QStringLiteral("Position")},
                                                {QStringLiteral("scale"), QStringLiteral("Scale")},    {QStringLiteral("rotation"), QStringLiteral("Rotation")},
                                                {QStringLiteral("opacity"), QStringLiteral("Opacity")}, {QStringLiteral("speed"), QStringLiteral("Speed")},
                                                {QStringLiteral("volume"), QStringLiteral("Volume")},  {QStringLiteral("muted"), QStringLiteral("Mute")},
                                                {QStringLiteral("text"), QStringLiteral("Edit text")}, {QStringLiteral("name"), QStringLiteral("Rename clip")}};
  const auto it = names.find(key);
  return it == names.end() ? QStringLiteral("Edit %1").arg(key) : it->second;
}

bool Inspector::run(const QString& key, const QVariant& value, bool live) {
  if (ids_.empty()) return false;
  std::vector<Op> ops;
  for (const QString& id : ids_) {
    if (const Item* item = getItem(project_.doc(), id)) opsFor(*item, key, value, ops);
  }
  if (ops.empty()) return false;
  if (live && !dragging_) {
    project_.beginGroup(labelFor(key));
    dragging_ = true;
  }
  return project_.apply(ops, labelFor(key));
}

bool Inspector::edit(const QString& key, const QVariant& value) {
  endDrag();
  return run(key, value, false);
}

bool Inspector::drag(const QString& key, const QVariant& value) { return run(key, value, true); }

void Inspector::endDrag() {
  if (!dragging_) return;
  dragging_ = false;
  project_.endGroup();
}

// ---------- effects ----------

bool Inspector::addEffect(const QString& type) {
  const EffectInfo* info = effectInfo(type);
  if (!info || ids_.empty()) return false;
  endDrag();
  std::vector<Op> ops;
  for (const QString& id : ids_) {
    const Item* item = getItem(project_.doc(), id);
    if (!item || item->type() == ItemType::Audio) continue;
    Effect e;
    e.id = QStringLiteral("fx_") + newId(QStringLiteral("itm")).mid(4);
    e.type = type;
    e.params[QString::fromLatin1(info->param)] = info->def;
    ops.emplace_back(EffectAdd{id, e});
  }
  return !ops.empty() && project_.apply(ops, QStringLiteral("Add effect"));
}

bool Inspector::removeEffect(const QString& effectId) {
  endDrag();
  std::vector<Op> ops;
  for (const QString& id : ids_) {
    const Item* item = getItem(project_.doc(), id);
    if (!item) continue;
    if (std::any_of(item->effects.begin(), item->effects.end(), [&](const Effect& e) { return e.id == effectId; })) ops.emplace_back(EffectRemove{id, effectId});
  }
  return !ops.empty() && project_.apply(ops, QStringLiteral("Remove effect"));
}

bool Inspector::setEffectParam(const QString& effectId, double value, bool live) {
  std::vector<Op> ops;
  for (const QString& id : ids_) {
    const Item* item = getItem(project_.doc(), id);
    if (!item) continue;
    for (const Effect& e : item->effects) {
      if (e.id != effectId) continue;
      const EffectInfo* info = effectInfo(e.type);
      if (!info) continue;
      EffectUpdate u;
      u.itemId = id;
      u.effectId = effectId;
      EffectParams params = e.params;
      params[QString::fromLatin1(info->param)] = value;
      u.patch.params = params;
      ops.emplace_back(u);
    }
  }
  if (ops.empty()) return false;
  if (live && !dragging_) {
    project_.beginGroup(QStringLiteral("Adjust effect"));
    dragging_ = true;
  }
  if (!live) endDrag();
  return project_.apply(ops, QStringLiteral("Adjust effect"));
}

// ---------- project settings ----------

QVariantMap Inspector::projectValues() const {
  const sf::Project& p = project_.doc().project;
  return {{QStringLiteral("name"), p.name},
          {QStringLiteral("width"), static_cast<qlonglong>(p.width)},
          {QStringLiteral("height"), static_cast<qlonglong>(p.height)},
          {QStringLiteral("fps"), static_cast<qlonglong>(p.fps)},
          {QStringLiteral("background"), p.styleConfig.backgroundColor}};
}

bool Inspector::editProject(const QString& key, const QVariant& value) {
  const auto& p = project_.doc().project;
  if (key == QLatin1String("name")) {
    const QString name = value.toString().trimmed();
    if (name.isEmpty() || name == p.name) return false;
    return project_.apply(ProjectRename{name}, QStringLiteral("Rename project"));
  }
  if (key == QLatin1String("width") || key == QLatin1String("height")) {
    ProjectSetCanvas c{p.width, p.height};
    (key == QLatin1String("width") ? c.width : c.height) = value.toLongLong();
    if (c.width == p.width && c.height == p.height) return false;
    return project_.apply(c, QStringLiteral("Set canvas size"));
  }
  if (key == QLatin1String("fps")) {
    const std::int64_t fps = value.toLongLong();
    if (fps == p.fps) return false;
    return project_.apply(ProjectSetFps{fps}, QStringLiteral("Set frame rate"));
  }
  if (key == QLatin1String("background")) {
    StyleConfig sc = p.styleConfig;
    sc.backgroundColor = value.toString();
    if (sc == p.styleConfig) return false;
    return project_.apply(ProjectSetStyleConfig{sc}, QStringLiteral("Set background"));
  }
  return false;
}

} // namespace sf::editor
