// Measured editing and the deterministic proposals built on it: timeline.stats, edit.snapToBeats, edit.safeCutPoint, timeline.check.
// Measure, propose, verify: every number comes from engine/timeline_analysis (pure functions over the document); the beat grid comes
// from music.structure and the speech spans from index.speech (or an energy gate), both called through the command registry.

#include "engine/cmd_util.h"
#include "engine/timeline_analysis.h"

#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace sf::engine {

namespace {

constexpr Frame kAlignTolerance = 2; // a cut within +-2 frames of a beat counts as on the beat
constexpr double kWordGapMs = 80.0;  // speech gaps shorter than this belong to the same word run

QJsonValue fin(double v, double scale = 100.0) { return std::isfinite(v) ? QJsonValue(std::round(v * scale) / scale) : QJsonValue(QJsonValue::Null); }

double fpsOf(const TimelineDoc& d) { return static_cast<double>(d.project.fps > 0 ? d.project.fps : 30); }

QJsonArray framesJson(const std::vector<Frame>& v) {
  QJsonArray a;
  for (Frame f : v) a.append(static_cast<double>(f));
  return a;
}

std::vector<Frame> framesFrom(const QJsonArray& a) {
  std::vector<Frame> v;
  for (const QJsonValue& x : a) v.push_back(static_cast<Frame>(std::llround(x.toDouble())));
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

// ---------------------------------------------------------------- the storyline

// The track the cuts live on: trackId when given, else the video track with the most shots.
Result storyTrack(const TimelineDoc& d, const QJsonObject& a, QString* out) {
  QString id = a.value(q("trackId")).toString();
  if (id.isEmpty()) {
    id = tl::primaryVideoTrack(d);
    if (id.isEmpty()) return invalid(QStringLiteral("No video track holds video or image items: nothing to measure."));
  } else if (!getTrack(d, id)) {
    return notFound(QStringLiteral("Track %1 not found.").arg(id), QStringLiteral("Call timeline.get."));
  }
  *out = id;
  return Result::success();
}

// ---------------------------------------------------------------- the beat grid

struct Grid {
  bool ok = false;
  QString reason;    // why there is none
  QString source;    // music.structure | explicit
  QString musicItemId;
  double bpm = 0, confidence = 0;
  std::vector<Frame> beats, downbeats;
};

QString guessMusicItem(const TimelineDoc& d) {
  const QStringList tracks = guessMusicTracks(d);
  const Item* best = nullptr;
  for (const Item& it : d.items) {
    if (!tracks.contains(it.trackId) || it.muted || it.type() != ItemType::Audio || !it.assetId) continue;
    if (!best || it.durationFrames > best->durationFrames) best = &it;
  }
  return best ? best->id : QString();
}

// `beatFrames` / `downbeatFrames` (timeline frames) when given, else the beats of the music item (musicItemId, or the longest audio item on
// the music track) as music.structure measures them.
Grid resolveGrid(CallContext& c, const QJsonObject& a) {
  Grid g;
  if (a.contains(q("beatFrames")) || a.contains(q("downbeatFrames"))) {
    g.source = q("explicit");
    g.beats = framesFrom(a.value(q("beatFrames")).toArray());
    g.downbeats = framesFrom(a.value(q("downbeatFrames")).toArray());
    if (g.beats.empty()) g.beats = g.downbeats;
    g.ok = !g.beats.empty();
    if (!g.ok) g.reason = q("beatFrames is empty");
    return g;
  }
  const TimelineDoc& d = c.session->project->doc();
  g.musicItemId = a.value(q("musicItemId")).toString();
  if (g.musicItemId.isEmpty()) g.musicItemId = guessMusicItem(d);
  if (g.musicItemId.isEmpty()) {
    g.reason = q("no music item on the timeline (pass musicItemId or beatFrames)");
    return g;
  }
  g.source = q("music.structure");
  const Result r = c.engine->call(q("music.structure"), {{q("itemId"), g.musicItemId}, {q("session"), c.session->id}});
  if (!r.ok) {
    g.reason = r.error.message;
    return g;
  }
  const QJsonObject s = r.value.toObject();
  if (!s.value(q("valid")).toBool()) {
    g.reason = QStringLiteral("music.structure found no tempo: %1").arg(s.value(q("note")).toString());
    return g;
  }
  g.bpm = s.value(q("bpm")).toDouble();
  g.confidence = s.value(q("tempoConfidence")).toDouble();
  g.beats = framesFrom(s.value(q("beatFrames")).toArray());
  g.downbeats = framesFrom(s.value(q("downbeatFrames")).toArray());
  g.ok = !g.beats.empty();
  if (!g.ok) g.reason = q("music.structure returned no beats");
  return g;
}

QJsonObject gridJson(const Grid& g) {
  QJsonObject o{{q("available"), g.ok}, {q("source"), g.source}};
  if (!g.ok) o.insert(q("reason"), g.reason);
  if (!g.musicItemId.isEmpty()) o.insert(q("musicItemId"), g.musicItemId);
  if (g.ok) {
    o.insert(q("beats"), static_cast<int>(g.beats.size()));
    o.insert(q("downbeats"), static_cast<int>(g.downbeats.size()));
    if (g.bpm > 0) {
      o.insert(q("bpm"), fin(g.bpm));
      o.insert(q("tempoConfidence"), fin(g.confidence));
    }
  }
  return o;
}

QJsonObject alignJson(const tl::BeatAlignment& a, bool withOffsets) {
  QJsonObject o{{q("cuts"), a.cuts}, {q("withinTolerance"), a.within}, {q("withinPct"), fin(a.withinPct)}, {q("toleranceFrames"), static_cast<double>(a.tolerance)},
                {q("meanOffsetMs"), fin(a.meanOffsetMs)}, {q("meanAbsOffsetMs"), fin(a.meanAbsOffsetMs)}};
  if (withOffsets) {
    QJsonArray off;
    for (const tl::CutOffset& c : a.perCut) off.append(static_cast<double>(c.cut - c.beat));
    o.insert(q("offsetsFrames"), off);
  }
  return o;
}

// ---------------------------------------------------------------- speech

// Speech spans (timeline frames) of the audio the storyline carries: the audible items on `trackId` and on the dialogue tracks, music excluded.
Result speechSpans(CallContext& c, const QString& trackId, std::vector<tl::FrameSpan>* out, QString* source) {
  const TimelineDoc& d = c.session->project->doc();
  const QStringList music = guessMusicTracks(d);
  QStringList tracks = guessSpeechTracks(d);
  if (!tracks.contains(trackId)) tracks << trackId;
  QStringList ids;
  for (const Item& it : d.items)
    if (tracks.contains(it.trackId) && !music.contains(it.trackId) && !it.muted && it.assetId && (it.type() == ItemType::Audio || it.type() == ItemType::Video)) ids << it.id;
  out->clear();
  *source = q("none");
  if (ids.isEmpty()) return Result::success();
  std::vector<std::pair<Frame, Frame>> r;
  if (Result res = itemSpeechRanges(c, ids, kWordGapMs, &r, source); !res.ok) return res;
  for (const auto& [s, e] : r) out->push_back({s, e});
  return Result::success();
}

// ---------------------------------------------------------------- timeline.stats

QJsonObject statsJson(const tl::ShotStats& s, double f) {
  QJsonArray shortest;
  for (const tl::ShortShot& x : s.shortest)
    shortest.append(QJsonObject{{q("itemId"), x.itemId}, {q("startFrame"), static_cast<double>(x.start)}, {q("frames"), static_cast<double>(x.length)}, {q("seconds"), fin(static_cast<double>(x.length) / f, 1000.0)}});
  QJsonArray rhythm;
  for (int n : s.cutsPer10s) rhythm.append(n);
  return QJsonObject{{q("startFrame"), static_cast<double>(s.from)}, {q("endFrame"), static_cast<double>(s.to)}, {q("durationSec"), fin(s.durationSec, 1000.0)}, {q("shotCount"), s.shotCount},
                     {q("cutCount"), static_cast<int>(s.cuts.size())}, {q("cuts"), framesJson(s.cuts)}, {q("aslSec"), fin(s.aslSec, 1000.0)}, {q("medianSec"), fin(s.medianSec, 1000.0)},
                     {q("stdevSec"), fin(s.stdevSec, 1000.0)}, {q("minSec"), fin(s.minSec, 1000.0)}, {q("maxSec"), fin(s.maxSec, 1000.0)}, {q("shortest"), shortest},
                     {q("cutsPerMinute"), fin(s.cutsPerMinute)}, {q("cutRhythm"), QJsonObject{{q("windowSec"), 10}, {q("cutsPerWindow"), rhythm}}},
                     {q("firstCutSec"), s.firstCutSec >= 0 ? fin(s.firstCutSec, 1000.0) : QJsonValue(QJsonValue::Null)},
                     {q("hook"), QJsonObject{{q("firstCutSec"), s.firstCutSec >= 0 ? fin(s.firstCutSec, 1000.0) : QJsonValue(QJsonValue::Null)}}}};
}

Result statsCmd(CallContext& c, const QJsonObject& a) {
  const TimelineDoc& d = c.session->project->doc();
  QString track;
  if (Result r = storyTrack(d, a, &track); !r.ok) return r;
  const Frame from = a.value(q("startFrame")).toInteger(0), to = a.value(q("endFrame")).toInteger(0);
  if (a.contains(q("endFrame")) && to <= from) return invalid(QStringLiteral("The range is empty (startFrame %1, endFrame %2).").arg(from).arg(to));
  const double f = fpsOf(d);
  const tl::ShotStats s = tl::computeStats(tl::shotsOnTrack(d, track), f, from, to);
  QJsonObject o = statsJson(s, f);
  o.insert(q("trackId"), track);
  o.insert(q("fps"), f);

  const Grid g = resolveGrid(c, a);
  QJsonObject ba = gridJson(g);
  if (g.ok) {
    ba.insert(q("beat"), alignJson(tl::alignment(s.cuts, g.beats, f, kAlignTolerance), true));
    if (!g.downbeats.empty()) ba.insert(q("downbeat"), alignJson(tl::alignment(s.cuts, g.downbeats, f, kAlignTolerance), false));
  }
  o.insert(q("beatAlignment"), ba);
  return Result::success(o);
}

// ---------------------------------------------------------------- edit.snapToBeats

std::vector<Frame> cutsOf(const TimelineDoc& d, const QString& track) {
  std::vector<Frame> cuts;
  const std::vector<tl::Shot> shots = tl::shotsOnTrack(d, track);
  for (size_t i = 1; i < shots.size(); ++i) cuts.push_back(shots[i].start);
  return cuts;
}

QJsonObject trimOp(const QString& itemId, const char* edge, Frame frame) {
  return QJsonObject{{q("type"), q("item.trim")}, {q("itemId"), itemId}, {q("edge"), q(edge)}, {q("frame"), static_cast<double>(frame)}, {q("ripple"), false}};
}

Result snapCmd(CallContext& c, const QJsonObject& a) {
  const editor::Project& p = *c.session->project;
  const TimelineDoc& d = p.doc();
  const double f = fpsOf(d);
  QString track;
  if (Result r = storyTrack(d, a, &track); !r.ok) return r;
  const QString to = a.value(q("to")).toString(q("beat"));
  const bool downbeat = to == QLatin1String("downbeat");

  tl::SnapInput in;
  in.doc = &d;
  in.trackId = track;
  for (const QJsonValue& v : a.value(q("itemIds")).toArray()) {
    const Item* it = getItem(d, v.toString());
    if (!it) return notFound(QStringLiteral("Item %1 not found.").arg(v.toString()), QStringLiteral("Call timeline.get."));
    if (it->trackId != track) return invalid(QStringLiteral("Item %1 is not on track %2.").arg(it->id, track));
    in.itemIds.push_back(it->id);
  }
  in.maxShift = a.value(q("maxShiftFrames")).toInteger(6);
  in.allowWordClip = a.value(q("allowWordClip")).toBool(false);
  const Grid g = resolveGrid(c, a);
  if (!g.ok) return invalid(QStringLiteral("No beat grid: %1").arg(g.reason));
  in.grid = downbeat ? g.downbeats : g.beats;
  if (in.grid.empty()) return invalid(QStringLiteral("The beat grid has no downbeats."));
  QString speechSource = q("none");
  if (!in.allowWordClip)
    if (Result r = speechSpans(c, track, &in.speech, &speechSource); !r.ok) return r;
  for (const tl::Shot& s : tl::shotsOnTrack(d, track)) {
    const Item* it = getItem(d, s.itemId);
    const Asset* as = it && it->assetId ? p.asset(*it->assetId) : nullptr;
    if (as && as->kind != AssetKind::Image && as->durationMs > 0) in.sourceFrames[as->id] = static_cast<Frame>(std::llround(static_cast<double>(as->durationMs) / 1000.0 * f));
  }

  const std::vector<Frame> cutsBefore = cutsOf(d, track);
  const tl::SnapPlan plan = tl::planSnap(in);
  const tl::BeatAlignment before = tl::alignment(cutsBefore, in.grid, f, kAlignTolerance);

  // predicted cut list: every planned move applied
  std::vector<Frame> predicted = cutsBefore;
  for (const tl::SnapMove& m : plan.moves) std::replace(predicted.begin(), predicted.end(), m.from, m.to);
  std::sort(predicted.begin(), predicted.end());
  const tl::BeatAlignment predictedAl = tl::alignment(predicted, in.grid, f, kAlignTolerance);

  struct Row {
    Frame frame;
    QJsonObject json;
  };
  std::vector<Row> rows;
  for (const tl::SnapMove& m : plan.moves)
    rows.push_back({m.from, QJsonObject{{q("frame"), static_cast<double>(m.from)}, {q("targetFrame"), static_cast<double>(m.to)}, {q("shiftFrames"), static_cast<double>(m.shift)},
                                        {q("shiftMs"), fin(static_cast<double>(m.shift) / f * 1000.0)}, {q("leftItemId"), m.leftId}, {q("rightItemId"), m.rightId},
                                        {q("method"), QStringLiteral("item.trim: %1 edge of %2 and %3 edge of %4, ripple=false")
                                                          .arg(q("out"), m.leftId, q("in"), m.rightId)},
                                        {q("wasInsideWord"), m.wasInsideWord}}});
  for (Frame fr : plan.alreadyOnGrid)
    rows.push_back({fr, QJsonObject{{q("frame"), static_cast<double>(fr)}, {q("targetFrame"), static_cast<double>(fr)}, {q("shiftFrames"), 0}, {q("shiftMs"), 0}, {q("method"), q("none (already on the grid)")}}});
  std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.frame < y.frame; });
  QJsonArray cuts;
  for (const Row& r : rows) cuts.append(r.json);
  QJsonArray skipped;
  for (const tl::SnapSkip& s : plan.skipped) {
    QJsonObject o{{q("frame"), static_cast<double>(s.frame)}, {q("reason"), s.reason}, {q("detail"), s.detail}, {q("leftItemId"), s.leftId}, {q("rightItemId"), s.rightId}};
    if (s.suggestedFrame) o.insert(q("suggestedFrame"), static_cast<double>(*s.suggestedFrame));
    skipped.append(o);
  }

