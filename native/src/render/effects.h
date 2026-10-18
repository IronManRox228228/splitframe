#pragma once

// GPU effects: data-driven filters (one picture in) and transitions (two pictures plus progress in).
//
// An effect is a definition (id, name, category, kind, tags, a parameter schema) in a registry JSON
// (render/fx/registry/*.json) and a fragment shader generated from a body file (render/fx/bodies/<id>.glsl) by
// tools/fxgen.py and baked by qt_add_shaders. Adding an effect is therefore data plus GLSL, no C++:
//
//   filter:      vec4 effect(vec2 uv)      reads getColor(uv)
//   transition:  vec4 transition(vec2 uv)  reads getFromColor(uv) / getToColor(uv), progress
//
// both with `time` (seconds into the clip), `ratio` (width / height), `resolution` (pixels), `fxPass` / `fxPasses`
// (multi-pass filters such as a separable blur) and the effect's params as read-only names. uv follows the
// gl-transitions convention (origin bottom-left, y up), so their GLSL ports unchanged.
//
// Params live in one fixed std140 block (FxUniforms): param i of the schema is vec4 slot i, at most 16.
// float / int / enum / bool use .x, vec2 / point .xy, color .rgba. Values are premultiplied scene-linear
// colour (display-encoded when the project blends in display space); colour params arrive as straight RGBA
// converted to the same space. A `point` is stored top-left-origin, normalised (what a UI gives) and is
// handed to the shader y-up like uv.
//
// Where they sit in a render is described in compositor.h. Everything here is CPU-only and testable without a GPU.

#include "core/schema.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace sf::render {

enum class FxKind { Filter, Transition };
enum class FxParamType { Float, Vec2, Color, Bool, Int, Enum, Point, Texture };

constexpr int kFxMaxParams = 16;

struct FxParamDef {
  QString name;  // a GLSL identifier; the shader sees it as a macro of this name
  QString label; // for UIs (defaults to the name)
  FxParamType type = FxParamType::Float;
  double min = 0, max = 1;         // float / int / vec2 components (clamped on set)
  QJsonValue def;                  // default, in the param's JSON form
  QStringList options;             // enum / texture: names in order (the shader sees the index or procedural texture)
  QString description;
  bool animatable() const { return type == FxParamType::Float || type == FxParamType::Int || type == FxParamType::Vec2 || type == FxParamType::Color || type == FxParamType::Point; }
};

struct FxDef {
  QString id;
  QString name;
  QString category; // "stylize", "blur", "distort", "colour", "wipe", "dissolve", ...
  FxKind kind = FxKind::Filter;
  QString description;
  QStringList tags;
  QStringList whenToUse; // short intents an agent can match ("draw the eye to the subject")
  int passes = 1;        // filters only: the shader runs this many times, each reading the previous output (fxPass = 0 ..)
  std::vector<FxParamDef> params; // slot = index
  QString shader;                 // resource name of the baked shader: ":/sf/render/fx/shaders/<id>.frag.qsb"
  const FxParamDef* param(const QString& name) const;
  const FxParamDef* textureParam() const;
};

// The definitions compiled into the library (render/fx/registry/*.json), loaded on first use.
class FxRegistry {
public:
  static const FxRegistry& instance();
  const std::vector<FxDef>& all() const { return defs_; }
  const FxDef* find(const QString& id) const;
  const QString& error() const { return error_; } // non-empty when a registry file failed to parse

private:
  FxRegistry();
  std::vector<FxDef> defs_;
  QString error_;
};

// Parses one registry document ({"effects": [...]}); false with *error on the first bad entry.
bool parseFxDefs(const QJsonObject& root, std::vector<FxDef>* out, QString* error);
// The definition with its schema, as `effect.list` returns it.
QJsonObject fxDefJson(const FxDef& def);

// ---- params ----

// Checks `given` (name -> JSON) against the schema: unknown names and wrong types fail, numbers are clamped into
// range, enums accept a name or an index. *normalised is the cleaned object.
bool normalizeFxParams(const FxDef& def, const QJsonObject& given, QJsonObject* normalised, QString* error);
// Checks that a keyframed key ("name" or "name.x" / ".y" / ".r" ".g" ".b" ".a") can be animated for `def`.
bool fxKeyframeKeyOk(const FxDef& def, const QString& key, QString* error);
// Every param at an item-local frame: default, overridden by `given`, overridden by keyframes (interpolated).
QJsonObject resolveFxParams(const FxDef& def, const QJsonObject& given, const KeyframeMap& keyframes, Frame localFrame);

// Layout of the uniform block in fx.vert and every effect fragment shader (std140).
struct FxUniforms {
  float mvp[16];
  float timing[4]; // progress, time (s), ratio, pass index
  float res[4];    // width, height, pass count, 0
  float p[kFxMaxParams][4];
};
static_assert(sizeof(FxUniforms) == 352);

// ---- transitions on the timeline ----

struct ActiveTransition {
  const FxTransition* tr = nullptr;
  const Item* from = nullptr;
  const Item* to = nullptr;
  Frame start = 0, end = 0; // [start, end): the frames the transition runs
  double progress = 0;      // 0..1 at the asked frame
};
// The frames a transition covers (align "center" straddles the cut at the outgoing clip's end, "start" runs after it,
// "end" up to it). False when it is not valid now: a clip is gone, they are on different tracks, or the incoming one
// no longer starts where the outgoing one ends.
bool transitionWindow(const TimelineDoc& doc, const FxTransition& tr, const Item** from, const Item** to, Frame* start, Frame* end);
// The valid transitions whose window contains `frame`.
std::vector<ActiveTransition> activeTransitions(const TimelineDoc& doc, Frame frame);

// Writes the resolved params into the slots (`linearColor`: convert colour params from sRGB to linear light).
void packFxParams(const FxDef& def, const QJsonObject& resolved, bool linearColor, float (&outSlots)[kFxMaxParams][4]);

} // namespace sf::render
