#include "render/effects.h"

#include "core/timeline_doc.h"
#include "render/color.h"
#include "render/fx_textures.h"
#include "render/layout.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>

#include <algorithm>
#include <array>
#include <cmath>

namespace sf::render {

namespace {

QString L(const char* s) { return QString::fromLatin1(s); }

struct KindName {
  FxParamType type;
  const char* name;
};
constexpr KindName kTypeNames[] = {{FxParamType::Float, "float"}, {FxParamType::Vec2, "vec2"}, {FxParamType::Color, "color"}, {FxParamType::Bool, "bool"},
                                   {FxParamType::Int, "int"},     {FxParamType::Enum, "enum"}, {FxParamType::Point, "point"}, {FxParamType::Texture, "texture"}};

QString typeName(FxParamType t) {
  for (const KindName& k : kTypeNames)
    if (k.type == t) return L(k.name);
  return L("float");
}

bool typeFromName(const QString& s, FxParamType* out) {
  for (const KindName& k : kTypeNames)
    if (s == QLatin1String(k.name)) {
      *out = k.type;
      return true;
    }
  return false;
}

QStringList strings(const QJsonValue& v) {
  QStringList out;
  for (const QJsonValue& e : v.toArray()) out << e.toString();
  return out;
}

// "#rrggbb", "#rrggbbaa" (CSS order) or a colour name -> straight sRGB 0..1
std::optional<std::array<double, 4>> colorFromString(const QString& s) {
  QString t = s.trimmed();
  double a = 1;
  if (t.startsWith(QLatin1Char('#')) && t.size() == 9) {
    bool ok = false;
    const int alpha = t.mid(7, 2).toInt(&ok, 16);
    if (!ok) return std::nullopt;
    a = alpha / 255.0;
    t = t.left(7);
  }
  const QColor c(t);
  if (!c.isValid()) return std::nullopt;
  return std::array<double, 4>{c.redF(), c.greenF(), c.blueF(), a};
}

// A colour param's JSON form ("#rrggbb" or [r, g, b, a?]) as straight sRGB 0..1
std::optional<std::array<double, 4>> colorFromJson(const QJsonValue& v) {
  if (v.isString()) return colorFromString(v.toString());
  if (!v.isArray()) return std::nullopt;
  const QJsonArray a = v.toArray();
  if (a.size() != 3 && a.size() != 4) return std::nullopt;
  std::array<double, 4> c{0, 0, 0, 1};
  for (qsizetype i = 0; i < a.size(); ++i) {
    if (!a.at(i).isDouble()) return std::nullopt;
    c[static_cast<size_t>(i)] = std::clamp(a.at(i).toDouble(), 0.0, 1.0);
  }
  return c;
}

std::optional<std::array<double, 2>> pairFromJson(const QJsonValue& v) {
  if (!v.isArray()) return std::nullopt;
  const QJsonArray a = v.toArray();
  if (a.size() != 2 || !a.at(0).isDouble() || !a.at(1).isDouble()) return std::nullopt;
  return std::array<double, 2>{a.at(0).toDouble(), a.at(1).toDouble()};
}

QJsonArray arrayOf(std::initializer_list<double> v) {
  QJsonArray a;
  for (double x : v) a.append(x);
  return a;
}

int enumIndex(const FxParamDef& p, const QJsonValue& v) {
  if (v.isString()) return static_cast<int>(p.options.indexOf(v.toString()));
  if (v.isDouble()) {
    const int i = static_cast<int>(std::lround(v.toDouble()));
    return i >= 0 && i < p.options.size() ? i : -1;
  }
  return -1;
}

// The default in JSON form when the registry gives none
QJsonValue defaultFor(const FxParamDef& p) {
  switch (p.type) {
  case FxParamType::Float: return std::clamp(0.0, p.min, p.max);
  case FxParamType::Int: return std::clamp(0.0, p.min, p.max);
  case FxParamType::Bool: return false;
  case FxParamType::Enum: return p.options.isEmpty() ? QString() : p.options.first();
  case FxParamType::Texture: return p.options.isEmpty() ? QStringLiteral("linear_lr") : p.options.first();
  case FxParamType::Vec2: return arrayOf({0, 0});
  case FxParamType::Point: return arrayOf({0.5, 0.5});
  case FxParamType::Color: return arrayOf({1, 1, 1, 1});
  }
  return 0.0;
}

bool validValue(const FxParamDef& p, const QJsonValue& v, QJsonValue* clean, QString* why) {
  const auto bad = [&](const char* expected) {
    if (why) *why = QStringLiteral("param \"%1\" (%2) expects %3").arg(p.name, typeName(p.type), L(expected));
    return false;
  };
  switch (p.type) {
  case FxParamType::Float:
    if (!v.isDouble()) return bad("a number");
    *clean = std::clamp(v.toDouble(), p.min, p.max);
    return true;
  case FxParamType::Int:
    if (!v.isDouble()) return bad("an integer");
    *clean = std::clamp(std::round(v.toDouble()), p.min, p.max);
    return true;
  case FxParamType::Bool:
    if (!v.isBool()) return bad("true or false");
    *clean = v;
    return true;
  case FxParamType::Enum: {
    const int i = enumIndex(p, v);
    if (i < 0) {
      if (why) *why = QStringLiteral("param \"%1\" expects one of: %2").arg(p.name, p.options.join(QStringLiteral(", ")));
      return false;
    }
    *clean = p.options.at(i);
    return true;
  }
  case FxParamType::Texture: {
    if (!v.isString()) return bad("a texture name");
    const QString s = v.toString();
    if (!proceduralTextureNames().contains(s)) {
      if (why) *why = QStringLiteral("param \"%1\" expects one of: %2").arg(p.name, proceduralTextureNames().join(QStringLiteral(", ")));
      return false;
    }
    *clean = s;
    return true;
  }
  case FxParamType::Vec2: {
    const auto pr = pairFromJson(v);
    if (!pr) return bad("[x, y]");
    *clean = arrayOf({std::clamp((*pr)[0], p.min, p.max), std::clamp((*pr)[1], p.min, p.max)});
    return true;
  }
  case FxParamType::Point: {
    const auto pr = pairFromJson(v);
    if (!pr) return bad("[x, y] in 0..1, origin top-left");
    *clean = arrayOf({std::clamp((*pr)[0], 0.0, 1.0), std::clamp((*pr)[1], 0.0, 1.0)});
    return true;
  }
  case FxParamType::Color: {
    const auto c = colorFromJson(v);
    if (!c) return bad("\"#rrggbb\" or [r, g, b, a] in 0..1");
    *clean = v.isString() ? v : QJsonValue(arrayOf({(*c)[0], (*c)[1], (*c)[2], (*c)[3]}));
    return true;
  }
  }
  return false;
}

// "name.x" -> ("name", "x")
bool splitComponent(const QString& key, QString* name, QString* suffix) {
  const qsizetype dot = key.indexOf(QLatin1Char('.'));
  if (dot < 0) return false;
  *name = key.left(dot);
  *suffix = key.mid(dot + 1);
  return true;
}

int componentCount(FxParamType t) {
  switch (t) {
  case FxParamType::Vec2:
  case FxParamType::Point: return 2;
  case FxParamType::Color: return 4;
  default: return 1;
  }
}

// Component of a vector param named by `suffix` (x y for vec2 / point, r g b a for colour), or -1
int componentIndex(FxParamType t, const QString& suffix) {
  static const QStringList xy{L("x"), L("y")};
  static const QStringList rgba{L("r"), L("g"), L("b"), L("a")};
  if (t == FxParamType::Vec2 || t == FxParamType::Point) return static_cast<int>(xy.indexOf(suffix));
  if (t == FxParamType::Color) return static_cast<int>(rgba.indexOf(suffix));
  return -1;
}

bool loadDef(const QJsonObject& o, FxDef* def, QString* error) {
  const auto fail = [&](const QString& why) {
    if (error) *error = QStringLiteral("effect \"%1\": %2").arg(o.value(L("id")).toString(), why);
    return false;
  };
  def->id = o.value(L("id")).toString();
  static const QRegularExpression idRe(QStringLiteral("^[a-z][a-z0-9_]*$"));
  if (!idRe.match(def->id).hasMatch()) return fail(QStringLiteral("id must be lowercase letters, digits and underscores"));
  def->name = o.value(L("name")).toString(def->id);
  def->category = o.value(L("category")).toString(QStringLiteral("other"));
  const QString kind = o.value(L("kind")).toString();
  if (kind == QLatin1String("filter")) def->kind = FxKind::Filter;
  else if (kind == QLatin1String("transition")) def->kind = FxKind::Transition;
  else return fail(QStringLiteral("kind must be \"filter\" or \"transition\""));
  def->description = o.value(L("description")).toString();
  def->tags = strings(o.value(L("tags")));
  def->whenToUse = strings(o.value(L("whenToUse")));
  def->passes = std::max(1, o.value(L("passes")).toInt(1));
  if (def->kind == FxKind::Transition) def->passes = 1;
  def->shader = QStringLiteral(":/sf/render/fx/shaders/%1.frag.qsb").arg(def->id);
  const QJsonArray params = o.value(L("params")).toArray();
  if (params.size() > kFxMaxParams) return fail(QStringLiteral("more than %1 params").arg(kFxMaxParams));
  for (const QJsonValue& pv : params) {
    const QJsonObject po = pv.toObject();
    FxParamDef p;
    p.name = po.value(L("name")).toString();
    if (p.name.isEmpty()) return fail(QStringLiteral("a param has no name"));
    for (const FxParamDef& other : def->params)
      if (other.name == p.name) return fail(QStringLiteral("duplicate param \"%1\"").arg(p.name));
    p.label = po.value(L("label")).toString(p.name);
    if (!typeFromName(po.value(L("type")).toString(), &p.type)) return fail(QStringLiteral("param \"%1\": unknown type").arg(p.name));
    p.min = po.value(L("min")).toDouble(0);
    p.max = po.value(L("max")).toDouble(1);
    p.options = strings(po.value(L("options")));
    p.description = po.value(L("description")).toString();
    if (p.type == FxParamType::Enum && p.options.isEmpty()) return fail(QStringLiteral("enum param \"%1\" needs options").arg(p.name));
    if (p.type == FxParamType::Texture) {
      for (const FxParamDef& other : def->params)
        if (other.type == FxParamType::Texture) return fail(QStringLiteral("at most 1 texture param per effect"));
      p.options = proceduralTextureNames();
    }
    p.def = po.contains(L("default")) ? po.value(L("default")) : defaultFor(p);
    QJsonValue clean;
    QString why;
    if (!validValue(p, p.def, &clean, &why)) return fail(QStringLiteral("default of %1").arg(why));
    p.def = clean;
    def->params.push_back(std::move(p));
  }
  return true;
}

} // namespace

const FxParamDef* FxDef::param(const QString& n) const {
  for (const FxParamDef& p : params)
    if (p.name == n) return &p;
  return nullptr;
}

const FxParamDef* FxDef::textureParam() const {
  for (const FxParamDef& p : params)
    if (p.type == FxParamType::Texture) return &p;
  return nullptr;
}

bool parseFxDefs(const QJsonObject& root, std::vector<FxDef>* out, QString* error) {
  for (const QJsonValue& v : root.value(L("effects")).toArray()) {
    FxDef def;
    if (!loadDef(v.toObject(), &def, error)) return false;
    for (const FxDef& d : *out)
      if (d.id == def.id) {
        if (error) *error = QStringLiteral("duplicate effect id \"%1\"").arg(def.id);
        return false;
      }
    out->push_back(std::move(def));
  }
  return true;
}

FxRegistry::FxRegistry() {
  QDir dir(QStringLiteral(":/sf/render/fx/registry"));
  for (const QString& name : dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
    QFile f(dir.filePath(name));
    if (!f.open(QIODevice::ReadOnly)) continue;
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (!doc.isObject()) {
      error_ = QStringLiteral("%1: %2").arg(name, pe.errorString());
      continue;
    }
    QString err;
    if (!parseFxDefs(doc.object(), &defs_, &err)) error_ = QStringLiteral("%1: %2").arg(name, err);
  }
}

const FxRegistry& FxRegistry::instance() {
  static const FxRegistry registry;
  return registry;
}

const FxDef* FxRegistry::find(const QString& id) const {
  for (const FxDef& d : defs_)
    if (d.id == id) return &d;
  return nullptr;
}

QJsonObject fxDefJson(const FxDef& def) {
  QJsonArray params;
  for (size_t i = 0; i < def.params.size(); ++i) {
    const FxParamDef& p = def.params[i];
    QJsonObject o{{L("name"), p.name}, {L("label"), p.label}, {L("type"), typeName(p.type)}, {L("default"), p.def}, {L("slot"), static_cast<int>(i)}};
    if (p.type == FxParamType::Float || p.type == FxParamType::Int || p.type == FxParamType::Vec2) {
      o.insert(L("min"), p.min);
      o.insert(L("max"), p.max);
    }
    if (p.type == FxParamType::Enum || p.type == FxParamType::Texture) o.insert(L("options"), QJsonArray::fromStringList(p.options));
    if (!p.description.isEmpty()) o.insert(L("description"), p.description);
    o.insert(L("animatable"), p.animatable());
    params.append(o);
  }
  return QJsonObject{{L("id"), def.id},
                     {L("name"), def.name},
                     {L("category"), def.category},
                     {L("kind"), def.kind == FxKind::Filter ? L("filter") : L("transition")},
                     {L("description"), def.description},
                     {L("tags"), QJsonArray::fromStringList(def.tags)},
                     {L("whenToUse"), QJsonArray::fromStringList(def.whenToUse)},
                     {L("passes"), def.passes},
                     {L("params"), params}};
}

bool normalizeFxParams(const FxDef& def, const QJsonObject& given, QJsonObject* normalised, QString* error) {
  QJsonObject out;
  for (auto it = given.begin(); it != given.end(); ++it) {
    const FxParamDef* p = def.param(it.key());
    if (!p) {
      if (error) {
        QStringList names;
        for (const FxParamDef& q : def.params) names << q.name;
        *error = QStringLiteral("effect \"%1\" has no param \"%2\" (params: %3)").arg(def.id, it.key(), names.isEmpty() ? QStringLiteral("none") : names.join(QStringLiteral(", ")));
      }
      return false;
    }
    QJsonValue clean;
    if (!validValue(*p, it.value(), &clean, error)) return false;
    out.insert(it.key(), clean);
  }
  if (normalised) *normalised = out;
  return true;
}

bool fxKeyframeKeyOk(const FxDef& def, const QString& key, QString* error) {
  QString name, suffix;
  const bool component = splitComponent(key, &name, &suffix);
  const FxParamDef* p = def.param(component ? name : key);
  const auto fail = [&](const QString& why) {
    if (error) *error = why;
    return false;
  };
  if (!p) return fail(QStringLiteral("effect \"%1\" has no param \"%2\"").arg(def.id, component ? name : key));
  if (!component) {
    if (p->type == FxParamType::Float || p->type == FxParamType::Int) return true;
    return fail(QStringLiteral("param \"%1\" is a %2: animate one component as \"%1.x\" / \"%1.y\" (vec2, point) or \"%1.r\" ... \"%1.a\" (color)").arg(p->name, typeName(p->type)));
  }
  if (componentCount(p->type) == 1) return fail(QStringLiteral("param \"%1\" has no components").arg(p->name));
  if (componentIndex(p->type, suffix) < 0) return fail(QStringLiteral("param \"%1\" has no component \"%2\"").arg(p->name, suffix));
  return true;
}

QJsonObject resolveFxParams(const FxDef& def, const QJsonObject& given, const KeyframeMap& keyframes, Frame localFrame) {
  QJsonObject out;
  for (const FxParamDef& p : def.params) {
    QJsonValue v = given.contains(p.name) ? given.value(p.name) : p.def;
    if (const auto kf = keyframes.find(p.name); kf != keyframes.end() && !kf->second.empty() && (p.type == FxParamType::Float || p.type == FxParamType::Int)) {
      double x = resolveKeyframes(kf->second, localFrame);
      x = std::clamp(x, p.min, p.max);
      v = p.type == FxParamType::Int ? std::round(x) : x;
    }
    const int n = componentCount(p.type);
    if (n > 1 && p.animatable()) {
      bool any = false;
      for (int c = 0; c < n; ++c) {
        static const char* const names[4] = {"x", "y", "z", "w"};
        static const char* const rgba[4] = {"r", "g", "b", "a"};
        const QString key = p.name + QLatin1Char('.') + L(p.type == FxParamType::Color ? rgba[c] : names[c]);
        if (const auto kf = keyframes.find(key); kf != keyframes.end() && !kf->second.empty()) {
          if (!any) {
            // expand to numbers first (a colour may be a "#rrggbb" string)
            QJsonArray base;
            if (p.type == FxParamType::Color) {
              const auto col = colorFromJson(v).value_or(std::array<double, 4>{1, 1, 1, 1});
              for (double d : col) base.append(d);
            } else {
              const auto pr = pairFromJson(v).value_or(std::array<double, 2>{0, 0});
              base = arrayOf({pr[0], pr[1]});
            }
            v = base;
            any = true;
          }
          QJsonArray a = v.toArray();
          a[c] = resolveKeyframes(kf->second, localFrame);
          v = a;
        }
      }
    }
    out.insert(p.name, v);
  }
  return out;
}

bool transitionWindow(const TimelineDoc& doc, const FxTransition& tr, const Item** from, const Item** to, Frame* start, Frame* end) {
  const Item* a = getItem(doc, tr.fromItemId);
  const Item* b = getItem(doc, tr.toItemId);
  if (!a || !b || a->trackId != b->trackId || b->startFrame != itemEnd(*a)) return false;
  const Frame cut = itemEnd(*a);
  const Frame d = std::max<Frame>(1, tr.duration);
  Frame s = cut;
  if (tr.align == QLatin1String("center")) s = cut - d / 2;
  else if (tr.align == QLatin1String("end")) s = cut - d;
  if (from) *from = a;
  if (to) *to = b;
  if (start) *start = s;
  if (end) *end = s + d;
  return true;
}

std::vector<ActiveTransition> activeTransitions(const TimelineDoc& doc, Frame frame) {
  std::vector<ActiveTransition> out;
  for (const FxTransition& tr : doc.transitions) {
    ActiveTransition a;
    if (!transitionWindow(doc, tr, &a.from, &a.to, &a.start, &a.end) || frame < a.start || frame >= a.end) continue;
    a.tr = &tr;
    a.progress = static_cast<double>(frame - a.start) / static_cast<double>(a.end - a.start);
    out.push_back(a);
  }
  return out;
}

void packFxParams(const FxDef& def, const QJsonObject& resolved, bool linearColor, float (&outSlots)[kFxMaxParams][4]) {
  for (auto& s : outSlots)
    for (float& f : s) f = 0;
  for (size_t i = 0; i < def.params.size() && i < static_cast<size_t>(kFxMaxParams); ++i) {
    const FxParamDef& p = def.params[i];
    const QJsonValue v = resolved.contains(p.name) ? resolved.value(p.name) : p.def;
    float* s = outSlots[i];
    switch (p.type) {
    case FxParamType::Float:
    case FxParamType::Int: s[0] = static_cast<float>(v.toDouble()); break;
    case FxParamType::Bool: s[0] = v.toBool() ? 1.0f : 0.0f; break;
    case FxParamType::Enum: s[0] = static_cast<float>(std::max(0, enumIndex(p, v))); break;
    case FxParamType::Vec2:
    case FxParamType::Point: {
      const auto pr = pairFromJson(v).value_or(std::array<double, 2>{0, 0});
      s[0] = static_cast<float>(pr[0]);
      s[1] = static_cast<float>(p.type == FxParamType::Point ? 1.0 - pr[1] : pr[1]);
      break;
    }
    case FxParamType::Color: {
      const auto c = colorFromJson(v).value_or(std::array<double, 4>{1, 1, 1, 1});
      for (size_t k = 0; k < 3; ++k) s[k] = static_cast<float>(linearColor ? toLinear(Transfer::Srgb, c[k]) : c[k]);
      s[3] = static_cast<float>(c[3]);
      break;
    }
    case FxParamType::Texture: break;
    }
  }
}

} // namespace sf::render