  QJsonObject o{{q("trackId"), track}, {q("to"), to}, {q("maxShiftFrames"), static_cast<double>(in.maxShift)}, {q("allowWordClip"), in.allowWordClip}, {q("grid"), gridJson(g)}, {q("gridFrames"), static_cast<int>(in.grid.size())},
                {q("speechSource"), speechSource}, {q("considered"), plan.considered}, {q("cuts"), cuts}, {q("moved"), static_cast<int>(plan.moves.size())},
                {q("alreadyOnGrid"), static_cast<int>(plan.alreadyOnGrid.size())}, {q("skipped"), skipped}, {q("before"), alignJson(before, false)}, {q("predicted"), alignJson(predictedAl, false)},
                {q("applied"), false}};
  if (!a.value(q("apply")).toBool()) return Result::success(o);
  if (plan.moves.empty()) {
    o.insert(q("note"), q("Nothing to move: no cut can be snapped (see skipped)."));
    o.insert(q("after"), alignJson(before, false));
    return Result::success(o);
  }
  if (const Track* t = getTrack(d, track); t && t->locked) return Result::failure(q("edit_rejected"), QStringLiteral("Track \"%1\" is locked. Unlock it first.").arg(t->name));

  QJsonArray ops;
  for (const tl::SnapMove& m : plan.moves) {
    if (m.shift > 0) { // the right shot shrinks first, then the left one grows into the room
      ops.append(trimOp(m.rightId, "in", m.to));
      ops.append(trimOp(m.leftId, "out", m.to));
    } else {
      ops.append(trimOp(m.leftId, "out", m.to));
      ops.append(trimOp(m.rightId, "in", m.to));
    }
  }
  const Result w = applyToSession(c, parseOps(ops), q("edit.snapToBeats"), {{q("track"), track}, {q("to"), to}});
  if (!w.ok) return w;
  o.insert(q("applied"), true);
  o.insert(q("undoLabel"), w.value.toObject().value(q("undoLabel")));
  const tl::BeatAlignment after = tl::alignment(cutsOf(p.doc(), track), in.grid, f, kAlignTolerance);
  o.insert(q("after"), alignJson(after, false));
  o.insert(q("verified"), after.within >= before.within && std::fabs(after.meanAbsOffsetMs) <= std::fabs(before.meanAbsOffsetMs) + 1e-9);
  return Result::success(o);
}

