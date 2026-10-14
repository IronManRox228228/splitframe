// The style library as commands: style.list, style.spec.schema, look.apply, pacing.check, mix.applyProfile.
// Taste is data (engine/style_library, JSON resources compiled in); these commands measure, propose from that data and verify with the
// measured layers (colour.analyze / colour.check, timeline.stats, audio.normalize / audio.duck / audio.check). A model picks names and
// small bounded offsets; every number is computed here.

#include "engine/cmd_util.h"
#include "engine/style_library.h"
#include "engine/timeline_analysis.h"

#include <algorithm>
#include <cmath>

namespace sf::engine {

namespace {

using namespace style;

QJsonValue fin(double v, double scale = 1000.0) { return std::isfinite(v) ? QJsonValue(std::round(v * scale) / scale) : QJsonValue(QJsonValue::Null); }

// ---------------------------------------------------------------- style.list / style.spec.schema

Result listCmd(CallContext&, const QJsonObject& a) {
  const QString kind = a.value(q("kind")).toString();
  const QString name = a.value(q("name")).toString();
  const auto one = [&](const QString& k) {
    QJsonObject o = listJson(k);
    if (!name.isEmpty()) {
      QJsonArray keep;
      for (const QJsonValue& v : o.value(q("items")).toArray())
        if (v.toObject().value(q("name")).toString() == name) keep.append(v);
      o.insert(q("items"), keep);
    }
    return o;
  };
  if (!kind.isEmpty()) {
    const QJsonObject o = one(kind);
    if (!name.isEmpty() && o.value(q("items")).toArray().isEmpty()) return notFound(QStringLiteral("No %1 named \"%2\".").arg(kind, name), QStringLiteral("Names: %1.").arg(o.value(q("names")).toVariant().toStringList().join(QStringLiteral(", "))));
    return Result::success(o);
  }
  QJsonObject all;
  for (const char* k : {"look", "pacing", "mix"}) all.insert(q(k), one(q(k)));
  return Result::success(QJsonObject{{q("looks"), all.value(q("look"))}, {q("pacing"), all.value(q("pacing"))}, {q("mix"), all.value(q("mix"))}});
}

Result specSchemaCmd(CallContext&, const QJsonObject&) {
  return Result::success(QJsonObject{{q("schema"), specSchema()},
                                     {q("example"), QJsonObject{{q("look"), q("warm-documentary")}, {q("intensity"), 0.7}, {q("warmth"), 0.05}, {q("contrast"), 0.0}, {q("saturation"), -0.05},
                                                                 {q("pacing"), q("documentary")}, {q("mix"), q("streaming")}}},
                                     {q("use"), q("look + intensity + warmth / contrast / saturation -> look.apply {look, intensity, warmth, contrast, saturation, itemIds}; pacing -> pacing.check {profile}; mix -> mix.applyProfile {profile}.")}});
}

// ---------------------------------------------------------------- look.apply

QJsonObject summary(const QJsonObject& m) {
  const QJsonObject sat = m.value(q("saturation")).toObject(), skin = m.value(q("skin")).toObject(), cast = m.value(q("cast")).toObject();
  return QJsonObject{{q("exposureStops"), fin(m.value(q("luma")).toObject().value(q("stopsVs18Grey")).toDouble())},
                     {q("contrastStops"), fin(m.value(q("contrast")).toObject().value(q("stops")).toDouble())},
                     {q("crushedPct"), fin(m.value(q("crushedPct")).toObject().value(q("all")).toDouble())},
                     {q("clippedPct"), fin(m.value(q("clippedPct")).toObject().value(q("all")).toDouble())},
                     {q("castTemperature"), fin(cast.value(q("temperature")).toDouble())},
                     {q("castTint"), fin(cast.value(q("tint")).toDouble())},
                     {q("chromaMean"), fin(sat.value(q("chromaMean")).toDouble())},
                     {q("chromaP95"), fin(sat.value(q("chromaP95")).toDouble())},
                     {q("gamutLimitPct"), fin(sat.value(q("nearGamutLimitPct")).toDouble())},
                     {q("skinPct"), fin(skin.value(q("pct")).toDouble())},
                     {q("skinHueDeg"), fin(skin.value(q("hueDeg")).toDouble())}};
}

QJsonObject viol(const QString& code, const QString& message, double value, double limit) {
  return QJsonObject{{q("code"), code}, {q("message"), message}, {q("value"), fin(value)}, {q("limit"), fin(limit)}};
}

// Every guard of the look against `after` (measured or predicted) compared with `before`: colour.check for the ones it measures, the rest here.
Result guardViolations(CallContext& c, const QString& itemId, const QJsonObject& before, const QJsonObject& after, const LookGuards& g, const PrimaryGrade& grade, const PrimaryGrade& current,
                       QJsonArray* out) {
  const QJsonObject thresholds{{q("blacksCrushedIncrease"), g.blacksCrushedIncrease}, {q("highlightsClippedIncrease"), g.highlightsClippedIncrease}, {q("skinHueShiftDeg"), g.skinHueShiftDeg},
                               {q("skinMinPct"), g.skinMinPct}, {q("exposureShiftStops"), g.exposureShiftStops + std::fabs(grade.exposure - current.exposure)}};
  const Result r = c.engine->call(q("colour.check"), {{q("itemId"), itemId}, {q("before"), before}, {q("after"), after}, {q("thresholds"), thresholds}, {q("session"), c.session->id}});
  if (!r.ok) return r;
  *out = r.value.toObject().value(q("violations")).toArray();
  const QJsonObject sat = after.value(q("saturation")).toObject(), skin = after.value(q("skin")).toObject();
  const double p95 = sat.value(q("chromaP95")).toDouble(), gamut = sat.value(q("nearGamutLimitPct")).toDouble();
  if (p95 > g.maxChromaP95) out->append(viol(q("maxChromaP95"), QStringLiteral("Saturation (display chroma, 95th percentile) reaches %1, over the look's %2.").arg(p95, 0, 'f', 3).arg(g.maxChromaP95, 0, 'f', 3), p95, g.maxChromaP95));
  if (gamut > g.maxGamutLimitPct) out->append(viol(q("maxGamutLimitPct"), QStringLiteral("%1% of the pixels sit at the gamut limit, over the look's %2%.").arg(gamut, 0, 'f', 1).arg(g.maxGamutLimitPct, 0, 'f', 1), gamut, g.maxGamutLimitPct));
  const double lift = shadowLift(grade);
  if (lift > g.maxLiftMaster + 1e-9) out->append(viol(q("maxLiftMaster"), QStringLiteral("The grade lifts the shadows by %1, over the look's %2.").arg(lift, 0, 'f', 3).arg(g.maxLiftMaster, 0, 'f', 3), lift, g.maxLiftMaster));
  if (skin.value(q("pct")).toDouble() > g.skinMinPct) {
    const double hue = skin.value(q("hueDeg")).toDouble();
    if (hue < g.skinBandMin || hue > g.skinBandMax)
      out->append(viol(q("skinHueBand"), QStringLiteral("Skin tones sit at %1 degrees on the vectorscope, outside the %2..%3 band.").arg(hue, 0, 'f', 1).arg(g.skinBandMin, 0, 'f', 0).arg(g.skinBandMax, 0, 'f', 0), hue, hue < g.skinBandMin ? g.skinBandMin : g.skinBandMax));
  }
  return Result::success();
}

struct Plan {
  const Item* item = nullptr; // only valid until the document changes: ids below
  QString id;
  PrimaryGrade current, proposed;
  QJsonObject before, predicted;
  QJsonArray violations;
  std::optional<double> suggested; // largest intensity that passes (only when the requested one does not)
  QJsonObject measured;            // after apply
  bool changed = false;
};

Result lookApplyCmd(CallContext& c, const QJsonObject& a) {
  const QString name = a.value(q("look")).toString();
  const Look* look = findLook(name);
  if (!look) return notFound(QStringLiteral("No look named \"%1\".").arg(name), QStringLiteral("Names: %1. See style.list.").arg(lookNames().join(QStringLiteral(", "))));
  const double intensity = std::clamp(a.value(q("intensity")).toDouble(1.0), 0.0, 1.0);
  const SpecOffsets off{a.value(q("warmth")).toDouble(), a.value(q("contrast")).toDouble(), a.value(q("saturation")).toDouble()};
  const bool replace = a.value(q("replace")).toBool(false), apply = a.value(q("apply")).toBool(false), force = a.value(q("force")).toBool(false);
  const LookGuards guards = guardsFromJson(a.value(q("guards")).toObject(), look->guards);
  const int samples = std::clamp(a.value(q("samples")).toInt(3), 1, 16);
  const TimelineDoc& d = c.session->project->doc();

  std::vector<Plan> plans;
  for (const QJsonValue& v : a.value(q("itemIds")).toArray()) {
    const Item* it = getItem(d, v.toString());
    if (!it) return notFound(QStringLiteral("Item %1 not found.").arg(v.toString()), QStringLiteral("Call timeline.get."));
    if (it->type() != ItemType::Video && it->type() != ItemType::Image) return invalid(QStringLiteral("Item %1 is %2: looks apply to video and image items.").arg(it->id, enumName(it->type())));
    Plan p;
    p.item = it;
    p.id = it->id;
    p.current = it->color && it->color->primaries ? *it->color->primaries : PrimaryGrade{};
    plans.push_back(p);
  }
  if (plans.empty()) return invalid(QStringLiteral("Pass itemIds: the video or image items to grade."));

  const double backoff[4] = {0.75, 0.5, 0.25, 0.0};
  bool allOk = true;
  for (Plan& p : plans) {
    const std::optional<PrimaryGrade> existing = replace ? std::nullopt : std::optional<PrimaryGrade>(p.current);
    std::vector<PrimaryGrade> grades{p.current, proposeLook(*look, intensity, off, existing)};
    for (double f : backoff) grades.push_back(proposeLook(*look, intensity * f, off, existing));
    p.proposed = grades[1];
    p.changed = !(p.proposed == p.current);
    std::vector<QJsonObject> metrics;
    if (Result r = predictGrades(c, p.item->id, grades, samples, &metrics); !r.ok) return r;
    p.before = metrics[0];
    p.predicted = metrics[1];
    if (Result r = guardViolations(c, p.item->id, p.before, p.predicted, guards, grades[1], p.current, &p.violations); !r.ok) return r;
    if (!p.violations.isEmpty()) {
      allOk = false;
      p.suggested = 0.0;
      for (size_t i = 0; i < 4; ++i) {
        QJsonArray vs;
        if (Result r = guardViolations(c, p.item->id, p.before, metrics[2 + i], guards, grades[2 + i], p.current, &vs); !r.ok) return r;
        if (vs.isEmpty()) {
          p.suggested = intensity * backoff[i];
          break;
        }
      }
    }
  }

  const auto itemsJson = [&] {
    QJsonArray arr;
    for (const Plan& p : plans) {
      QJsonObject o{{q("itemId"), p.id}, {q("currentPrimaries"), sf::toJson(p.current)}, {q("proposedPrimaries"), sf::toJson(p.proposed)}, {q("before"), summary(p.before)},
                    {q("predicted"), summary(p.predicted)}, {q("check"), QJsonObject{{q("ok"), p.violations.isEmpty()}, {q("violations"), p.violations}}}};
      if (!p.violations.isEmpty()) o.insert(q("suggestedIntensity"), p.suggested ? fin(*p.suggested, 100.0) : QJsonValue(QJsonValue::Null));
      if (!p.measured.isEmpty()) o.insert(q("measured"), summary(p.measured));
      arr.append(o);
    }
    return arr;
  };
  QJsonObject o{{q("look"), look->name}, {q("intensity"), intensity}, {q("offsets"), QJsonObject{{q("warmth"), off.warmth}, {q("contrast"), off.contrast}, {q("saturation"), off.saturation}}},
                {q("mode"), replace ? q("replace") : q("layer")}, {q("guards"), toJson(*look).value(q("guards")).toObject()}, {q("ok"), allOk}, {q("applied"), false}};
  QJsonArray notes;
  if (!replace) notes.append(q("The look is layered on each clip's current grade (replace=true drops the current grade first)."));
  if (!apply) {
    o.insert(q("items"), itemsJson());
    o.insert(q("notes"), notes);
    return Result::success(o);
  }
  if (!allOk && !force) {
    notes.append(q("Not applied: a guard failed (see items[].check and suggestedIntensity). Lower the intensity, relax `guards`, or pass force=true."));
    o.insert(q("items"), itemsJson());
    o.insert(q("notes"), notes);
    return Result::success(o);
  }
  QJsonArray ops;
  for (const Plan& p : plans) {
    if (!p.changed) continue;
    ItemColor cur = p.item->color.value_or(ItemColor{});
    cur.primaries = p.proposed == PrimaryGrade{} ? std::nullopt : std::optional<PrimaryGrade>(p.proposed);
    ops.append(QJsonObject{{q("type"), q("item.update")}, {q("itemId"), p.id}, {q("patch"), QJsonObject{{q("color"), sf::toJson(cur)}}}});
  }
  if (ops.isEmpty()) {
    notes.append(q("Nothing to write: the clips already carry this grade."));
    o.insert(q("items"), itemsJson());
    o.insert(q("notes"), notes);
    return Result::success(o);
  }
  const Result w = applyToSession(c, parseOps(ops), QStringLiteral("look.apply %1").arg(look->name), {{q("look"), look->name}});
  if (!w.ok) return w;
  o.insert(q("applied"), true);
  o.insert(q("undoLabel"), w.value.toObject().value(q("undoLabel")));
  // verify: measure the clips as they are now and compare with what was predicted
  bool verified = true;
  for (Plan& p : plans) {
    const Result r = c.engine->call(q("colour.analyze"), {{q("itemId"), p.id}, {q("samples"), samples}, {q("session"), c.session->id}});
    if (!r.ok) return r;
    p.measured = r.value.toObject();
    const QJsonObject pm = summary(p.predicted), mm = summary(p.measured);
    if (std::fabs(pm.value(q("exposureStops")).toDouble() - mm.value(q("exposureStops")).toDouble()) > 0.15 ||
        std::fabs(pm.value(q("castTemperature")).toDouble() - mm.value(q("castTemperature")).toDouble()) > 0.06)
      verified = false;
  }
  o.insert(q("verified"), verified);
  if (!verified) notes.append(q("The measured clip differs from the prediction by more than 0.15 stops or 0.06 of cast (the clip's input transform or LUT acts outside the primaries)."));
  o.insert(q("items"), itemsJson());
  o.insert(q("notes"), notes);
  return Result::success(o);
}

// ---------------------------------------------------------------- pacing.check

Result pacingCheckCmd(CallContext& c, const QJsonObject& a) {
  const QString name = a.value(q("profile")).toString();
  const PacingProfile* prof = findPacing(name);
  if (!prof) return notFound(QStringLiteral("No pacing profile named \"%1\".").arg(name), QStringLiteral("Names: %1. See style.list.").arg(pacingNames().join(QStringLiteral(", "))));
  QJsonObject args{{q("session"), c.session->id}};
  for (const char* k : {"trackId", "startFrame", "endFrame", "musicItemId", "beatFrames", "downbeatFrames"})
    if (a.contains(q(k))) args.insert(q(k), a.value(q(k)));
  const Result r = c.engine->call(q("timeline.stats"), args);
  if (!r.ok) return r;
  const QJsonObject st = r.value.toObject();
  const TimelineDoc& d = c.session->project->doc();
  const double fps = static_cast<double>(d.project.fps > 0 ? d.project.fps : 30);
  const tl::ShotStats stats = tl::computeStats(tl::shotsOnTrack(d, st.value(q("trackId")).toString()), fps, st.value(q("startFrame")).toInteger(), a.contains(q("endFrame")) ? st.value(q("endFrame")).toInteger() : 0);
  if (stats.shotCount == 0) return invalid(QStringLiteral("Nothing to check: no shots in the range."));

  const QJsonObject ba = st.value(q("beatAlignment")).toObject();
  std::optional<double> share;
  QJsonArray notes;
  if (ba.value(q("available")).toBool()) share = ba.value(q("beat")).toObject().value(q("withinPct")).toDouble();
  else if (prof->cutsOnBeatMinPct > 0) notes.append(QStringLiteral("cuts-on-beat share not checked: %1").arg(ba.value(q("reason")).toString()));
  const std::vector<PacingViolation> vs = checkPacing(*prof, stats, share, fps);
  QJsonArray v;
  for (const PacingViolation& x : vs) {
    QJsonObject o{{q("rule"), x.rule}, {q("frame"), x.frame ? QJsonValue(static_cast<double>(*x.frame)) : QJsonValue(QJsonValue::Null)}, {q("measured"), fin(x.measured)}, {q("limit"), fin(x.limit)}, {q("message"), x.message}};
    v.append(o);
  }
  const QJsonObject measured{{q("shotCount"), stats.shotCount}, {q("cutCount"), static_cast<int>(stats.cuts.size())}, {q("aslSec"), fin(stats.aslSec)}, {q("medianSec"), fin(stats.medianSec)}, {q("minSec"), fin(stats.minSec)},
                             {q("maxSec"), fin(stats.maxSec)}, {q("firstCutSec"), stats.firstCutSec >= 0 ? fin(stats.firstCutSec) : QJsonValue(QJsonValue::Null)},
                             {q("cutsOnBeatPct"), share ? fin(*share, 100.0) : QJsonValue(QJsonValue::Null)}};
  return Result::success(QJsonObject{{q("pass"), v.isEmpty()}, {q("profile"), toJson(*prof)}, {q("violations"), v}, {q("measured"), measured}, {q("stats"), st}, {q("notes"), notes}});
}

// ---------------------------------------------------------------- mix.applyProfile

Result mixApplyCmd(CallContext& c, const QJsonObject& a) {
  const QString name = a.value(q("profile")).toString();
  const MixProfile* prof = findMix(name);
  if (!prof) return notFound(QStringLiteral("No mix profile named \"%1\".").arg(name), QStringLiteral("Names: %1. See style.list.").arg(mixNames().join(QStringLiteral(", "))));
  const bool apply = a.value(q("apply")).toBool(false);
  const QString origin = c.origin;
  const auto withCommon = [&](QJsonObject args) {
    args.insert(q("session"), c.session->id);
    if (!origin.isEmpty()) args.insert(q("origin"), origin);
    return args;
  };

  QJsonObject duckArgs{{q("targetMarginDb"), prof->duckMarginDb}, {q("attackMs"), prof->attackMs}, {q("releaseMs"), prof->releaseMs}};
  for (const char* k : {"speechTrack", "speechTracks", "musicTrack", "ranges", "gapMs", "replaceExisting"})
    if (a.contains(q(k))) duckArgs.insert(q(k), a.value(q(k)));
  const QJsonObject normArgs{{q("target"), prof->targetLufs}, {q("truePeakCeiling"), prof->truePeakCeilingDb}, {q("scope"), a.value(q("scope")).toString(q("master"))}};

  QJsonObject o{{q("profile"), toJson(*prof)}, {q("applied"), false}};
  QJsonArray notes;
  QJsonObject ducking;
  bool duckable = false;
  QJsonObject duckDry = duckArgs;
  duckDry.insert(q("apply"), false);
  if (Result r = c.engine->call(q("audio.duck"), withCommon(duckDry)); r.ok) {
    ducking = r.value.toObject();
    duckable = !ducking.value(q("keyframes")).toArray().isEmpty();
  } else if (r.error.code == QLatin1String("invalid_params")) {
    ducking = QJsonObject{{q("skipped"), r.error.message}};
    notes.append(QStringLiteral("Ducking skipped: %1").arg(r.error.message));
  } else {
    return r;
  }
  QJsonObject normDry = normArgs;
  normDry.insert(q("apply"), false);
  const Result nd = c.engine->call(q("audio.normalize"), withCommon(normDry));
  if (!nd.ok) return nd;
  QJsonObject normalize = nd.value.toObject();
  if (duckable) notes.append(q("The loudness proposal is measured before the duck; with apply=true the normalize runs after it and is measured again."));

  if (apply) {
    // duck + normalize are ONE undo step: the group closes when this block ends (also on an early return, keeping what was written)
    struct Group {
      editor::Project& project;
      Group(editor::Project& p, const QString& label, const QString& actor) : project(p) { project.beginGroup(label, actor); }
      ~Group() { project.endGroup(); }
    } group(*c.session->project, QStringLiteral("mix.applyProfile %1").arg(prof->name), c.actor());
    int steps = 0;
    if (duckable) {
      QJsonObject da = duckArgs;
      da.insert(q("apply"), true);
      const Result r = c.engine->call(q("audio.duck"), withCommon(da));
      if (!r.ok) return r;
      ducking = r.value.toObject();
      ++steps;
    }
    QJsonObject na = normArgs;
    na.insert(q("apply"), true);
    const Result r = c.engine->call(q("audio.normalize"), withCommon(na));
    if (!r.ok) return r;
    normalize = r.value.toObject();
    if (normalize.value(q("applied")).toBool()) ++steps;
    o.insert(q("applied"), steps > 0);
    o.insert(q("undoSteps"), steps > 0 ? 1 : 0); // composed: the duck and the normalize are one history entry
    o.insert(q("writes"), steps);
    QJsonObject ca{{q("target"), prof->targetLufs}, {q("tolerance"), prof->toleranceLu}, {q("truePeakCeiling"), prof->truePeakCeilingDb}, {q("minSpeechMarginDb"), prof->minSpeechMarginDb()}};
    for (const char* k : {"speechTrack", "speechTracks", "musicTrack", "ranges", "gapMs"})
      if (a.contains(q(k))) ca.insert(q(k), a.value(q(k)));
    ca.insert(q("session"), c.session->id); // read-only: no origin
    const Result chk = c.engine->call(q("audio.check"), ca);
    o.insert(chk.ok ? q("check") : q("checkError"), chk.ok ? chk.value : QJsonValue(chk.error.message));
  }
  o.insert(q("ducking"), ducking);
  o.insert(q("normalize"), normalize);
  o.insert(q("notes"), notes);
  return Result::success(o);
}

} // namespace

void registerStyleCommands(Engine& e) {
  e.add(spec("style.list", "", "engine", false,
             QStringLiteral("The style library (taste as data, compiled in): `kind` look | pacing | mix (omit for all three), `name` for one item. Looks are primary colour corrections (exposure, temperature, tint, contrast, saturation, lift / gamma / gain) with guard rules "
                            "(maxChromaP95, maxGamutLimitPct, maxLiftMaster, skin hue shift and band, crushed / clipped growth); pacing profiles give an ASL range, the share of cuts on the beat, a hook time and shot limits; mix profiles give a loudness target, true-peak "
                            "ceiling, speech-over-music margin and duck attack / release. Each kind comes with its JSON schema."),
             js::obj({{q("kind"), js::choice({q("look"), q("pacing"), q("mix")})}, {q("name"), js::str()}}), listCmd, false));
  e.add(spec("style.spec.schema", "", "engine", false,
             QStringLiteral("The JSON schema of a style spec: the small, bounded object a model fills in to choose a style ({look, intensity 0..1, warmth / contrast / saturation offsets within +-0.2, pacing, mix}). Names come from style.list. "
                            "Returns {schema, example, use}: the spec's fields feed look.apply, pacing.check and mix.applyProfile."),
             js::obj({}), specSchemaCmd, false));
  e.add(spec("look.apply", "", "colour", true,
             QStringLiteral("Grade clips with a look from the library. The look's parameters are scaled by `intensity` (0..1, default 1), nudged by the bounded offsets `warmth` (+-0.2, added to temperature), `contrast` and `saturation` (+-0.2, multiplicative) and layered on each clip's "
                            "current grade (`replace`=true drops it). Each clip is measured, graded on the CPU twin of the shader and checked against the look's guards (colour.check thresholds plus chroma, gamut, shadow lift and a skin hue band; override any in `guards`): "
                            "items[] holds currentPrimaries, proposedPrimaries, before / predicted (headline colour.analyze numbers) and check{ok, violations[]}; a failing clip gets suggestedIntensity, the largest of 75 / 50 / 25 / 0 % of the request that passes. "
                            "apply=true writes every clip as ONE undo step (refused while a guard fails, unless force=true) and measures again: items[].measured, verified."),
             js::obj({{q("look"), js::choice(lookNames())}, {q("intensity"), js::number(QStringLiteral("0..1, default 1"), 0, 1)}, {q("itemIds"), [] { QJsonObject o = js::array(js::str()); o.insert(q("minItems"), 1); return o; }()},
                      {q("warmth"), js::number(QStringLiteral("temperature offset, -0.2 .. 0.2"), -kMaxWarmthOffset, kMaxWarmthOffset)}, {q("contrast"), js::number(QStringLiteral("contrast offset, -0.2 .. 0.2"), -kMaxContrastOffset, kMaxContrastOffset)},
                      {q("saturation"), js::number(QStringLiteral("saturation offset, -0.2 .. 0.2"), -kMaxSaturationOffset, kMaxSaturationOffset)}, {q("replace"), js::boolean(QStringLiteral("Drop the clip's current grade instead of layering on it"))},
                      {q("guards"), js::any(QStringLiteral("Override look guards: any key of the look's guards (maxChromaP95, maxGamutLimitPct, maxLiftMaster, blacksCrushedIncrease, highlightsClippedIncrease, skinHueShiftDeg, skinMinPct, exposureShiftStops, skinHueBandDeg)"))},
                      {q("samples"), js::integer(QStringLiteral("Frames sampled per clip (default 3)"), 1, 16)}, {q("force"), js::boolean(QStringLiteral("Write even when a guard fails"))}, {q("apply"), js::boolean(QStringLiteral("Write the grades (undoable)"))}},
                     {q("look"), q("itemIds")}),
             lookApplyCmd));
  e.add(spec("pacing.check", "", "editor", false,
             QStringLiteral("Compare the edit's measured pacing (timeline.stats) with a pacing profile from the library (`profile`). Returns {pass, violations[], measured, profile, stats}; each violation is {rule, frame, measured, limit, message}: asl_too_fast / asl_too_slow (average shot length "
                            "outside the profile's range), hook (the first cut is later than hookMaxSec), beat_share (too few cuts within +-2 frames of a beat, only for music-driven profiles when a beat grid exists), min_shot / max_shot. Optional trackId, "
                            "startFrame / endFrame, musicItemId, beatFrames / downbeatFrames as in timeline.stats."),
             js::obj({{q("profile"), js::choice(pacingNames())}, {q("trackId"), js::str()}, {q("startFrame"), js::integer(QString(), 0)}, {q("endFrame"), js::integer(QString(), 1)}, {q("musicItemId"), js::str()},
                      {q("beatFrames"), js::array(js::integer())}, {q("downbeatFrames"), js::array(js::integer())}},
                     {q("profile")}),
             pacingCheckCmd));
  e.add(spec("mix.applyProfile", "", "audio", true,
             QStringLiteral("Mix to a profile from the library (`profile`): audio.duck with the profile's speech-over-music margin, attack and release, then audio.normalize to its loudness target and true-peak ceiling (the numbers are the profile's, nothing is guessed). Dry run: ducking "
                            "(ranges, keyframes, measured margin before / after) and normalize (before, proposal) exactly as those commands return them; ducking is skipped, with the reason, when no dialogue / music tracks can be found (speechTrack(s), musicTrack, ranges, gapMs, "
                            "replaceExisting as in audio.duck; `scope` as in audio.normalize). apply=true writes the duck and the normalize as ONE undo step (undoSteps = 1; writes counts the parts) and verifies with audio.check at the profile's tolerance."),
             js::obj({{q("profile"), js::choice(mixNames())}, {q("speechTrack"), js::str()}, {q("speechTracks"), js::array(js::str())}, {q("musicTrack"), js::str()},
                      {q("ranges"), js::array(js::obj({{q("startFrame"), js::integer(QString(), 0)}, {q("endFrame"), js::integer(QString(), 0)}}, {q("startFrame"), q("endFrame")}))}, {q("gapMs"), js::number(QString(), 0, 5000)},
                      {q("replaceExisting"), js::boolean()}, {q("scope"), js::str(QStringLiteral("master (default), a track id or an item id"))}, {q("apply"), js::boolean(QStringLiteral("Write the mix (undoable)"))}},
                     {q("profile")}),
             mixApplyCmd));
}

} // namespace sf::engine
