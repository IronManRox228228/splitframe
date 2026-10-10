#include "core/ops.h"

#include <QDateTime>
#include <QJsonArray>

namespace sf {

namespace {

constexpr IntBounds kIntNonNeg{.min = 0};

QString q(const char* s) { return QString::fromLatin1(s); }

template <class... Ts> struct Overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> Overloaded(Ts...) -> Overloaded<Ts...>;

QJsonArray strings(const std::vector<QString>& v) {
  QJsonArray a;
  for (const QString& s : v) a.append(s);
  return a;
}

std::vector<QString> parseStrings(const Rd& r) {
  return r.list([](const Rd& x) { return x.str(); });
}

// itemAddLoose items inside restoreTimeline must still carry props when the type has required
// fields (parseItem hands back a placeholder otherwise).
Item parseLooseItem(const Rd& r, bool* omitted) {
  Item it = parseItem(r, true, omitted);
  const ItemType t = it.type();
  if (*omitted && t != ItemType::Video && t != ItemType::Audio && t != ItemType::Image)
    r.field(QStringLiteral("props")).fail(QStringLiteral("Required"));
  return it;
}

TrackPatch parseTrackPatch(const Rd& r) {
  r.requireObject();
  TrackPatch p;
  p.name = r.field(QStringLiteral("name")).optStr();
  p.locked = r.field(QStringLiteral("locked")).optBool();
  p.muted = r.field(QStringLiteral("muted")).optBool();
  p.hidden = r.field(QStringLiteral("hidden")).optBool();
  return p;
}

} // namespace

QString Op::type() const {
  return std::visit(
      Overloaded{
          [](const ProjectRename&) { return q("project.rename"); },
          [](const ProjectSetCanvas&) { return q("project.setCanvas"); },
          [](const ProjectSetFps&) { return q("project.setFps"); },
          [](const ProjectRestoreTimeline&) { return q("project.restoreTimeline"); },
          [](const ProjectSetStyleConfig&) { return q("project.setStyleConfig"); },
          [](const ProjectSetReference&) { return q("project.setReference"); },
          [](const TrackAdd&) { return q("track.add"); },
          [](const TrackRemove&) { return q("track.remove"); },
          [](const TrackUpdate&) { return q("track.update"); },
          [](const TrackReorder&) { return q("track.reorder"); },
          [](const ItemAdd&) { return q("item.add"); },
          [](const ItemRemove&) { return q("item.remove"); },
          [](const ItemUpdate&) { return q("item.update"); },
          [](const ItemMove&) { return q("item.move"); },
          [](const ItemTrim&) { return q("item.trim"); },
          [](const ItemSplit&) { return q("item.split"); },
          [](const ItemClone&) { return q("item.clone"); },
          [](const ItemSlip&) { return q("item.slip"); },
          [](const ItemSetSpeed&) { return q("item.setSpeed"); },
          [](const ItemSetTimeRemap&) { return q("item.setTimeRemap"); },
          [](const ItemSetKeyframes&) { return q("item.setKeyframes"); },
          [](const EffectAdd&) { return q("effect.add"); },
          [](const EffectRemove&) { return q("effect.remove"); },
          [](const EffectUpdate&) { return q("effect.update"); },
          [](const MaskAdd&) { return q("mask.add"); },
          [](const MaskRemove&) { return q("mask.remove"); },
          [](const MarkerAdd&) { return q("marker.add"); },
          [](const MarkerRemove&) { return q("marker.remove"); },
          [](const MarkerUpdate&) { return q("marker.update"); },
          [](const BatchOp&) { return q("batch"); },
          [](const MixerSet&) { return q("mixer.set"); },
      },
      body);
}

// ---------- parse ----------

Op parseOp(const Rd& r) {
  r.requireObject();
  const Rd typeRd = r.field(QStringLiteral("type"));
  const QString type = typeRd.str();
  const auto f = [&](const char* key) { return r.field(QString::fromLatin1(key)); };

  if (type == q("project.rename")) {
    ProjectRename o;
    const Rd n = f("name");
    o.name = n.str();
    if (o.name.isEmpty()) n.fail(QStringLiteral("String must contain at least 1 character(s)"));
    return o;
  }
  if (type == q("project.setCanvas")) {
    ProjectSetCanvas o;
    o.width = f("width").integer({.min = 1});
    o.height = f("height").integer({.min = 1});
    return o;
  }
  if (type == q("project.setFps")) {
    ProjectSetFps o;
    o.fps = f("fps").integer({.min = 1, .max = 120});
    return o;
  }
  if (type == q("project.restoreTimeline")) {
    ProjectRestoreTimeline o;
    o.fps = f("fps").integer({.min = 1, .max = 120});
    o.items = f("items").list([](const Rd& x) {
      bool omitted = false;
      return parseLooseItem(x, &omitted);
    });
    o.markers = f("markers").list(parseMarker);
    return o;
  }
  if (type == q("project.setStyleConfig")) {
    ProjectSetStyleConfig o;
    const Rd s = f("styleConfig");
    if (s.missing()) s.fail(QStringLiteral("Required"));
    o.styleConfig = parseStyleConfig(s);
    return o;
  }
  if (type == q("project.setReference")) {
    ProjectSetReference o;
    const Rd a = f("assetId");
    if (a.raw().isNull()) o.assetId = std::nullopt;
    else o.assetId = a.str();
    return o;
  }
  if (type == q("track.add")) {
    TrackAdd o;
    o.trackId = f("trackId").str();
    o.kind = f("kind").enumeration<TrackKind>();
    o.name = f("name").str();
    o.locked = f("locked").boolOr(false);
    o.muted = f("muted").boolOr(false);
    o.hidden = f("hidden").boolOr(false);
    o.index = f("index").optInteger(kIntNonNeg);
    return o;
  }
  if (type == q("mixer.set")) {
    MixerSet o;
    const Rd m = f("mixer");
    if (!m.missing() && !m.raw().isNull()) o.mixer = parseMixer(m);
    return o;
  }
  if (type == q("track.remove")) {
    TrackRemove o;
    o.trackId = f("trackId").str();
    return o;
  }
  if (type == q("track.update")) {
    TrackUpdate o;
    o.trackId = f("trackId").str();
    o.patch = parseTrackPatch(f("patch"));
    return o;
  }
  if (type == q("track.reorder")) {
    TrackReorder o;
    o.trackIds = parseStrings(f("trackIds"));
    return o;
  }
  if (type == q("item.add")) {
    ItemAdd o;
    o.item = parseItem(f("item"), true, &o.propsOmitted);
    return o;
  }
  if (type == q("item.remove")) {
    ItemRemove o;
    const Rd ids = f("itemIds");
    o.itemIds = parseStrings(ids);
    if (o.itemIds.empty()) ids.fail(QStringLiteral("Array must contain at least 1 element(s)"));
    o.ripple = f("ripple").boolOr(false);
    return o;
  }
  if (type == q("item.update")) {
    ItemUpdate o;
    o.itemId = f("itemId").str();
    o.patch = parseItemPatch(f("patch"));
    return o;
  }
  if (type == q("item.move")) {
    ItemMove o;
    o.itemId = f("itemId").str();
    o.trackId = f("trackId").optStr();
    o.startFrame = f("startFrame").optInteger();
    return o;
  }
  if (type == q("item.trim")) {
    ItemTrim o;
    o.itemId = f("itemId").str();
    o.edge = f("edge").enumeration<TrimEdge>();
    o.frame = f("frame").integer();
    o.ripple = f("ripple").boolOr(false);
    return o;
  }
  if (type == q("item.split")) {
    ItemSplit o;
    o.itemId = f("itemId").str();
    o.atFrame = f("atFrame").integer();
    o.newItemId = f("newItemId").str();
    return o;
  }
  if (type == q("item.clone")) {
    ItemClone o;
    o.itemId = f("itemId").str();
    o.newItemId = f("newItemId").str();
    o.trackId = f("trackId").optStr();
    o.startFrame = f("startFrame").optInteger();
    return o;
  }
  if (type == q("item.slip")) {
    ItemSlip o;
    o.itemId = f("itemId").str();
    o.sourceInFrame = f("sourceInFrame").integer();
    return o;
  }
  if (type == q("item.setSpeed")) {
    ItemSetSpeed o;
    o.itemId = f("itemId").str();
    o.speed = f("speed").num({.min = 0.1, .max = 16.0});
    return o;
  }
  if (type == q("item.setTimeRemap")) {
    ItemSetTimeRemap o;
    o.itemId = f("itemId").str();
    o.points = f("points").list(parseTimeRemapPoint);
    return o;
  }
  if (type == q("item.setKeyframes")) {
    ItemSetKeyframes o;
    o.itemId = f("itemId").str();
    o.property = f("property").str();
    o.keyframes = f("keyframes").list(parseKeyframe);
    return o;
  }
  if (type == q("effect.add")) {
    EffectAdd o;
    o.itemId = f("itemId").str();
    o.effect = parseEffect(f("effect"));
    return o;
  }
  if (type == q("effect.remove")) {
    EffectRemove o;
    o.itemId = f("itemId").str();
    o.effectId = f("effectId").str();
    return o;
  }
  if (type == q("effect.update")) {
    EffectUpdate o;
    o.itemId = f("itemId").str();
    o.effectId = f("effectId").str();
    const Rd p = f("patch");
    p.requireObject();
    o.patch.type = p.field(QStringLiteral("type")).optStr();
    const Rd params = p.field(QStringLiteral("params"));
    if (!params.missing()) o.patch.params = parseEffectParams(params);
    return o;
  }
  if (type == q("mask.add")) {
    MaskAdd o;
    o.itemId = f("itemId").str();
    o.mask = parseMask(f("mask"));
    return o;
  }
  if (type == q("mask.remove")) {
    MaskRemove o;
    o.itemId = f("itemId").str();
    o.maskId = f("maskId").str();
    return o;
  }
  if (type == q("marker.add")) {
    MarkerAdd o;
    o.marker = parseMarker(f("marker"));
    return o;
  }
  if (type == q("marker.remove")) {
    MarkerRemove o;
    o.markerId = f("markerId").str();
    return o;
  }
  if (type == q("marker.update")) {
    MarkerUpdate o;
    o.markerId = f("markerId").str();
    const Rd p = f("patch");
    p.requireObject();
    o.patch.frame = p.field(QStringLiteral("frame")).optInteger();
    o.patch.label = p.field(QStringLiteral("label")).optStr();
    o.patch.color = p.field(QStringLiteral("color")).optStr();
    return o;
  }
  if (type == q("batch")) {
    BatchOp o;
    const Rd ops = f("ops");
    for (const Rd& e : ops.items()) o.ops.push_back(parseOp(e));
    if (o.ops.empty()) ops.fail(QStringLiteral("Array must contain at least 1 element(s)"));
    return o;
  }
  typeRd.fail(QStringLiteral("Invalid discriminator value: unknown op type '%1'").arg(type));
}

// ---------- write ----------

namespace {

QJsonObject tagged(const char* type, QJsonObject o) {
  o.insert(QStringLiteral("type"), q(type));
  return o;
}

QJsonObject trackPatchJson(const TrackPatch& p) {
  QJsonObject o;
  if (p.name) o.insert(QStringLiteral("name"), *p.name);
  if (p.locked) o.insert(QStringLiteral("locked"), *p.locked);
  if (p.muted) o.insert(QStringLiteral("muted"), *p.muted);
  if (p.hidden) o.insert(QStringLiteral("hidden"), *p.hidden);
  return o;
}

QJsonValue jf(Frame v) { return QJsonValue(static_cast<qint64>(v)); }

} // namespace

QJsonObject toJson(const Op& op) {
  return std::visit(
      Overloaded{
          [](const ProjectRename& o) { return tagged("project.rename", {{q("name"), o.name}}); },
          [](const ProjectSetCanvas& o) {
            return tagged("project.setCanvas", {{q("width"), jf(o.width)}, {q("height"), jf(o.height)}});
          },
          [](const ProjectSetFps& o) { return tagged("project.setFps", {{q("fps"), jf(o.fps)}}); },
          [](const ProjectRestoreTimeline& o) {
            QJsonArray items, markers;
            for (const Item& i : o.items) items.append(toJson(i));
            for (const Marker& m : o.markers) markers.append(toJson(m));
            return tagged("project.restoreTimeline", {{q("fps"), jf(o.fps)}, {q("items"), items}, {q("markers"), markers}});
          },
          [](const ProjectSetStyleConfig& o) {
            return tagged("project.setStyleConfig", {{q("styleConfig"), toJson(o.styleConfig)}});
          },
          [](const ProjectSetReference& o) {
            return tagged("project.setReference", {{q("assetId"), o.assetId ? QJsonValue(*o.assetId) : QJsonValue(QJsonValue::Null)}});
          },
          [](const TrackAdd& o) {
            QJsonObject j{{q("trackId"), o.trackId}, {q("kind"), enumName(o.kind)}, {q("name"), o.name},
                          {q("locked"), o.locked},   {q("muted"), o.muted},         {q("hidden"), o.hidden}};
            if (o.index) j.insert(q("index"), jf(*o.index));
            return tagged("track.add", j);
          },
          [](const TrackRemove& o) { return tagged("track.remove", {{q("trackId"), o.trackId}}); },
          [](const TrackUpdate& o) {
            return tagged("track.update", {{q("trackId"), o.trackId}, {q("patch"), trackPatchJson(o.patch)}});
          },
          [](const TrackReorder& o) { return tagged("track.reorder", {{q("trackIds"), strings(o.trackIds)}}); },
          [](const ItemAdd& o) { return tagged("item.add", {{q("item"), toJson(o.item, o.propsOmitted)}}); },
          [](const ItemRemove& o) {
            return tagged("item.remove", {{q("itemIds"), strings(o.itemIds)}, {q("ripple"), o.ripple}});
          },
          [](const ItemUpdate& o) {
            return tagged("item.update", {{q("itemId"), o.itemId}, {q("patch"), toJson(o.patch)}});
          },
          [](const ItemMove& o) {
            QJsonObject j{{q("itemId"), o.itemId}};
            if (o.trackId) j.insert(q("trackId"), *o.trackId);
            if (o.startFrame) j.insert(q("startFrame"), jf(*o.startFrame));
            return tagged("item.move", j);
          },
          [](const ItemTrim& o) {
            return tagged("item.trim", {{q("itemId"), o.itemId},
                                        {q("edge"), enumName(o.edge)},
                                        {q("frame"), jf(o.frame)},
                                        {q("ripple"), o.ripple}});
          },
          [](const ItemSplit& o) {
            return tagged("item.split", {{q("itemId"), o.itemId}, {q("atFrame"), jf(o.atFrame)}, {q("newItemId"), o.newItemId}});
          },
          [](const ItemClone& o) {
            QJsonObject j{{q("itemId"), o.itemId}, {q("newItemId"), o.newItemId}};
            if (o.trackId) j.insert(q("trackId"), *o.trackId);
            if (o.startFrame) j.insert(q("startFrame"), jf(*o.startFrame));
            return tagged("item.clone", j);
          },
          [](const ItemSlip& o) {
            return tagged("item.slip", {{q("itemId"), o.itemId}, {q("sourceInFrame"), jf(o.sourceInFrame)}});
          },
          [](const ItemSetSpeed& o) { return tagged("item.setSpeed", {{q("itemId"), o.itemId}, {q("speed"), o.speed}}); },
          [](const ItemSetTimeRemap& o) {
            QJsonArray pts;
            for (const TimeRemapPoint& p : o.points) pts.append(toJson(p));
            return tagged("item.setTimeRemap", {{q("itemId"), o.itemId}, {q("points"), pts}});
          },
          [](const ItemSetKeyframes& o) {
            QJsonArray kfs;
            for (const Keyframe& k : o.keyframes) kfs.append(toJson(k));
            return tagged("item.setKeyframes", {{q("itemId"), o.itemId}, {q("property"), o.property}, {q("keyframes"), kfs}});
          },
          [](const EffectAdd& o) { return tagged("effect.add", {{q("itemId"), o.itemId}, {q("effect"), toJson(o.effect)}}); },
          [](const EffectRemove& o) {
            return tagged("effect.remove", {{q("itemId"), o.itemId}, {q("effectId"), o.effectId}});
          },
          [](const EffectUpdate& o) {
            QJsonObject patch;
            if (o.patch.type) patch.insert(q("type"), *o.patch.type);
            if (o.patch.params) patch.insert(q("params"), toJson(*o.patch.params));
            return tagged("effect.update", {{q("itemId"), o.itemId}, {q("effectId"), o.effectId}, {q("patch"), patch}});
          },
          [](const MaskAdd& o) { return tagged("mask.add", {{q("itemId"), o.itemId}, {q("mask"), toJson(o.mask)}}); },
          [](const MaskRemove& o) { return tagged("mask.remove", {{q("itemId"), o.itemId}, {q("maskId"), o.maskId}}); },
          [](const MarkerAdd& o) { return tagged("marker.add", {{q("marker"), toJson(o.marker)}}); },
          [](const MarkerRemove& o) { return tagged("marker.remove", {{q("markerId"), o.markerId}}); },
          [](const MarkerUpdate& o) {
            QJsonObject patch;
            if (o.patch.frame) patch.insert(q("frame"), jf(*o.patch.frame));
            if (o.patch.label) patch.insert(q("label"), *o.patch.label);
            if (o.patch.color) patch.insert(q("color"), *o.patch.color);
            return tagged("marker.update", {{q("markerId"), o.markerId}, {q("patch"), patch}});
          },
          [](const MixerSet& o) {
            return tagged("mixer.set", {{q("mixer"), o.mixer ? QJsonValue(toJson(*o.mixer)) : QJsonValue(QJsonValue::Null)}});
          },
          [](const BatchOp& o) {
            QJsonArray ops;
            for (const Op& e : o.ops) ops.append(toJson(e));
            return tagged("batch", {{q("ops"), ops}});
          },
      },
      op.body);
}

bool operator==(const Op& a, const Op& b) { return toJson(a) == toJson(b); }

void validateOp(const Op& op) { (void)parseOp(Rd(toJson(op))); }

OpEntry makeOpEntry(const Op& op, std::int64_t seq, const QString& actor) {
  return {seq, actor, op, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)};
}

OpEntry parseOpEntry(const Rd& r) {
  r.requireObject();
  OpEntry e;
  e.seq = r.field(QStringLiteral("seq")).integer();
  e.actor = r.field(QStringLiteral("actor")).str();
  e.op = parseOp(r.field(QStringLiteral("op")));
  e.createdAt = r.field(QStringLiteral("createdAt")).str();
  return e;
}

QJsonObject toJson(const OpEntry& e) {
  return {{q("seq"), jf(e.seq)}, {q("actor"), e.actor}, {q("op"), toJson(e.op)}, {q("createdAt"), e.createdAt}};
}

} // namespace sf