// ---------------------------------------------------------------- edit.safeCutPoint

Result safeCutCmd(CallContext& c, const QJsonObject& a) {
  const TimelineDoc& d = c.session->project->doc();
  const QString id = a.value(q("itemId")).toString();
  const Item* it = getItem(d, id);
  if (!it) return notFound(QStringLiteral("Item %1 not found.").arg(id), QStringLiteral("Call timeline.get."));
  const Frame frame = a.value(q("frame")).toInteger(), maxShift = a.value(q("maxShiftFrames")).toInteger(12);
  const double f = fpsOf(d);
  QJsonObject o{{q("itemId"), id}, {q("frame"), static_cast<double>(frame)}, {q("maxShiftFrames"), static_cast<double>(maxShift)}};
  if (!it->assetId || it->muted || (it->type() != ItemType::Audio && it->type() != ItemType::Video)) {
    o.insert(q("insideWord"), false);
    o.insert(q("safe"), true);
    o.insert(q("safeFrame"), static_cast<double>(frame));
    o.insert(q("shiftFrames"), 0);
    o.insert(q("speechSource"), q("none"));
    o.insert(q("note"), q("The item carries no audible audio: any frame is safe."));
    return Result::success(o);
  }
  std::vector<std::pair<Frame, Frame>> r;
  QString source;
  if (Result res = itemSpeechRanges(c, {id}, a.value(q("gapMs")).toDouble(kWordGapMs), &r, &source); !res.ok) return res;
  std::vector<tl::FrameSpan> spans;
  for (const auto& [s, e] : r) spans.push_back({s, e});
  o.insert(q("speechSource"), source);
  o.insert(q("speechRanges"), static_cast<int>(spans.size()));
  const tl::FrameSpan* word = tl::insideWord(spans, frame);
  o.insert(q("insideWord"), word != nullptr);
  const std::optional<Frame> safe = tl::nearestGapFrame(spans, frame, maxShift);
  o.insert(q("safe"), safe.has_value());
  o.insert(q("safeFrame"), safe ? QJsonValue(static_cast<double>(*safe)) : QJsonValue(QJsonValue::Null));
  o.insert(q("shiftFrames"), safe ? QJsonValue(static_cast<double>(*safe - frame)) : QJsonValue(QJsonValue::Null));
  if (safe) o.insert(q("shiftMs"), fin(static_cast<double>(*safe - frame) / f * 1000.0));
  if (word) {
    o.insert(q("word"), QJsonObject{{q("startFrame"), static_cast<double>(word->start)}, {q("endFrame"), static_cast<double>(word->end)}});
    o.insert(q("alternatives"), QJsonObject{{q("before"), static_cast<double>(word->start)}, {q("after"), static_cast<double>(word->end)}});
    if (!safe) o.insert(q("note"), QStringLiteral("No speech gap within %1 frames of frame %2: raise maxShiftFrames or cut at the word edges (alternatives).").arg(maxShift).arg(frame));
  }
  return Result::success(o);
}

