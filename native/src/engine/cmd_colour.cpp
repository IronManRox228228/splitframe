// Colour measurement and the deterministic proposals built on it: colour.analyze, colour.autoBalance, colour.match, colour.check.
// Measure, then decide: every number comes from render/colour_analysis (pure functions over the offscreen render's pixels).

#include "engine/cmd_util.h"
#include "render/colour_analysis.h"
#include "render/colour_capture.h"

#include <algorithm>
#include <cmath>

namespace sf::engine {

namespace {

constexpr int kDefaultSamples = 5;

const char* const kMetricsDoc =
    "Metrics (all measured on <= 480 px point-sampled frames of the clip alone; letterbox is excluded): "
    "luma{mean, median, p1..p99} = Rec.709 luma of the LINEAR image (1.0 = SDR white, 0.18 = mid grey); luma.stopsVs18Grey = log2(median / 0.18), the exposure in stops (0 = a median of mid grey); "
    "histogram.fraction = 64 bins of DISPLAY luma, bin 0 black, values are fractions of pixels; "
    "contrast{stops = log2(p99 / p1) of linear luma (each floored at 0.001), rms = standard deviation of display luma 0..1}; "
    "crushedPct{r,g,b,all} = percent of pixels at or below 2/255 on the display (all = display luma); clippedPct likewise at or above 253/255; "
    "cast{temperature, tint} = the colour cast of near-neutral pixels in the SAME units as primaries.temperature / primaries.tint (+temperature = warm, +tint = magenta), "
    "cast.proposedCorrection is its negation, cast.neutralPixelsPct says how many pixels it is based on (below 0.5 the cast is reported as 0); "
    "saturation{chromaMean, chromaP95 = display max(R,G,B) - min(R,G,B), 0..1; nearGamutLimitPct = percent of pixels with HSV saturation >= 0.95}; "
    "skin{pct = percent of pixels in the skin hue/saturation/luma band, hueDeg = mean hue angle on the Rec.709 vectorscope, offsetFromSkinLineDeg = hueDeg - 123 (the skin-tone line)}; "
    "range = min/max of exposure, crushed and clipped across the sampled frames.";

Result needItem(CallContext& c, const QString& id, const Item** out) {
  const Item* it = getItem(c.session->project->doc(), id);
  if (!it) return notFound(QStringLiteral("Item %1 not found.").arg(id), QStringLiteral("Call timeline.get."));
  if (it->type() != ItemType::Video && it->type() != ItemType::Image) return invalid(QStringLiteral("Item %1 is %2: colour applies to video and image items.").arg(id, enumName(it->type())));
  *out = it;
  return Result::success(QJsonObject{});
}

// Pixels of one item (alone on the canvas) at `samples` frames across its span.
Result captureItem(CallContext& c, const Item& item, int samples, bool keepPrimaries, render::Capture* out) {
  QString err;
  render::OffscreenRenderer* off = c.engine->renderer(&err);
  if (!off) return Result::failure(q("unavailable"), QStringLiteral("The renderer is not available: %1").arg(err));
  TimelineDoc doc = render::isolateItem(c.session->project->doc(), item.id, keepPrimaries);
  *out = render::captureFrames(*off, doc, assetTable(*c.session), render::sampleFrames(item, samples), true);
  if (!out->ok()) return Result::failure(q("unavailable"), out->error.isEmpty() ? QStringLiteral("Nothing could be measured: the clip has no pixels on the canvas.") : out->error);
  return Result::success(QJsonObject{});
}

// The cast's pixel selection for an item: its near-neutral pixels with NO primary correction, so a grade cannot change which pixels the cast is measured on
// (before / after comparisons then use one pixel set). `graded` is the capture of the item with its grade (reused when the item has none).
Result castMasksFor(CallContext& c, const Item& item, int samples, const render::Capture& graded, std::vector<std::vector<unsigned char>>* out) {
  if (!item.color || !item.color->primaries) {
    *out = render::neutralMasks(graded.frames);
    return Result::success(QJsonObject{});
  }
  render::Capture src;
  if (Result r = captureItem(c, item, samples, false, &src); !r.ok) return r;
  *out = render::neutralMasks(src.frames);
  return Result::success(QJsonObject{});
}

int samplesFrom(const QJsonObject& a) { return std::clamp(a.value(q("samples")).toInt(kDefaultSamples), 1, 16); }

QJsonArray frameList(const std::vector<Frame>& f) {
  QJsonArray a;
  for (Frame x : f) a.append(static_cast<double>(x));
  return a;
}

Result applyPrimaries(CallContext& c, const QJsonObject& a, const QString& itemId, const PrimaryGrade& g) {
  QJsonObject args{{q("itemId"), itemId}, {q("resetPrimaries"), true}, {q("primaries"), toJson(g)}};
  for (const char* k : {"session", "origin"})
    if (a.contains(q(k))) args.insert(q(k), a.value(q(k)));
  return c.engine->call(q("item.color.set"), args);
}

double r3(double v) { return std::round(v * 1000.0) / 1000.0; }

QJsonObject summary(const render::ColourMetrics& m) {
  return QJsonObject{{q("exposureStops"), r3(m.exposureStops)},   {q("contrastStops"), r3(m.contrastStops)}, {q("crushedPct"), r3(m.crushedPct.all)},
                     {q("clippedPct"), r3(m.clippedPct.all)},     {q("castTemperature"), r3(m.castTemperature)}, {q("castTint"), r3(m.castTint)},
                     {q("chromaMean"), r3(m.chromaMean)},         {q("skinPct"), r3(m.skinPct)},             {q("skinOffsetDeg"), r3(m.skinOffsetDeg)}};
}

Result analyzeCmd(CallContext& c, const QJsonObject& a) {
  const TimelineDoc& doc = c.session->project->doc();
  render::Capture cap;
  QJsonObject extra;
  if (a.contains(q("itemId"))) {
    const Item* it = nullptr;
    if (Result r = needItem(c, a.value(q("itemId")).toString(), &it); !r.ok) return r;
    const bool withGrade = a.value(q("withGrade")).toBool(true);
    if (Result r = captureItem(c, *it, samplesFrom(a), withGrade, &cap); !r.ok) return r;
    extra.insert(q("itemId"), it->id);
    extra.insert(q("withGrade"), withGrade);
    extra.insert(q("frameList"), frameList(render::sampleFrames(*it, samplesFrom(a))));
  } else {
    QString err;
    render::OffscreenRenderer* off = c.engine->renderer(&err);
    if (!off) return Result::failure(q("unavailable"), QStringLiteral("The renderer is not available: %1").arg(err));
    std::vector<Frame> frames;
    if (a.contains(q("frame"))) {
      frames.push_back(a.value(q("frame")).toInteger());
      if (frames.front() < 0) return invalid(QStringLiteral("frame must be >= 0."));
    } else {
      const Frame total = std::max<Frame>(docDurationFrames(doc), 1);
      const int n = samplesFrom(a);
      for (int i = 0; i < n; ++i) frames.push_back(std::min<Frame>(total - 1, static_cast<Frame>((i + 0.5) * static_cast<double>(total) / n)));
      frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    }
    cap = render::captureFrames(*off, doc, assetTable(*c.session), frames, false);
    if (!cap.ok()) return Result::failure(q("unavailable"), cap.error);
    extra.insert(q("frameList"), frameList(frames));
  }
  std::vector<std::vector<unsigned char>> masks;
  if (a.contains(q("itemId"))) {
    const Item* it = getItem(doc, a.value(q("itemId")).toString());
    if (Result r = castMasksFor(c, *it, samplesFrom(a), cap, &masks); !r.ok) return r;
  }
  QJsonObject o = render::analyzeFrames(cap.frames, masks.empty() ? nullptr : &masks).toJson();
  if (!masks.empty()) {
    QJsonObject cast = o.value(q("cast")).toObject();
    cast.insert(q("selection"), q("near-neutral pixels of the ungraded clip (the same pixels with or without a grade)"));
    o.insert(q("cast"), cast);
  }
  for (auto it = extra.begin(); it != extra.end(); ++it) o.insert(it.key(), it.value());
  return Result::success(o);
}

Result autoBalanceCmd(CallContext& c, const QJsonObject& a) {
  const Item* it = nullptr;
  if (Result r = needItem(c, a.value(q("itemId")).toString(), &it); !r.ok) return r;
  render::Capture cap;
  if (Result r = captureItem(c, *it, samplesFrom(a), false, &cap); !r.ok) return r;
  render::PixelSet pooled;
  for (const render::PixelSet& f : cap.frames) pooled.append(f);
  render::AutoBalanceOptions opt;
  if (a.contains(q("exposureTarget"))) opt.exposureTarget = a.value(q("exposureTarget")).toDouble();
  if (a.contains(q("toleranceStops"))) opt.toleranceStops = a.value(q("toleranceStops")).toDouble();
  const render::Proposal p = render::proposeAutoBalance(pooled, opt);
  QJsonObject o{{q("itemId"), it->id},
                {q("proposedPrimaries"), toJson(p.primaries)},
                {q("before"), p.before.toJson()},
                {q("predicted"), p.predicted.toJson()},
                {q("summary"), QJsonObject{{q("before"), summary(p.before)}, {q("predicted"), summary(p.predicted)}}},
                {q("notes"), QJsonArray::fromStringList(p.notes)},
                {q("applied"), false}};
  if (a.value(q("apply")).toBool()) {
    Result r = applyPrimaries(c, a, it->id, p.primaries);
    if (!r.ok) return r;
    o.insert(q("applied"), true);
    o.insert(q("undoLabel"), r.value.toObject().value(q("undoLabel")));
  }
  return Result::success(o);
}

Result matchCmd(CallContext& c, const QJsonObject& a) {
  const Item *it = nullptr, *ref = nullptr;
  if (Result r = needItem(c, a.value(q("itemId")).toString(), &it); !r.ok) return r;
  if (Result r = needItem(c, a.value(q("referenceItemId")).toString(), &ref); !r.ok) return r;
  if (it->id == ref->id) return invalid(QStringLiteral("itemId and referenceItemId are the same clip."));
  render::Capture tgt, rf;
  if (Result r = captureItem(c, *it, samplesFrom(a), false, &tgt); !r.ok) return r;
  if (Result r = captureItem(c, *ref, samplesFrom(a), true, &rf); !r.ok) return r;
  render::PixelSet pooled;
  for (const render::PixelSet& f : tgt.frames) pooled.append(f);
  const render::ColourMetrics refMetrics = render::analyzeFrames(rf.frames);
  const render::Proposal p = render::proposeMatch(pooled, refMetrics);
  QJsonObject o{{q("itemId"), it->id},
                {q("referenceItemId"), ref->id},
                {q("proposedPrimaries"), toJson(p.primaries)},
                {q("residual"), p.residual},
                {q("plausible"), p.plausible},
                {q("iterations"), p.iterations},
                {q("before"), summary(p.before)},
                {q("predicted"), summary(p.predicted)},
                {q("reference"), summary(refMetrics)},
                {q("notes"), QJsonArray::fromStringList(p.notes)},
                {q("applied"), false}};
  if (a.value(q("apply")).toBool()) {
    Result r = applyPrimaries(c, a, it->id, p.primaries);
    if (!r.ok) return r;
    o.insert(q("applied"), true);
    o.insert(q("undoLabel"), r.value.toObject().value(q("undoLabel")));
  }
  return Result::success(o);
}

Result checkCmd(CallContext& c, const QJsonObject& a) {
  const Item* it = nullptr;
  if (Result r = needItem(c, a.value(q("itemId")).toString(), &it); !r.ok) return r;
  if (!a.value(q("before")).isObject()) return invalid(QStringLiteral("before must be the object colour.analyze returned for this clip before the change."));
  const render::ColourMetrics before = render::ColourMetrics::fromJson(a.value(q("before")).toObject());
  if (!before.valid) return invalid(QStringLiteral("before holds no measurement."));
  render::ColourMetrics after;
  if (a.value(q("after")).isObject()) {
    after = render::ColourMetrics::fromJson(a.value(q("after")).toObject());
  } else {
    render::Capture cap;
    if (Result r = captureItem(c, *it, samplesFrom(a), true, &cap); !r.ok) return r;
    std::vector<std::vector<unsigned char>> masks;
    if (Result r = castMasksFor(c, *it, samplesFrom(a), cap, &masks); !r.ok) return r;
    after = render::analyzeFrames(cap.frames, &masks);
  }
  const render::CheckThresholds t = render::thresholdsFromJson(a.value(q("thresholds")).toObject());
  const std::vector<render::Violation> v = render::checkColour(before, after, t);
  return Result::success(QJsonObject{{q("ok"), v.empty()},
                                     {q("violations"), render::violationsJson(v)},
                                     {q("before"), summary(before)},
                                     {q("after"), summary(after)},
                                     {q("exposureShiftStops"), r3(after.exposureStops - before.exposureStops)}});
}

} // namespace

// Declared in cmd_util.h, used by the style library commands: the clip's UNGRADED pixels graded by each candidate on the CPU twin of the shader.
Result predictGrades(CallContext& c, const QString& itemId, const std::vector<PrimaryGrade>& grades, int samples, std::vector<QJsonObject>* metrics) {
  const Item* it = nullptr;
  if (Result r = needItem(c, itemId, &it); !r.ok) return r;
  render::Capture cap;
  if (Result r = captureItem(c, *it, std::clamp(samples, 1, 16), false, &cap); !r.ok) return r;
  metrics->clear();
  const std::vector<std::vector<unsigned char>> masks = render::neutralMasks(cap.frames); // cap is ungraded: every candidate's cast is measured on the same pixels
  for (const PrimaryGrade& g : grades) {
    std::vector<render::PixelSet> graded;
    for (const render::PixelSet& f : cap.frames) graded.push_back(render::gradePixels(f, g));
    metrics->push_back(render::analyzeFrames(graded, &masks).toJson());
  }
  return Result::success(QJsonObject{});
}

void registerColourCommands(Engine& e) {
  const QString dflt = QStringLiteral("Frames sampled evenly across the clip (default 5, 1..16).");
  e.add(spec("colour.analyze", "", "engine", false,
             QStringLiteral("Measure the colour of a clip (itemId: that clip alone, with its grade unless withGrade=false, sampled at `samples` frames evenly across its span) or of the composited timeline "
                            "(`frame`, or samples across the whole timeline). Deterministic: the same picture gives the same numbers, so measure before and after every colour decision. ") +
                 QString::fromLatin1(kMetricsDoc),
             js::obj({{q("itemId"), js::str(QStringLiteral("A video or image item; omit to measure the composite"))},
                      {q("frame"), js::integer(QStringLiteral("Timeline frame, composite only"), 0)},
                      {q("samples"), js::integer(dflt, 1, 16)},
                      {q("withGrade"), js::boolean(QStringLiteral("itemId only: include the clip's current primary correction (default true); false measures the ungraded source"))}}),
             analyzeCmd));
  e.add(spec("colour.autoBalance", "", "colour", true,
             QStringLiteral("Propose primaries that neutralise the clip's measured colour cast (temperature / tint, from near-neutral pixels) and bring its median exposure to exposureTarget (linear luma, default 0.18 = mid grey; "
                            "left alone within toleranceStops, default 0.25; never beyond +-2 stops), without growing crushed or clipped pixels by more than 1 percentage point. The proposal is absolute (measured on the "
                            "ungraded source, it replaces the current primaries) and verified on the CPU twin of the shader: `predicted` holds the metrics after it. With apply=true it is written like item.color.set "
                            "(one undo step, actor from origin). before / predicted are colour.analyze results; summary lists the headline numbers."),
             js::obj({{q("itemId"), js::str()},
                      {q("exposureTarget"), js::number(QStringLiteral("Target median linear luma, default 0.18"), 0.02, 0.9)},
                      {q("toleranceStops"), js::number(QStringLiteral("Leave exposure alone when already this close, default 0.25"), 0, 2)},
                      {q("samples"), js::integer(dflt, 1, 16)},
                      {q("apply"), js::boolean(QStringLiteral("Write the proposal to the clip (undoable)"))}},
                     {q("itemId")}),
             autoBalanceCmd));
  e.add(spec("colour.match", "", "colour", true,
             QStringLiteral("Shot match: propose primaries that make clip `itemId` look like clip `referenceItemId` (as the reference is graded now). Reinhard-style: the per-channel log2 mean and standard deviation of the "
                            "target are moved onto the reference's using exposure, contrast, temperature, tint and saturation (per-channel gain only for a residual the clamps leave), iterated against the CPU twin of the shader. "
                            "`residual.rmsStops` is the remaining error in stops (below 0.1 is a close match); `plausible` false means the grade needed extreme values (see notes): do not apply it blindly; `before` / `predicted` / `reference` give exposureStops, contrastStops, crushedPct, clippedPct, "
                            "castTemperature / castTint, chromaMean and skin figures. The proposal is absolute (replaces the clip's primaries). apply=true writes it like item.color.set (one undo step). "
                            "Follow with colour.check to make sure nothing was lost."),
             js::obj({{q("itemId"), js::str()}, {q("referenceItemId"), js::str()}, {q("samples"), js::integer(dflt, 1, 16)}, {q("apply"), js::boolean(QStringLiteral("Write the proposal to the clip (undoable)"))}},
                     {q("itemId"), q("referenceItemId")}),
             matchCmd));
  e.add(spec("colour.check", "", "colour", false,
             QStringLiteral("Verify a colour change: pass the object colour.analyze returned for this clip BEFORE the change as `before`; the clip is measured again (or pass `after`) and compared. Returns {ok, violations[]}, "
                            "each {code, message, value, limit}: blacksCrushedIncrease (percentage points more pixels at display luma <= 2/255, default limit 2), highlightsClippedIncrease (more pixels >= 253/255, default 2), "
                            "skinHueShiftDeg (mean skin hue moved on the vectorscope, default 6 degrees, only when skin covers more than skinMinPct = 2 %), exposureShiftStops (median moved more than the requested "
                            "amount, default 1 stop). Override any limit in `thresholds`."),
             js::obj({{q("itemId"), js::str()},
                      {q("before"), js::any(QStringLiteral("The colour.analyze result from before the change"))},
                      {q("after"), js::any(QStringLiteral("Optional colour.analyze result to compare instead of measuring now"))},
                      {q("thresholds"), js::obj({{q("blacksCrushedIncrease"), js::number(QStringLiteral("percentage points"), 0, 100)},
                                                 {q("highlightsClippedIncrease"), js::number(QStringLiteral("percentage points"), 0, 100)},
                                                 {q("skinHueShiftDeg"), js::number(QStringLiteral("degrees"), 0, 180)},
                                                 {q("skinMinPct"), js::number(QStringLiteral("percent of pixels"), 0, 100)},
                                                 {q("exposureShiftStops"), js::number(QStringLiteral("stops: the change you asked for"), 0, 10)}},
                                                {})},
                      {q("samples"), js::integer(dflt, 1, 16)}},
                     {q("itemId"), q("before")}),
             checkCmd));
}

} // namespace sf::engine
