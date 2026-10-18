// GPU effects (render/effects.h): the effect catalogue, per-item effect stacks and transitions between adjacent clips.
// Every mutating command is one undo step: a stack edit is an item.update patch of the item's `fx`, a transition edit a
// transitions.set of the whole list (core ops, so history, inverses and events behave as for any other edit).
// The inspector hooks in here: it lists effect.list, edits through effect.set / effect.keyframe, and reads item.fx.

#include "engine/cmd_util.h"
#include "engine/cmd_fx.h"
#include "engine/hand_quad.h"
#include "render/effects.h"

#include <algorithm>

namespace sf::engine {

namespace {

using render::FxDef;
using render::FxKind;
using render::FxRegistry;

const QJsonObject kEasing = js::choice({q("linear"), q("hold"), q("easeIn"), q("easeOut"), q("easeInOut")}, QStringLiteral("Easing into this keyframe (default linear)"));
const QStringList kAligns{q("center"), q("start"), q("end")};

QJsonArray fxJson(const std::vector<FxInstance>& fx) {
  QJsonArray a;
  for (const FxInstance& f : fx) a.append(toJson(f));
  return a;
}

Result fail(const QString& message, const QString& hint = {}) {
  return Result::failure(q("invalid_params"), hint.isEmpty() ? message : message + QStringLiteral(" ") + hint);
}

// The item must exist and draw something (not audio)
const Item* fxItem(CallContext& c, const QString& id, Result* err) {
  const Item* it = getItem(c.session->project->doc(), id);
  if (!it) {
    *err = notFound(QStringLiteral("Item %1 not found.").arg(id), QStringLiteral("Call timeline.get."));
    return nullptr;
  }
  if (it->type() == ItemType::Audio) {
    *err = fail(QStringLiteral("Item %1 is audio; effects apply to pictures.").arg(id));
    return nullptr;
  }
  const Track* t = getTrack(c.session->project->doc(), it->trackId);
  if (t && t->locked) {
    *err = Result::failure(q("edit_rejected"), QStringLiteral("Track \"%1\" is locked.").arg(t->name));
    return nullptr;
  }
  return it;
}

const FxDef* defOf(const QString& effectId, FxKind kind, Result* err) {
  const FxDef* def = FxRegistry::instance().find(effectId);
  if (!def) {
    *err = notFound(QStringLiteral("Effect \"%1\" not found.").arg(effectId), QStringLiteral("Call effect.list."));
    return nullptr;
  }
  if (def->kind != kind) {
    *err = fail(QStringLiteral("Effect \"%1\" is a %2: use %3.").arg(effectId, kind == FxKind::Filter ? q("transition") : q("filter"), kind == FxKind::Filter ? q("transition.add") : q("effect.add")));
    return nullptr;
  }
  return def;
}

// `it` points into the document, which apply() replaces: nothing may touch it afterwards
Result applyFx(CallContext& c, const Item& it, std::vector<FxInstance> fx, const QString& label, QJsonObject extra) {
  const QString itemId = it.id;
  ItemPatch patch;
  patch.fx = std::move(fx);
  Result r = applyToSession(c, {Op(ItemUpdate{itemId, patch})}, label, extra);
  if (!r.ok) return r;
  QJsonObject o = r.value.toObject();
  if (const Item* now = getItem(c.session->project->doc(), itemId)) o.insert(q("fx"), fxJson(now->fx));
  return Result::success(o);
}

FxInstance* findFx(std::vector<FxInstance>& fx, const QString& id) {
  for (FxInstance& f : fx)
    if (f.id == id) return &f;
  return nullptr;
}

Result noFx(const Item& it, const QString& fxId) {
  QStringList ids;
  for (const FxInstance& f : it.fx) ids << f.id;
  return notFound(QStringLiteral("Effect instance %1 not found on item %2 (has: %3).").arg(fxId, it.id, ids.isEmpty() ? q("none") : ids.join(QStringLiteral(", "))));
}

// ---- transitions ----

Result applyTransitions(CallContext& c, std::vector<FxTransition> list, const QString& label, QJsonObject extra) {
  Result r = applyToSession(c, {Op(TransitionsSet{std::move(list)})}, label, extra);
  if (!r.ok) return r;
  QJsonObject o = r.value.toObject();
  QJsonArray arr;
  for (const FxTransition& t : c.session->project->doc().transitions) arr.append(toJson(t));
  o.insert(q("transitions"), arr);
  return Result::success(o);
}

// Why `duration` doesn't fit this pair, or empty
QString durationProblem(const Item& a, const Item& b, Frame duration, const QString& align) {
  const Frame limit = align == QLatin1String("center") ? 2 * std::min(a.durationFrames, b.durationFrames) : align == QLatin1String("start") ? b.durationFrames : a.durationFrames;
  if (duration <= limit) return {};
  return QStringLiteral("Duration %1 is longer than the clips allow for align \"%2\" (max %3 frames: it may not run past the clip it sits in).").arg(duration).arg(align).arg(limit);
}

// Why this transition clashes with the others (same pair, or windows overlapping on a shared clip), or empty
QString clash(const TimelineDoc& doc, const std::vector<FxTransition>& list, const FxTransition& cand, const QString& ignoreId) {
  Frame cs = 0, ce = 0;
  if (!render::transitionWindow(doc, cand, nullptr, nullptr, &cs, &ce)) return {};
  for (const FxTransition& t : list) {
    if (t.id == ignoreId) continue;
    if (t.fromItemId == cand.fromItemId && t.toItemId == cand.toItemId) return QStringLiteral("These clips already have transition %1: use transition.set.").arg(t.id);
    const bool shares = t.fromItemId == cand.fromItemId || t.fromItemId == cand.toItemId || t.toItemId == cand.fromItemId || t.toItemId == cand.toItemId;
    Frame s = 0, e = 0;
    if (shares && render::transitionWindow(doc, t, nullptr, nullptr, &s, &e) && s < ce && cs < e)
      return QStringLiteral("Transition %1 overlaps this one on a shared clip: shorten one or use another cut.").arg(t.id);
  }
  return {};
}

// The clip that starts where `a` ends on the same track, if any
const Item* nextAdjacent(const TimelineDoc& doc, const Item& a) {
  for (const Item* it : itemsOnTrack(doc, a.trackId))
    if (it != &a && it->startFrame == itemEnd(a)) return it;
  return nullptr;
}

const FxTransition* findTransition(const TimelineDoc& doc, const QString& id) {
  for (const FxTransition& t : doc.transitions)
    if (t.id == id) return &t;
  return nullptr;
}

QJsonObject transitionJson(const TimelineDoc& doc, const FxTransition& t) {
  QJsonObject o = toJson(t);
  Frame s = 0, e = 0;
  const bool valid = render::transitionWindow(doc, t, nullptr, nullptr, &s, &e);
  o.insert(q("valid"), valid);
  if (valid) {
    o.insert(q("startFrame"), static_cast<double>(s));
    o.insert(q("endFrame"), static_cast<double>(e));
  }
  return o;
}

} // namespace


Result buildEffectAddOps(CallContext& c, const QString& itemId, const QString& effectId, const QJsonObject& params, QJsonArray* ops, int index) {
  Result err;
  const Item* it = fxItem(c, itemId, &err);
  if (!it) return err;
  const FxDef* def = defOf(effectId, FxKind::Filter, &err);
  if (!def) return err;
  FxInstance f;
  f.id = newId(QStringLiteral("fx"));
  f.effectId = def->id;
  QString why;
  if (!render::normalizeFxParams(*def, params, &f.params, &why)) return fail(why, QStringLiteral("See effect.list."));
  std::vector<FxInstance> stack = it->fx;
  const qsizetype at = (index >= 0) ? std::min<qsizetype>(index, static_cast<qsizetype>(stack.size())) : static_cast<qsizetype>(stack.size());
  stack.insert(stack.begin() + at, f);

  ItemPatch patch;
  patch.fx = std::move(stack);
  if (ops) {
    ops->append(QJsonObject{
      {q("type"), q("item.update")},
      {q("itemId"), itemId},
      {q("patch"), sf::toJson(patch)}
    });
  }
  return Result::success();
}

// mask.trackHands: drive a polygon_mask effect from the deterministic B9 hands analysis.
// Session, mutating, one undo step: a single item.update patch of the item's `fx`.
// Returns {pending: true, jobId} while the hands analysis is not done yet.
Result maskTrackHands(CallContext& c, const QJsonObject& a) {
  Result err;
  const QString itemId = a.value(q("itemId")).toString();
  if (itemId.isEmpty()) return invalid(q("Give itemId."));
  const Item* it = fxItem(c, itemId, &err);
  if (!it) return err;

  const double fps = a.contains(q("fps")) ? a.value(q("fps")).toDouble(15.0) : 15.0;
  const double minScore = a.contains(q("minScore")) ? a.value(q("minScore")).toDouble(0.6) : 0.6;
  const bool invert = a.value(q("invert")).toBool(false);
  const bool wait = a.value(q("wait")).toBool(true);
  double minCutoff = 1.0, beta = 0.05;
  if (a.contains(q("smoothing"))) {
    const QJsonObject sm = a.value(q("smoothing")).toObject();
    minCutoff = sm.value(q("minCutoff")).toDouble(1.0);
    beta = sm.value(q("beta")).toDouble(0.05);
  }
  const QString fxIdArg = a.contains(q("fxId")) ? a.value(q("fxId")).toString() : QString();

  // 1. Get the hands result. analysis.get blocks (up to timeoutMs) when wait=true, so a CLI call
  //    can finish; enqueue=true ensures an unqueued job is scheduled.
  QJsonObject getArgs{
    {q("analyzer"), q("hands")},
    {q("itemId"), itemId},
    {q("params"), QJsonObject{{q("fps"), fps}}},
    {q("wait"), wait},
    {q("enqueue"), true},
    {q("session"), c.session->id}
  };
  if (a.contains(q("timeoutMs"))) getArgs.insert(q("timeoutMs"), a.value(q("timeoutMs")));
  const Result ar = c.engine->call(q("analysis.get"), getArgs);
  if (!ar.ok) return ar;
  const QJsonObject g = ar.value.toObject();
  if (g.value(q("status")).toString() != q("done"))
    return Result::success(QJsonObject{{q("pending"), true}, {q("jobId"), g.value(q("jobId"))}});
  const QJsonArray frames = g.value(q("result")).toObject().value(q("frames")).toArray();

  // 2-3. Build the per-sample quads (left thumb/index, right index/thumb, angle-sorted) and smooth.
  std::vector<QuadSample> samples = buildHandQuads(frames, minScore);
  smoothQuads(samples, minCutoff, beta);

  // 4. Fill gaps: short gaps hold the last good quad; long gaps are flagged for opacity keyframes.
  const std::vector<Gap> gaps = classifyGaps(samples, 0.3);

  int trackedSamples = 0;
  for (const QuadSample& s : samples)
    if (!s.gap) ++trackedSamples;

  // Convert sample seconds to item-local frames at the project fps.
  const TimelineDoc& doc = c.session->project->doc();
  const Frame srcIn = it->sourceInFrame.value_or(0);
  const auto localFrame = [&](double t) { return std::max(Frame(0), toFrames(t, doc) - srcIn); };

  // 5. Keyframes for p0..p3 at the retained (thinned) sample frames, linear easing; count = 4 static.
  std::vector<FxInstance> stack = it->fx;
  FxInstance* existing = nullptr;
  for (FxInstance& f : stack)
    if (f.effectId == q("polygon_mask") && (fxIdArg.isEmpty() || f.id == fxIdArg)) { existing = &f; break; }
  QString fxId;
  if (!existing) {
    if (!fxIdArg.isEmpty()) return notFound(q("Effect instance %1 not found on item %2 (has no polygon_mask).").arg(fxIdArg, itemId));
    FxInstance f;
    f.id = newId(q("fx"));
    f.effectId = q("polygon_mask");
    f.params[q("count")] = 4;
    f.params[q("invert")] = invert;
    stack.push_back(std::move(f));
    existing = &stack.back();
    fxId = existing->id;
  } else {
    existing->params[q("count")] = 4;
    existing->params[q("invert")] = invert;
    existing->enabled = true;
    fxId = existing->id;
  }
  for (const char* k : {"p0.x", "p0.y", "p1.x", "p1.y", "p2.x", "p2.y", "p3.x", "p3.y", "opacity"}) {
    existing->keyframes.erase(q(k));
  }

  const std::vector<int> kept = keyframeIndices(samples);
  const int quadKeyframes = static_cast<int>(kept.size());
  QJsonObject keyframeCounts;
  keyframeCounts.insert(q("p0"), quadKeyframes);
  keyframeCounts.insert(q("p1"), quadKeyframes);
  keyframeCounts.insert(q("p2"), quadKeyframes);
  keyframeCounts.insert(q("p3"), quadKeyframes);
  for (int i : kept) {
    const QuadSample& s = samples[i];
    const Frame fr = localFrame(s.t);
    Keyframe kf;
    kf.frame = fr;
    kf.easing = Easing::Linear;
    const QPointF* p = s.quad.data();
    kf.value = p[0].x(); existing->keyframes[q("p0.x")].push_back(kf);
    kf.value = p[0].y(); existing->keyframes[q("p0.y")].push_back(kf);
    kf.value = p[1].x(); existing->keyframes[q("p1.x")].push_back(kf);
    kf.value = p[1].y(); existing->keyframes[q("p1.y")].push_back(kf);
    kf.value = p[2].x(); existing->keyframes[q("p2.x")].push_back(kf);
    kf.value = p[2].y(); existing->keyframes[q("p2.y")].push_back(kf);
    kf.value = p[3].x(); existing->keyframes[q("p3.x")].push_back(kf);
    kf.value = p[3].y(); existing->keyframes[q("p3.y")].push_back(kf);
  }

  // Opacity keyframes: 1 on last tracked frame before long gap, 0 on next frame,
  // hold 0 through gap, 1 on frame hands return. 0 at start if clip starts in gap.
  const auto opKfs = buildOpacityKeyframes(samples, 1.0 / fps);
  auto& okf = existing->keyframes[q("opacity")];
  for (const auto& okfItem : opKfs) {
    const Frame fr = localFrame(okfItem.t);
    if (!okf.empty() && okf.back().frame == fr) {
      okf.back().value = okfItem.opacity;
    } else {
      Keyframe kf;
      kf.frame = fr;
      kf.value = okfItem.opacity;
      kf.easing = Easing::Linear;
      okf.push_back(kf);
    }
  }
  std::sort(okf.begin(), okf.end(), [](const Keyframe& a, const Keyframe& b) { return a.frame < b.frame; });
  keyframeCounts.insert(q("opacity"), static_cast<int>(okf.size()));

  QJsonArray gapArr;
  for (const Gap& gap : gaps)
    gapArr.append(QJsonObject{{q("startSec"), gap.startSec}, {q("endSec"), gap.endSec}});

  // 6. ONE item.update patch = one undo step.
  ItemPatch patch;
  patch.fx = std::move(stack);
  QJsonArray ops;
  ops.append(QJsonObject{
    {q("type"), q("item.update")},
    {q("itemId"), itemId},
    {q("patch"), sf::toJson(patch)}
  });

  QJsonObject extra{
    {q("itemId"), itemId},
    {q("fxId"), fxId},
    {q("samples"), static_cast<double>(samples.size())},
    {q("trackedSamples"), static_cast<double>(trackedSamples)},
    {q("gaps"), gapArr},
    {q("keyframes"), keyframeCounts},
    {q("notes"), q("Landmarks from the deterministic B9 hands analysis; no model runs at edit time.")}
  };
  Result w = applyToSession(c, parseOps(ops), q("mask.trackHands"), extra);
  if (!w.ok) return w;
  QJsonObject o = w.value.toObject();
  if (const Item* now = getItem(c.session->project->doc(), itemId)) o.insert(q("fx"), fxJson(now->fx));
  return Result::success(o);
}

void registerFxCommands(Engine& e) {
  e.add(spec("effect.list", "", "editor", false,
             QStringLiteral("The GPU effect catalogue: every definition with id, name, category, kind (filter = one picture in, applied with effect.add; transition = two pictures plus progress, applied "
                            "with transition.add), description, tags, whenToUse intents and the param schema (name, type float|int|enum|bool|vec2|point|color|texture, min, max, default, options, animatable). "
                            "Params are JSON: number, bool, enum name, texture name, [x, y] (vec2; point is [x, y] in 0..1 from the top-left), \"#rrggbb\" or [r, g, b, a] (color)."),
             js::obj({{q("category"), js::str(QStringLiteral("Only this category (stylize, blur, distort, dissolve, wipe, ...)"))}, {q("kind"), js::choice({q("filter"), q("transition")})}}),
             [](CallContext&, const QJsonObject& a) {
               const QString category = a.value(q("category")).toString();
               const QString kind = a.value(q("kind")).toString();
               QJsonArray list;
               QStringList categories;
               for (const FxDef& d : FxRegistry::instance().all()) {
                 if (!category.isEmpty() && d.category != category) continue;
                 if (!kind.isEmpty() && (d.kind == FxKind::Filter) != (kind == QLatin1String("filter"))) continue;
                 list.append(render::fxDefJson(d));
                 if (!categories.contains(d.category)) categories << d.category;
               }
               return Result::success(QJsonObject{{q("effects"), list}, {q("categories"), QJsonArray::fromStringList(categories)}});
             },
             false));

  e.add(spec("effect.add", "", "editor", true,
             QStringLiteral("Add a filter effect to an item's stack (applied in order, after the colour stage, before the item's transform and opacity). `params` sets only what differs from the "
                            "defaults; `index` places it in the stack (default: last). Returns the new `fxId` and the stack."),
             js::obj({{q("itemId"), js::str()}, {q("effectId"), js::str(QStringLiteral("From effect.list (kind filter)"))}, {q("params"), js::any()}, {q("index"), js::integer(QString(), 0)}},
                     {q("itemId"), q("effectId")}),
             [](CallContext& c, const QJsonObject& a) {
               const QString itemId = a.value(q("itemId")).toString();
               const int idx = a.contains(q("index")) ? a.value(q("index")).toInt() : -1;
               QJsonArray ops;
               Result r = buildEffectAddOps(c, itemId, a.value(q("effectId")).toString(), a.value(q("params")).toObject(), &ops, idx);
               if (!r.ok) return r;
               const QJsonArray stackArr = ops.at(0).toObject().value(q("patch")).toObject().value(q("fx")).toArray();
               const QString fxId = (idx >= 0 && idx < stackArr.size()) ? stackArr.at(idx).toObject().value(q("id")).toString() : stackArr.last().toObject().value(q("id")).toString();
               Result w = applyToSession(c, parseOps(ops), QStringLiteral("effect.add"), {{q("fxId"), fxId}});
               if (!w.ok) return w;
               QJsonObject o = w.value.toObject();
               if (const Item* now = getItem(c.session->project->doc(), itemId)) o.insert(q("fx"), fxJson(now->fx));
               return Result::success(o);
             }));

  e.add(spec("effect.set", "", "editor", true,
             QStringLiteral("Change an effect on an item: `params` merges over the current ones (a null value resets that param to its default), `enabled` bypasses or re-enables it. Numbers are clamped to the "
                            "param's range."),
             js::obj({{q("itemId"), js::str()}, {q("fxId"), js::str()}, {q("params"), js::any()}, {q("enabled"), js::boolean()}}, {q("itemId"), q("fxId")}),
             [](CallContext& c, const QJsonObject& a) {
               Result err;
               const Item* it = fxItem(c, a.value(q("itemId")).toString(), &err);
               if (!it) return err;
               std::vector<FxInstance> stack = it->fx;
               FxInstance* f = findFx(stack, a.value(q("fxId")).toString());
               if (!f) return noFx(*it, a.value(q("fxId")).toString());
               if (!a.contains(q("params")) && !a.contains(q("enabled"))) return fail(QStringLiteral("Give params and/or enabled."));
               if (a.contains(q("params"))) {
                 const FxDef* def = FxRegistry::instance().find(f->effectId);
                 if (!def) return notFound(QStringLiteral("Effect \"%1\" is not in this build.").arg(f->effectId));
                 const QJsonObject given = a.value(q("params")).toObject();
                 QJsonObject set;
                 for (auto i = given.begin(); i != given.end(); ++i)
                   if (!i.value().isNull()) set.insert(i.key(), i.value());
                 QJsonObject clean;
                 QString why;
                 if (!render::normalizeFxParams(*def, set, &clean, &why)) return fail(why, QStringLiteral("See effect.list."));
                 for (auto i = given.begin(); i != given.end(); ++i) {
                   if (i.value().isNull()) {
                     if (!def->param(i.key())) return fail(QStringLiteral("Effect \"%1\" has no param \"%2\".").arg(def->id, i.key()));
                     f->params.remove(i.key());
                   } else {
                     f->params.insert(i.key(), clean.value(i.key()));
                   }
                 }
               }
               if (a.contains(q("enabled"))) f->enabled = a.value(q("enabled")).toBool();
               return applyFx(c, *it, std::move(stack), QStringLiteral("effect.set"), {{q("fxId"), a.value(q("fxId"))}});
             }));

  e.add(spec("effect.keyframe", "", "editor", true,
             QStringLiteral("Animate a param of an effect: set (or replace) the keyframe at item-local frame `t` to value `v`, eased into from the previous keyframe with `ease`. Float and int params animate "
                            "directly (param \"radius\"); vec2, point and color params animate one component at a time (\"center.x\", \"tint.r\"). With `clear: true` the param's keyframes are removed "
                            "(its static value applies again)."),
             js::obj({{q("itemId"), js::str()}, {q("fxId"), js::str()}, {q("param"), js::str()}, {q("t"), js::integer(QStringLiteral("Item-local frame (0 = the item's first frame)"), 0)},
                      {q("v"), js::number(QStringLiteral("Value at that frame"))}, {q("ease"), kEasing}, {q("clear"), js::boolean()}},
                     {q("itemId"), q("fxId"), q("param")}),
             [](CallContext& c, const QJsonObject& a) {
               Result err;
               const Item* it = fxItem(c, a.value(q("itemId")).toString(), &err);
               if (!it) return err;
               std::vector<FxInstance> stack = it->fx;
               FxInstance* f = findFx(stack, a.value(q("fxId")).toString());
               if (!f) return noFx(*it, a.value(q("fxId")).toString());
               const FxDef* def = FxRegistry::instance().find(f->effectId);
               if (!def) return notFound(QStringLiteral("Effect \"%1\" is not in this build.").arg(f->effectId));
               const QString param = a.value(q("param")).toString();
               QString why;
               if (!render::fxKeyframeKeyOk(*def, param, &why)) return fail(why);
               if (a.value(q("clear")).toBool()) {
                 f->keyframes.erase(param);
               } else {
                 if (!a.contains(q("t")) || !a.contains(q("v"))) return fail(QStringLiteral("Give t and v (or clear: true)."));
                 const Keyframe k = parseKeyframe(Rd(QJsonObject{{q("frame"), a.value(q("t"))}, {q("value"), a.value(q("v"))}, {q("easing"), a.value(q("ease")).toString(q("linear"))}}));
                 KeyframeList& list = f->keyframes[param];
                 const auto same = std::find_if(list.begin(), list.end(), [&](const Keyframe& x) { return x.frame == k.frame; });
                 if (same != list.end()) *same = k;
                 else list.push_back(k);
                 std::stable_sort(list.begin(), list.end(), [](const Keyframe& x, const Keyframe& y) { return x.frame < y.frame; });
               }
               const QString fxId = f->id;
               return applyFx(c, *it, std::move(stack), QStringLiteral("effect.keyframe"), {{q("fxId"), fxId}});
             }));

  e.add(spec("effect.remove", "", "editor", true, QStringLiteral("Remove an effect from an item's stack."),
             js::obj({{q("itemId"), js::str()}, {q("fxId"), js::str()}}, {q("itemId"), q("fxId")}),
             [](CallContext& c, const QJsonObject& a) {
               Result err;
               const Item* it = fxItem(c, a.value(q("itemId")).toString(), &err);
               if (!it) return err;
               std::vector<FxInstance> stack = it->fx;
               const auto at = std::find_if(stack.begin(), stack.end(), [&](const FxInstance& f) { return f.id == a.value(q("fxId")).toString(); });
               if (at == stack.end()) return noFx(*it, a.value(q("fxId")).toString());
               stack.erase(at);
               return applyFx(c, *it, std::move(stack), QStringLiteral("effect.remove"), {});
             }));

  e.add(spec("effect.reorder", "", "editor", true,
             QStringLiteral("Move an effect to another position in the item's stack (0 = applied first). Order matters: blur then vignette is not vignette then blur."),
             js::obj({{q("itemId"), js::str()}, {q("fxId"), js::str()}, {q("index"), js::integer(QStringLiteral("New position, 0 = first (clamped to the end)"), 0)}}, {q("itemId"), q("fxId"), q("index")}),
             [](CallContext& c, const QJsonObject& a) {
               Result err;
               const Item* it = fxItem(c, a.value(q("itemId")).toString(), &err);
               if (!it) return err;
               std::vector<FxInstance> stack = it->fx;
               const auto from = std::find_if(stack.begin(), stack.end(), [&](const FxInstance& f) { return f.id == a.value(q("fxId")).toString(); });
               if (from == stack.end()) return noFx(*it, a.value(q("fxId")).toString());
               const FxInstance moved = *from;
               stack.erase(from);
               stack.insert(stack.begin() + std::min<qsizetype>(a.value(q("index")).toInt(), static_cast<qsizetype>(stack.size())), moved);
               return applyFx(c, *it, std::move(stack), QStringLiteral("effect.reorder"), {});
             }));

  e.add(spec("mask.trackHands", "", "engine", true,
             QStringLiteral("Track hands with a polygon mask: drives a `polygon_mask` filter on `itemId` (added if absent) from the deterministic B9 `hands` analysis. "
                            "The thumb tip (4) and index fingertip (8) of both hands form a quad, so the clip shows through the frame your hands make. "
                            "Args: `itemId` (video item), `fxId?` (existing polygon_mask, else one is added), `fps?` (analysis sample rate, default 15), "
                            "`minScore?` (default 0.6), `smoothing?` {minCutoff: 1.0, beta: 0.05} for the One-Euro filter, `invert?` (default false), `wait?` (default true). "
                            "Returns {itemId, fxId, samples, trackedSamples, gaps:[{startSec,endSec}], keyframes:<count per param>, notes}, or {pending:true, jobId} while the analysis runs. "
                            "All numbers come from deterministic code, not a model. One undo step."),
             js::obj({{q("itemId"), js::str()}, {q("fxId"), js::str()}, {q("fps"), js::number()},
                      {q("minScore"), js::number()}, {q("smoothing"), js::any()}, {q("invert"), js::boolean()}, {q("wait"), js::boolean()}, {q("timeoutMs"), js::integer()}},
                     {q("itemId")}),
             maskTrackHands));

  // ---- transitions ----
  e.add(spec("transition.list", "", "editor", false,
             QStringLiteral("The project's transitions with the frames they run over and whether they are still valid (the clips still butt together)."), js::obj({}),
             [](CallContext& c, const QJsonObject&) {
               const TimelineDoc& d = c.session->project->doc();
               QJsonArray arr;
               for (const FxTransition& t : d.transitions) arr.append(transitionJson(d, t));
               return Result::success(QJsonObject{{q("transitions"), arr}});
             }));

  e.add(spec("transition.add", "", "editor", true,
             QStringLiteral("Add a transition (effect of kind transition, see effect.list) at a cut between two clips that butt together on one track. Give `itemId` (the left clip: the cut at its end) or `trackId` "
                            "(every cut on that track that has none yet). `duration` is in frames; `align` says where it sits: center straddles the cut (default; uses source beyond the clip edges where the "
                            "media has it, else holds the edge frame), start runs after the cut, end runs up to it. A clip can't be longer than the window it feeds allows."),
             js::obj({{q("itemId"), js::str(QStringLiteral("Left clip of the cut"))}, {q("trackId"), js::str(QStringLiteral("All cuts on this track"))}, {q("effectId"), js::str()},
                      {q("duration"), js::integer(QStringLiteral("Frames"), 1)}, {q("align"), js::choice(kAligns)}, {q("params"), js::any()}},
                     {q("effectId"), q("duration")}),
             [](CallContext& c, const QJsonObject& a) {
               const TimelineDoc& d = c.session->project->doc();
               if (a.contains(q("itemId")) == a.contains(q("trackId"))) return fail(QStringLiteral("Give exactly one of itemId (left clip) or trackId."));
               Result err;
               const FxDef* def = defOf(a.value(q("effectId")).toString(), FxKind::Transition, &err);
               if (!def) return err;
               FxTransition proto;
               proto.effectId = def->id;
               proto.duration = a.value(q("duration")).toInteger();
               proto.align = a.value(q("align")).toString(q("center"));
               QString why;
               if (!render::normalizeFxParams(*def, a.value(q("params")).toObject(), &proto.params, &why)) return fail(why, QStringLiteral("See effect.list."));

               std::vector<const Item*> lefts;
               if (a.contains(q("itemId"))) {
                 const Item* l = getItem(d, a.value(q("itemId")).toString());
                 if (!l) return notFound(QStringLiteral("Item %1 not found.").arg(a.value(q("itemId")).toString()), QStringLiteral("Call timeline.get."));
                 lefts.push_back(l);
               } else {
                 const Track* t = getTrack(d, a.value(q("trackId")).toString());
                 if (!t) return notFound(QStringLiteral("Track %1 not found.").arg(a.value(q("trackId")).toString()), QStringLiteral("Call timeline.get."));
                 lefts = itemsOnTrack(d, t->id);
               }
               std::vector<FxTransition> list = d.transitions;
               QJsonArray created;
               for (const Item* l : lefts) {
                 const Track* t = getTrack(d, l->trackId);
                 if (t && t->locked) return Result::failure(q("edit_rejected"), QStringLiteral("Track \"%1\" is locked.").arg(t->name));
                 const Item* r = nextAdjacent(d, *l);
                 if (!r) {
                   if (a.contains(q("itemId"))) return fail(QStringLiteral("No clip starts at frame %1 on the same track, where %2 ends: transitions need two clips that butt together.").arg(itemEnd(*l)).arg(l->id));
                   continue;
                 }
                 if (!a.contains(q("itemId")) && std::any_of(list.begin(), list.end(), [&](const FxTransition& x) { return x.fromItemId == l->id && x.toItemId == r->id; })) continue;
                 FxTransition tr = proto;
                 tr.id = newId(QStringLiteral("trn"));
                 tr.fromItemId = l->id;
                 tr.toItemId = r->id;
                 if (const QString p = durationProblem(*l, *r, tr.duration, tr.align); !p.isEmpty()) {
                   if (a.contains(q("itemId"))) return fail(p);
                   continue;
                 }
                 if (const QString p = clash(d, list, tr, {}); !p.isEmpty()) {
                   if (a.contains(q("itemId"))) return fail(p);
                   continue;
                 }
                 created.append(tr.id);
                 list.push_back(std::move(tr));
               }
               if (created.isEmpty()) return fail(QStringLiteral("No cut to add a transition to (clips must butt together, fit the duration and not already have one)."));
               return applyTransitions(c, std::move(list), QStringLiteral("transition.add"), {{q("transitionIds"), created}});
             }));

  e.add(spec("transition.set", "", "editor", true,
             QStringLiteral("Change a transition: another `effectId` (params then start from the new effect's defaults unless given), `duration` (frames), `align`, and/or `params` (merged; null resets one)."),
             js::obj({{q("transitionId"), js::str()}, {q("effectId"), js::str()}, {q("duration"), js::integer(QStringLiteral("Frames"), 1)}, {q("align"), js::choice(kAligns)}, {q("params"), js::any()}},
                     {q("transitionId")}),
             [](CallContext& c, const QJsonObject& a) {
               const TimelineDoc& d = c.session->project->doc();
               std::vector<FxTransition> list = d.transitions;
               const auto it = std::find_if(list.begin(), list.end(), [&](const FxTransition& t) { return t.id == a.value(q("transitionId")).toString(); });
               if (it == list.end()) return notFound(QStringLiteral("Transition %1 not found.").arg(a.value(q("transitionId")).toString()), QStringLiteral("Call transition.list."));
               FxTransition& tr = *it;
               Result err;
               if (a.contains(q("effectId"))) {
                 if (!defOf(a.value(q("effectId")).toString(), FxKind::Transition, &err)) return err;
                 tr.effectId = a.value(q("effectId")).toString();
                 tr.params = {};
               }
               const FxDef* def = FxRegistry::instance().find(tr.effectId);
               if (!def) return notFound(QStringLiteral("Effect \"%1\" is not in this build.").arg(tr.effectId));
               if (a.contains(q("params"))) {
                 const QJsonObject given = a.value(q("params")).toObject();
                 QJsonObject set;
                 for (auto i = given.begin(); i != given.end(); ++i)
                   if (!i.value().isNull()) set.insert(i.key(), i.value());
                 QJsonObject clean;
                 QString why;
                 if (!render::normalizeFxParams(*def, set, &clean, &why)) return fail(why, QStringLiteral("See effect.list."));
                 for (auto i = given.begin(); i != given.end(); ++i) {
                   if (i.value().isNull()) tr.params.remove(i.key());
                   else tr.params.insert(i.key(), clean.value(i.key()));
                 }
               }
               if (a.contains(q("duration"))) tr.duration = a.value(q("duration")).toInteger();
               if (a.contains(q("align"))) tr.align = a.value(q("align")).toString();
               const Item* l = getItem(d, tr.fromItemId);
               const Item* r = getItem(d, tr.toItemId);
               if (l && r) {
                 if (const QString p = durationProblem(*l, *r, tr.duration, tr.align); !p.isEmpty()) return fail(p);
                 if (const QString p = clash(d, list, tr, tr.id); !p.isEmpty()) return fail(p);
               }
               const QString id = tr.id;
               return applyTransitions(c, std::move(list), QStringLiteral("transition.set"), {{q("transitionId"), id}});
             }));

  e.add(spec("transition.remove", "", "editor", true, QStringLiteral("Remove a transition (the cut becomes hard again)."), js::obj({{q("transitionId"), js::str()}}, {q("transitionId")}),
             [](CallContext& c, const QJsonObject& a) {
               std::vector<FxTransition> list = c.session->project->doc().transitions;
               const auto it = std::find_if(list.begin(), list.end(), [&](const FxTransition& t) { return t.id == a.value(q("transitionId")).toString(); });
               if (it == list.end()) return notFound(QStringLiteral("Transition %1 not found.").arg(a.value(q("transitionId")).toString()), QStringLiteral("Call transition.list."));
               list.erase(it);
               return applyTransitions(c, std::move(list), QStringLiteral("transition.remove"), {});
             }));
}

} // namespace sf::engine