// ---------------------------------------------------------------- timeline.check

QJsonObject violation(const char* rule, const QJsonValue& frame, double f, const QJsonValue& measured, const QJsonValue& limit, const QString& message, const QString& itemId = {}) {
  QJsonObject o{{q("rule"), q(rule)}, {q("frame"), frame}, {q("timeSec"), frame.isNull() ? QJsonValue(QJsonValue::Null) : fin(frame.toDouble() / f, 1000.0)}, {q("measured"), measured}, {q("limit"), limit}, {q("message"), message}};
  if (!itemId.isEmpty()) o.insert(q("itemId"), itemId);
  return o;
}

// "9:16", "16:9", "1.78" -> width / height; 0 when unreadable
double parseAspect(const QString& s) {
  const QStringList parts = s.split(QRegularExpression(QStringLiteral("[:/x]")));
  bool ok1 = false, ok2 = true;
  const double w = parts.value(0).toDouble(&ok1);
  const double h = parts.size() > 1 ? parts.value(1).toDouble(&ok2) : 1.0;
  return ok1 && ok2 && w > 0 && h > 0 ? w / h : 0.0;
}

Result checkCmd(CallContext& c, const QJsonObject& a) {
  const editor::Project& p = *c.session->project;
  const TimelineDoc& d = p.doc();
  const double f = fpsOf(d);
  QString track;
  if (Result r = storyTrack(d, a, &track); !r.ok) return r;
  const double tol = a.value(q("toleranceSec")).toDouble(0.5), minShotSec = a.value(q("minShotSec")).toDouble(0.25);
  QJsonArray v, notes;
  const std::vector<tl::Shot> shots = tl::shotsOnTrack(d, track);

  for (const tl::Hole& h : tl::findGaps(shots))
    v.append(violation("gap", static_cast<double>(h.frame), f, static_cast<double>(h.frames), 0,
                       QStringLiteral("%1 frame(s) of nothing %2 (item.move or item.delete ripple closes it).").arg(h.frames).arg(h.beforeId.isEmpty() ? QStringLiteral("before the first shot") : QStringLiteral("after %1").arg(h.beforeId)), h.afterId));
  for (const tl::Hole& h : tl::findOverlaps(shots))
    v.append(violation("overlap", static_cast<double>(h.frame), f, static_cast<double>(h.frames), 0, QStringLiteral("%1 overlaps %2 by %3 frame(s) on the same track.").arg(h.beforeId, h.afterId).arg(h.frames), h.afterId));

  // flash frames: the copilot lint rule flash_frame (copilot/lint_engine.cpp), on every visual item of the checked track
  for (const tl::Shot& s : shots) {
    const double sec = static_cast<double>(s.length()) / f;
    if (s.length() < 3 || sec < minShotSec)
      v.append(violation("flash_frame", static_cast<double>(s.start), f, fin(sec, 1000.0), minShotSec, QStringLiteral("Shot %1 is only %2 frames (%3 s), under %4 s.").arg(s.itemId).arg(s.length()).arg(sec, 0, 'f', 3).arg(minShotSec), s.itemId));
  }

  if (a.contains(q("targetDurationSec"))) {
    const double target = a.value(q("targetDurationSec")).toDouble(), dur = static_cast<double>(docDurationFrames(d)) / f;
    if (std::fabs(dur - target) > tol)
      v.append(violation("duration", static_cast<double>(docDurationFrames(d)), f, fin(dur, 1000.0), target,
                         QStringLiteral("The timeline runs %1 s, %2 s %3 the %4 s target (tolerance %5 s).").arg(dur, 0, 'f', 2).arg(std::fabs(dur - target), 0, 'f', 2).arg(dur > target ? q("over") : q("under")).arg(target).arg(tol)));
  }

  const double canvas = d.project.height > 0 ? static_cast<double>(d.project.width) / static_cast<double>(d.project.height) : 0.0;
  if (a.contains(q("aspect"))) {
    const double want = parseAspect(a.value(q("aspect")).toString());
    if (want <= 0) return invalid(QStringLiteral("aspect must look like \"16:9\" or \"1.78\"."));
    if (canvas > 0 && std::fabs(canvas / want - 1.0) > 0.01)
      v.append(violation("aspect_canvas", QJsonValue(QJsonValue::Null), f, fin(canvas, 1000.0), fin(want, 1000.0), QStringLiteral("The canvas is %1x%2 (%3:1), not %4 (project.settings.set changes it).").arg(d.project.width).arg(d.project.height).arg(canvas, 0, 'f', 3).arg(a.value(q("aspect")).toString())));
  }
  for (const Item& it : d.items) {
    if ((it.type() != ItemType::Video && it.type() != ItemType::Image) || !it.assetId) continue;
    const Asset* as = p.asset(*it.assetId);
    if (!as || as->width <= 0 || as->height <= 0 || canvas <= 0) continue;
    const double ar = static_cast<double>(as->width) / static_cast<double>(as->height);
    if (std::fabs(ar / canvas - 1.0) > 0.02)
      v.append(violation("aspect_mismatch", static_cast<double>(it.startFrame), f, fin(ar, 1000.0), fin(canvas, 1000.0),
                         QStringLiteral("Clip %1 is %2x%3 (%4:1) on a %5:1 canvas: it letterboxes or crops.").arg(it.id).arg(as->width).arg(as->height).arg(ar, 0, 'f', 3).arg(canvas, 0, 'f', 3), it.id));
  }

  // cuts against speech
  const std::vector<Frame> cuts = cutsOf(d, track);
  std::vector<tl::FrameSpan> spans;
  QString speechSource;
  if (Result r = speechSpans(c, track, &spans, &speechSource); !r.ok) return r;
  for (Frame cut : cuts)
    if (const tl::FrameSpan* w = tl::insideWord(spans, cut)) {
      const Frame into = std::min(cut - w->start, w->end - cut);
      v.append(violation("word_clip", static_cast<double>(cut), f, static_cast<double>(into), 0,
                         QStringLiteral("The cut at frame %1 lands %2 frame(s) inside speech (%3..%4). edit.safeCutPoint finds the nearest gap.").arg(cut).arg(into).arg(w->start).arg(w->end)));
    }

  // cuts against the music
  QJsonObject beat{{q("checked"), false}};
  if (a.contains(q("maxOffBeatMs"))) {
    const double maxMs = a.value(q("maxOffBeatMs")).toDouble();
    const Grid g = resolveGrid(c, a);
    beat.insert(q("maxOffBeatMs"), maxMs);
    if (!g.ok) {
      beat.insert(q("reason"), g.reason);
      notes.append(QStringLiteral("off_beat skipped: %1").arg(g.reason));
    } else {
      beat.insert(q("checked"), true);
      const tl::BeatAlignment al = tl::alignment(cuts, g.beats, f, kAlignTolerance);
      beat.insert(q("alignment"), alignJson(al, false));
      for (const tl::CutOffset& co : al.perCut) {
        const double ms = std::fabs(static_cast<double>(co.cut - co.beat)) / f * 1000.0;
        if (ms > maxMs)
          v.append(violation("off_beat", static_cast<double>(co.cut), f, fin(ms), maxMs,
                             QStringLiteral("The cut at frame %1 is %2 ms from the nearest beat (frame %3), over %4 ms. edit.snapToBeats proposes the fix.").arg(co.cut).arg(ms, 0, 'f', 0).arg(co.beat).arg(maxMs, 0, 'f', 0)));
      }
    }
  }

  QJsonObject limits{{q("toleranceSec"), tol}, {q("minShotSec"), minShotSec}};
  for (const char* k : {"targetDurationSec", "aspect", "maxOffBeatMs"})
    if (a.contains(q(k))) limits.insert(q(k), a.value(q(k)));
  return Result::success(QJsonObject{{q("pass"), v.isEmpty()}, {q("violations"), v}, {q("trackId"), track}, {q("shots"), static_cast<int>(shots.size())}, {q("cuts"), static_cast<int>(cuts.size())},
                                     {q("durationSec"), fin(static_cast<double>(docDurationFrames(d)) / f, 1000.0)}, {q("speechSource"), speechSource}, {q("beat"), beat}, {q("limits"), limits}, {q("notes"), notes}});
}

} // namespace

void registerTimelineCommands(Engine& e) {
  const QString gridDoc = QStringLiteral("The beat grid comes from music.structure on `musicItemId` (default: the longest audio item on the music track), or from `beatFrames` / `downbeatFrames` (timeline frames) when given.");
  e.add(spec("timeline.stats", "", "editor", false,
             QStringLiteral("Measure the pacing of the storyline: shots are the video / image items of `trackId` (default: the video track with the most shots), cuts are the start frames of every shot after the first. Returns cuts, shotCount, "
                            "aslSec (average shot length), medianSec, stdevSec, minSec, maxSec, shortest[] (5 shortest shots), cutsPerMinute, cutRhythm{windowSec: 10, cutsPerWindow[]}, firstCutSec / hook (time to the first cut) "
                            "and, when a music item exists, beatAlignment{beat, downbeat: withinPct = share of cuts within +-2 frames of a beat, meanOffsetMs (positive = cuts land late), meanAbsOffsetMs, offsetsFrames}. ") + gridDoc +
                 QStringLiteral(" Optional startFrame / endFrame limit the range. Deterministic: measure before and after every pacing decision."),
             js::obj({{q("startFrame"), js::integer(QString(), 0)}, {q("endFrame"), js::integer(QString(), 1)}, {q("trackId"), js::str()}, {q("musicItemId"), js::str()},
                      {q("beatFrames"), js::array(js::integer())}, {q("downbeatFrames"), js::array(js::integer())}}),
             statsCmd));
  e.add(spec("edit.snapToBeats", "", "editor", true,
             QStringLiteral("Propose moving cuts onto the beat (`to`: beat | downbeat). The cut between two abutting shots moves by trimming the out edge of the left item and the in edge of the right one (item.trim, ripple=false), so the "
                            "timeline length does not change. `itemIds` snaps the cut at the head of each of these items; omitted: every cut on `trackId` (default: the main video track). A cut moves to the nearest target within `maxShiftFrames` (6). "
                            "Returns cuts[{frame, targetFrame, shiftFrames, method}] and skipped[{frame, reason, detail}]: too_far (no beat that near), exceeds_handles (the source has no media left for the trim), clips_word (the target lands "
                            "inside speech: the footage index's spans or an energy gate on the audio; suggestedFrame is the nearest gap; `allowWordClip` overrides), too_short (would leave a shot under 2 frames), not_abutting (gap or overlap at the cut). "
                            "before / predicted give the share of cuts within +-2 frames of the grid. apply=true writes every move as ONE undo step and re-measures: after, verified. ") + gridDoc,
             js::obj({{q("itemIds"), js::array(js::str())}, {q("trackId"), js::str()}, {q("to"), js::choice({q("beat"), q("downbeat")})}, {q("maxShiftFrames"), js::integer(QString(), 0, 60)},
                      {q("allowWordClip"), js::boolean()}, {q("musicItemId"), js::str()}, {q("beatFrames"), js::array(js::integer())}, {q("downbeatFrames"), js::array(js::integer())},
                      {q("apply"), js::boolean(QStringLiteral("Write the moves (undoable)"))}}),
             snapCmd));
  e.add(spec("edit.safeCutPoint", "", "editor", false,
             QStringLiteral("Where can a cut at `frame` (timeline frame) go without clipping a word? Speech spans of the item's audio come from the footage index (index.speech) when it knows the asset, else an energy gate on the rendered audio "
                            "(speechSource says which); gaps under gapMs (80) are one word run. Returns insideWord, the containing word (startFrame / endFrame), safeFrame (the frame itself in a gap, else the nearer word edge within `maxShiftFrames`, 12; "
                            "null when none is that near), shiftFrames and the two alternatives (before / after the word)."),
             js::obj({{q("itemId"), js::str()}, {q("frame"), js::integer(QStringLiteral("Timeline frame"), 0)}, {q("maxShiftFrames"), js::integer(QString(), 0, 300)}, {q("gapMs"), js::number(QString(), 0, 2000)}}, {q("itemId"), q("frame")}),
             safeCutCmd));
  e.add(spec("timeline.check", "", "editor", false,
             QStringLiteral("Verify the edit against a delivery spec. Returns {pass, violations[], limits, beat}; each violation is {rule, frame, timeSec, measured, limit, message}. Rules (on the main video track unless trackId): gap (nothing between shots, "
                            "or before the first shot), overlap (shots on one track overlap), flash_frame (the copilot lint rule: a shot under 3 frames or `minShotSec`, default 0.25), duration (the timeline is off `targetDurationSec` by more than `toleranceSec`, default 0.5), "
                            "aspect_canvas (the canvas is not `aspect`, e.g. \"9:16\"), aspect_mismatch (a clip's aspect differs from the canvas by more than 2 %), word_clip (a cut lands inside speech, see edit.safeCutPoint), off_beat (only when `maxOffBeatMs` is given "
                            "and a beat grid exists: a cut is further than that from the nearest beat). ") + gridDoc + QStringLiteral(" Run it before and after a change."),
             js::obj({{q("targetDurationSec"), js::number(QString(), 0)}, {q("toleranceSec"), js::number(QString(), 0, 600)}, {q("aspect"), js::str(QStringLiteral("\"16:9\", \"9:16\", \"1:1\" or a ratio like 1.78"))},
                      {q("maxOffBeatMs"), js::number(QString(), 0, 5000)}, {q("minShotSec"), js::number(QString(), 0, 30)}, {q("trackId"), js::str()}, {q("musicItemId"), js::str()},
                      {q("beatFrames"), js::array(js::integer())}, {q("downbeatFrames"), js::array(js::integer())}}),
             checkCmd));
}

} // namespace sf::engine
