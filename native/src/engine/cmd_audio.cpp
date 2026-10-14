// Measured audio and the deterministic proposals built on it: audio.measure, audio.normalize, audio.duck, music.structure, audio.check.
// Measure, then decide: every number comes from audio/audio_analysis (pure functions over the rendered mix), the mix is rendered
// offline by the same AudioEngine export uses, and every proposal is verified by measuring again.

#include "audio/audio_analysis.h"
#include "audio/engine.h"
#include "audio/loudness.h"
#include "audio/source.h"
#include "audio/timebase.h"
#include "engine/cmd_util.h"
#include "media/audio_decoder.h"

#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>

namespace sf::engine {

namespace {

constexpr double kRate = audio::kAnalysisRate;
constexpr double kInfD = std::numeric_limits<double>::infinity();

// ---------------------------------------------------------------- JSON helpers

QJsonValue fin(double v, double scale = 100.0) { return std::isfinite(v) ? QJsonValue(std::round(v * scale) / scale) : QJsonValue(QJsonValue::Null); }

double fps(const TimelineDoc& d) { return static_cast<double>(d.project.fps > 0 ? d.project.fps : 30); }

QJsonObject summary(const audio::MixMeasurement& m) {
  const audio::LoudnessResult& l = m.loudness;
  return QJsonObject{{q("integratedLufs"), fin(l.integrated)}, {q("loudnessRangeLu"), fin(l.range)}, {q("truePeakDbtp"), fin(l.truePeakDb)}, {q("samplePeakDb"), fin(l.samplePeakDb)},
                     {q("clippedSamples"), static_cast<double>(m.clippedSamples)}, {q("noiseFloorDb"), fin(m.noiseFloorDb)}, {q("silent"), !std::isfinite(l.integrated)}};
}

// ---------------------------------------------------------------- rendering

TimelineDoc isolate(const TimelineDoc& d, const std::function<bool(const Item&)>& keep) {
  TimelineDoc o = d;
  o.items.erase(std::remove_if(o.items.begin(), o.items.end(), [&](const Item& i) { return !keep(i); }), o.items.end());
  return o;
}

TimelineDoc onlyTracks(const TimelineDoc& d, const QStringList& tracks) {
  return isolate(d, [&](const Item& i) { return tracks.contains(i.trackId); });
}

std::shared_ptr<audio::FileProvider> makeSources(const CallContext& c) {
  auto sources = std::make_shared<audio::FileProvider>();
  std::map<QString, audio::FileProvider::File> files;
  for (const auto& [id, ref] : assetTable(*c.session)) files[id] = {ref.path, ref.kind != AssetKind::Image};
  sources->setFiles(std::move(files));
  return sources;
}

using Sink = std::function<void(const float*, std::int64_t)>;

// Offline mix of a document, the way export renders it (AudioEngine, default options), streamed in 1 s chunks.
class OfflineMix {
public:
  OfflineMix(const CallContext& c, const TimelineDoc& doc) : fps_(static_cast<std::int64_t>(fps(doc))), engine_(std::make_shared<const TimelineDoc>(doc), makeSources(c)) {}
  void render(Frame in, Frame out, const Sink& sink) {
    const std::int64_t base = audio::frameToSample(in, fps_), total = audio::frameToSample(out, fps_) - base;
    std::vector<float> buf(static_cast<size_t>(audio::kAnalysisRate) * 2);
    for (std::int64_t done = 0; done < total; done += audio::kAnalysisRate) {
      const std::int64_t n = std::min<std::int64_t>(audio::kAnalysisRate, total - done);
      engine_.render(base + done, n, buf.data());
      sink(buf.data(), n);
    }
  }

private:
  std::int64_t fps_;
  audio::AudioEngine engine_;
};

// ---------------------------------------------------------------- scopes

struct Scope {
  QString kind; // master | track | item | asset
  QString id;
  TimelineDoc doc;       // master / track / item: the document with only the scope's audio left in it
  Frame in = 0, out = 0; // timeline range that holds the scope's audio
  const Asset* asset = nullptr;
};

bool audible(const Item& i) { return i.type() == ItemType::Audio || i.type() == ItemType::Video; }

Result resolveScope(CallContext& c, const QString& id, Scope* s) {
  const editor::Project& p = *c.session->project;
  const TimelineDoc& d = p.doc();
  s->id = id;
  if (id.isEmpty() || id == QLatin1String("master")) {
    s->kind = q("master");
    s->id = q("master");
    s->doc = d;
    s->in = 0;
    s->out = docDurationFrames(d);
  } else if (const Track* t = getTrack(d, id)) {
    s->kind = q("track");
    s->doc = onlyTracks(d, {t->id});
    bool any = false;
    s->in = 0;
    s->out = 0;
    for (const Item& i : s->doc.items) {
      if (!audible(i)) continue;
      s->in = any ? std::min(s->in, i.startFrame) : i.startFrame;
      s->out = std::max(s->out, itemEnd(i));
      any = true;
    }
    if (!any) return invalid(QStringLiteral("Track %1 has no audio items.").arg(id));
  } else if (const Item* it = getItem(d, id)) {
    if (!audible(*it)) return invalid(QStringLiteral("Item %1 is %2: it has no audio.").arg(id, enumName(it->type())));
    s->kind = q("item");
    s->doc = isolate(d, [&](const Item& i) { return i.id == it->id; });
    s->in = it->startFrame;
    s->out = itemEnd(*it);
  } else if (const Asset* a = p.asset(id)) {
    if (a->kind == AssetKind::Image) return invalid(QStringLiteral("Asset %1 is an image: no audio.").arg(id));
    s->kind = q("asset");
    s->asset = a;
  } else {
    return notFound(QStringLiteral("No master, track, item or asset \"%1\".").arg(id), QStringLiteral("Use \"master\", or an id from timeline.get / assets.list."));
  }
  if (s->kind != QLatin1String("asset") && s->out <= s->in) return invalid(QStringLiteral("Nothing to measure: the scope is empty."));
  return Result::success();
}

Result measureScope(CallContext& c, const Scope& s, Frame in, Frame out, audio::MixMeasurement* m) {
  audio::MixAnalyzer an;
  if (s.asset) {
    QString err;
    auto dec = AudioDecoder::open(s.asset->path, &err);
    if (!dec) return Result::failure(q("unavailable"), err.isEmpty() ? QStringLiteral("Can't decode %1.").arg(s.asset->path) : err);
    const qint64 total = dec->info().totalSamples;
    std::vector<float> buf(static_cast<size_t>(audio::kAnalysisRate) * 2);
    for (qint64 at = 0; at < total; at += audio::kAnalysisRate) {
      const qint64 n = std::min<qint64>(audio::kAnalysisRate, total - at);
      dec->read(at, n, buf.data());
      an.process(buf.data(), n);
    }
  } else {
    OfflineMix mix(c, s.doc);
    mix.render(in, out, [&](const float* p, std::int64_t n) { an.process(p, n); });
  }
  *m = an.result();
  return Result::success();
}

Result measureScope(CallContext& c, const Scope& s, audio::MixMeasurement* m) { return measureScope(c, s, s.in, s.out, m); }

// ---------------------------------------------------------------- audio.measure

QJsonObject measurementJson(const Scope& s, Frame in, Frame out, const audio::MixMeasurement& m, double docFps) {
  QJsonObject o = summary(m);
  o.insert(q("scope"), QJsonObject{{q("kind"), s.kind}, {q("id"), s.id}});
  if (!s.asset) {
    o.insert(q("startFrame"), static_cast<double>(in));
    o.insert(q("endFrame"), static_cast<double>(out));
  }
  o.insert(q("seconds"), fin(m.seconds(), 1000.0));
  QJsonArray runs;
  for (const audio::ClipRun& r : m.clipRuns) {
    const double t = static_cast<double>(r.startSample) / kRate;
    QJsonObject ro{{q("timeSec"), fin(t + (s.asset ? 0.0 : static_cast<double>(in) / docFps), 1000.0)}, {q("durationMs"), fin(static_cast<double>(r.endSample - r.startSample + 1) / kRate * 1000.0, 10.0)},
                   {q("samples"), static_cast<double>(r.samples)}};
    if (!s.asset) ro.insert(q("frame"), static_cast<double>(in + static_cast<Frame>(std::floor(t * docFps))));
    runs.append(ro);
  }
  o.insert(q("clipRuns"), runs);
  QJsonArray st;
  for (double v : m.shortTermLufs) st.append(fin(v));
  o.insert(q("shortTermLufs"), QJsonObject{{q("stepSec"), 1}, {q("values"), st}});
  return o;
}

Result measureCmd(CallContext& c, const QJsonObject& a) {
  Scope s;
  if (Result r = resolveScope(c, a.value(q("scope")).toString(q("master")), &s); !r.ok) return r;
  Frame in = s.in, out = s.out;
  if (!s.asset) {
    if (a.contains(q("startFrame"))) in = a.value(q("startFrame")).toInteger();
    if (a.contains(q("endFrame"))) out = a.value(q("endFrame")).toInteger();
    if (out <= in || in < 0) return invalid(QStringLiteral("The range is empty (startFrame %1, endFrame %2).").arg(in).arg(out));
  }
  audio::MixMeasurement m;
  if (Result r = measureScope(c, s, in, out, &m); !r.ok) return r;
  return Result::success(measurementJson(s, in, out, m, fps(c.session->project->doc())));
}

// ---------------------------------------------------------------- audio.normalize

MixNode& stripFor(Mixer& m, const QString& trackId) {
  for (MixNode& n : m.strips)
    if (n.id == trackId) return n;
  m.strips.push_back(MixNode{.id = trackId});
  return m.strips.back();
}

void scaleNode(MixNode& n, double g) {
  n.volume *= g;
  const auto it = n.automation.find(q("volume"));
  if (it != n.automation.end())
    for (Keyframe& k : it->second) k.value *= g;
}

Result normalizeCmd(CallContext& c, const QJsonObject& a) {
  const double target = a.value(q("target")).toDouble(-14.0), ceiling = a.value(q("truePeakCeiling")).toDouble(-1.0);
  Scope s;
  if (Result r = resolveScope(c, a.value(q("scope")).toString(q("master")), &s); !r.ok) return r;
  if (s.asset) return invalid(QStringLiteral("Asset %1 is not part of the mix: normalize the item or track that plays it.").arg(s.id));
  audio::MixMeasurement before;
  if (Result r = measureScope(c, s, &before); !r.ok) return r;
  const audio::LoudnessResult& l = before.loudness;
  const double gain = audio::normalizeGainDb(l, target, ceiling);
  const bool limited = std::isfinite(l.integrated) && gain < target - l.integrated - 1e-9;
  QJsonObject o{{q("scope"), QJsonObject{{q("kind"), s.kind}, {q("id"), s.id}}}, {q("target"), target}, {q("truePeakCeiling"), ceiling}, {q("before"), summary(before)}, {q("applied"), false}};
  QJsonObject prop{{q("gainDb"), fin(gain)}, {q("limitedByTruePeak"), limited}, {q("predictedLufs"), fin(l.integrated + gain)}, {q("predictedTruePeakDbtp"), fin(l.truePeakDb + gain)},
                   {q("writes"), s.kind == QLatin1String("item") ? q("item volume") : q("mixer fader (%1)").arg(s.kind == QLatin1String("master") ? q("master") : q("track strip"))}};
  QJsonArray notes;
  if (!std::isfinite(l.integrated)) notes.append(q("The scope is silent: nothing to normalize."));
  if (limited) notes.append(QStringLiteral("The true-peak ceiling (%1 dBTP) holds the gain below what reaches %2 LUFS; the loudness lands %3 LU under the target.").arg(ceiling).arg(target).arg(target - l.integrated - gain, 0, 'f', 1));
  o.insert(q("proposal"), prop);
  if (!a.value(q("apply")).toBool()) {
    o.insert(q("notes"), notes);
    return Result::success(o);
  }
  if (std::fabs(gain) < 0.05) {
    notes.append(q("Already within 0.05 dB of the best gain: nothing written."));
    o.insert(q("notes"), notes);
    o.insert(q("after"), summary(before));
    return Result::success(o);
  }

  const TimelineDoc& d = c.session->project->doc();
  const double g = std::pow(10.0, gain / 20.0);
  Result w;
  if (s.kind == QLatin1String("item")) {
    const Item* it = getItem(d, s.id);
    double vol = it->volume * g;
    if (vol > 1.0 && g > 1.0) {
      vol = std::max(it->volume, 1.0); // the mixer caps item gain at unity: boosting needs the track fader
      notes.append(q("Item gain is capped at unity by the mixer, so the boost could not be written to the item: normalize its track instead."));
    }
    QJsonObject patch{{q("volume"), vol}};
    QJsonArray ops{QJsonObject{{q("type"), q("item.update")}, {q("itemId"), s.id}, {q("patch"), patch}}};
    const auto kf = it->keyframes.find(q("volume"));
    if (kf != it->keyframes.end() && !kf->second.empty()) {
      QJsonArray ks;
      for (Keyframe k : kf->second) {
        k.value = g > 1.0 ? std::min(k.value * g, std::max(k.value, 1.0)) : k.value * g;
        ks.append(toJson(k));
      }
      ops.append(QJsonObject{{q("type"), q("item.setKeyframes")}, {q("itemId"), s.id}, {q("property"), q("volume")}, {q("keyframes"), ks}});
    }
    w = applyToSession(c, parseOps(ops), q("audio.normalize"), {{q("scope"), s.id}});
  } else {
    Mixer m = d.mixer.value_or(Mixer{});
    scaleNode(s.kind == QLatin1String("master") ? m.master : stripFor(m, s.id), g);
    w = applyToSession(c, {Op(MixerSet{m})}, q("audio.normalize"), {{q("scope"), s.id}});
  }
  if (!w.ok) return w;
  o.insert(q("applied"), true);
  o.insert(q("undoLabel"), w.value.toObject().value(q("undoLabel")));
  Scope s2;
  audio::MixMeasurement after;
  if (Result r = resolveScope(c, s.id, &s2); !r.ok) return r;
  if (Result r = measureScope(c, s2, &after); !r.ok) return r;
  o.insert(q("after"), summary(after));
  const double achieved = after.loudness.integrated - l.integrated;
  o.insert(q("achievedGainDb"), fin(achieved));
  const bool verified = std::isfinite(after.loudness.integrated) && std::fabs(achieved - gain) <= 0.3;
  o.insert(q("verified"), verified);
  if (!verified) notes.append(q("The re-measured loudness differs from the prediction by more than 0.3 dB (nonlinear processing in the chain, or a gain cap)."));
  o.insert(q("notes"), notes);
  return Result::success(o);
}

// ---------------------------------------------------------------- track roles and speech ranges

struct Roles {
  QStringList speech, music;
};

bool hasAudioItems(const TimelineDoc& d, const Track& t) {
  for (const Item& i : d.items)
    if (i.trackId == t.id && audible(i) && !i.muted) return true;
  return false;
}

Roles defaultRoles(const TimelineDoc& d) {
  static const QRegularExpression speechRx(q("dialog|voice|speech|narrat|interview|\\bvo\\b|\\bhost\\b"), QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression musicRx(q("music|score|song|soundtrack|\\bbgm\\b|\\bbed\\b"), QRegularExpression::CaseInsensitiveOption);
  Roles r;
  for (const Track& t : d.tracks)
    if (hasAudioItems(d, t) && speechRx.match(t.name).hasMatch()) r.speech << t.id;
  for (const Track& t : d.tracks)
    if (hasAudioItems(d, t) && musicRx.match(t.name).hasMatch() && !r.speech.contains(t.id)) r.music << t.id;
  if (r.speech.isEmpty())
    for (const Track& t : d.tracks)
      if (t.kind == TrackKind::Video && hasAudioItems(d, t) && !r.music.contains(t.id)) r.speech << t.id;
  if (r.music.isEmpty())
    for (const Track& t : d.tracks)
      if (t.kind == TrackKind::Audio && hasAudioItems(d, t) && !r.speech.contains(t.id)) r.music << t.id;
  return r;
}

QStringList listArg(const QJsonObject& a, const char* single, const char* plural) {
  QStringList l;
  if (a.contains(q(single))) l << a.value(q(single)).toString();
  for (const QJsonValue& v : a.value(q(plural)).toArray()) l << v.toString();
  l.removeAll(QString());
  l.removeDuplicates();
  return l;
}

Result needTracks(const TimelineDoc& d, const QStringList& ids) {
  for (const QString& id : ids)
    if (!getTrack(d, id)) return notFound(QStringLiteral("Track %1 not found.").arg(id), QStringLiteral("Call timeline.get."));
  return Result::success();
}

struct SpeechRanges {
  std::vector<audio::Span> frames; // merged, timeline frames
  QString source;                  // explicit | index | energyGate | index+energyGate
};

// Speech ranges on the timeline: given explicitly, from the footage index's speech spans of the items on the dialogue tracks
// (when the index commands are registered and know the asset), else from an energy gate on the rendered dialogue.
Result speechRanges(CallContext& c, const QStringList& tracks, const QJsonArray& explicitRanges, double gapMs, SpeechRanges* out) {
  const TimelineDoc& d = c.session->project->doc();
  const double f = fps(d);
  std::vector<audio::Span> spans;
  bool idx = false, gate = false;
  if (!explicitRanges.isEmpty()) {
    for (const QJsonValue& v : explicitRanges) {
      const QJsonObject r = v.toObject();
      spans.push_back({r.value(q("startFrame")).toInteger(), r.value(q("endFrame")).toInteger()});
    }
    out->source = q("explicit");
  } else {
    QStringList noSpans; // item ids without index spans
    const bool haveIndex = c.engine->find(q("index.speech")) != nullptr;
    for (const Item& it : d.items) {
      if (!tracks.contains(it.trackId) || !audible(it) || it.muted || !it.assetId) continue;
      bool found = false;
      if (haveIndex) {
        const Result r = c.engine->call(q("index.speech"), {{q("assetId"), *it.assetId}, {q("session"), c.session->id}});
        const double srcIn = static_cast<double>(it.sourceInFrame.value_or(0)) / f, speed = it.speed > 0 ? it.speed : 1.0;
        if (r.ok) {
          for (const QJsonValue& v : r.value.toArray()) {
            const QJsonObject sp = v.toObject();
            if (!sp.value(q("isSpeech")).toBool(true)) continue;
            const double t0 = static_cast<double>(it.startFrame) / f + (sp.value(q("startSec")).toDouble() - srcIn) / speed;
            const double t1 = static_cast<double>(it.startFrame) / f + (sp.value(q("endSec")).toDouble() - srcIn) / speed;
            const Frame a0 = std::max(it.startFrame, static_cast<Frame>(std::llround(t0 * f))), a1 = std::min(itemEnd(it), static_cast<Frame>(std::llround(t1 * f)));
            found = true; // the asset is indexed, even if this part of it holds no speech
            if (a1 > a0) spans.push_back({a0, a1});
          }
          found = found || !r.value.toArray().isEmpty();
        }
      }
      if (found) idx = true;
      else noSpans << it.id;
    }
    if (!noSpans.isEmpty()) {
      const TimelineDoc gdoc = isolate(d, [&](const Item& i) { return noSpans.contains(i.id); });
      Frame in = gdoc.items.front().startFrame, end = itemEnd(gdoc.items.front());
      for (const Item& i : gdoc.items) {
        in = std::min(in, i.startFrame);
        end = std::max(end, itemEnd(i));
      }
      OfflineMix mix(c, gdoc);
      audio::EnergyGate gateMeter;
      mix.render(in, end, [&](const float* p, std::int64_t n) { gateMeter.process(p, n); });
      for (const audio::Span& s : gateMeter.ranges())
        spans.push_back({in + static_cast<Frame>(std::llround(static_cast<double>(s.start) / kRate * f)), in + static_cast<Frame>(std::llround(static_cast<double>(s.end) / kRate * f))});
      gate = true;
    }
    out->source = idx && gate ? q("index+energyGate") : idx ? q("index") : q("energyGate");
  }
  out->frames = audio::mergeSpans(spans, static_cast<std::int64_t>(std::llround(gapMs * f / 1000.0)));
  return Result::success();
}

QJsonArray spansJson(const std::vector<audio::Span>& v, double f) {
  QJsonArray a;
  for (const audio::Span& s : v)
    a.append(QJsonObject{{q("startFrame"), static_cast<double>(s.start)}, {q("endFrame"), static_cast<double>(s.end)}, {q("startSec"), fin(static_cast<double>(s.start) / f, 1000.0)}, {q("endSec"), fin(static_cast<double>(s.end) / f, 1000.0)}});
  return a;
}

// ---------------------------------------------------------------- speech over music

struct Margin {
  audio::Span range;
  double speechDb = -kInfD, musicDb = -kInfD, marginDb = -kInfD;
};
struct Margins {
  std::vector<Margin> perRange;
  double speechDb = -kInfD, musicDb = -kInfD, marginDb = -kInfD; // pooled over all ranges
};

// RMS level of the dialogue and of the music over each speech range (and pooled), both rendered through the mixer.
Margins measureMargins(CallContext& c, const TimelineDoc& speechDoc, const TimelineDoc& musicDoc, const std::vector<audio::Span>& ranges) {
  Margins m;
  OfflineMix sp(c, speechDoc), mu(c, musicDoc);
  audio::EnergyMeter ps, pm;
  for (const audio::Span& r : ranges) {
    audio::EnergyMeter es, em;
    sp.render(r.start, r.end, [&](const float* p, std::int64_t n) { es.process(p, n); ps.process(p, n); });
    mu.render(r.start, r.end, [&](const float* p, std::int64_t n) { em.process(p, n); pm.process(p, n); });
    m.perRange.push_back({r, es.db(), em.db(), audio::marginDb(es.db(), em.db())});
  }
  m.speechDb = ps.db();
  m.musicDb = pm.db();
  m.marginDb = audio::marginDb(m.speechDb, m.musicDb);
  return m;
}

QJsonObject marginJson(const Margins& m) {
  return QJsonObject{{q("speechDb"), fin(m.speechDb)}, {q("musicDb"), fin(m.musicDb)}, {q("marginDb"), std::isfinite(m.marginDb) ? fin(m.marginDb) : (m.marginDb > 0 ? QJsonValue(q("music silent")) : QJsonValue(QJsonValue::Null))}};
}

// ---------------------------------------------------------------- audio.duck

Result duckCmd(CallContext& c, const QJsonObject& a) {
  const TimelineDoc& d = c.session->project->doc();
  const double f = fps(d);
  QStringList speech = listArg(a, "speechTrack", "speechTracks");
  QString music = a.value(q("musicTrack")).toString();
  const Roles roles = defaultRoles(d);
  if (music.isEmpty() && !roles.music.isEmpty()) music = roles.music.front();
  if (music.isEmpty()) return invalid(QStringLiteral("Pass musicTrack (no music track could be guessed from the track names and kinds)."));
  const QJsonArray explicitRanges = a.value(q("ranges")).toArray();
  if (speech.isEmpty() && explicitRanges.isEmpty()) speech = roles.speech;
  if (speech.isEmpty() && explicitRanges.isEmpty()) return invalid(QStringLiteral("Pass speechTrack(s) or ranges (no dialogue track could be guessed)."));
  if (Result r = needTracks(d, speech + QStringList{music}); !r.ok) return r;
  if (speech.contains(music)) return invalid(QStringLiteral("musicTrack %1 is also a speech track.").arg(music));
  const double attack = a.value(q("attackMs")).toDouble(150.0), release = a.value(q("releaseMs")).toDouble(500.0), targetMargin = a.value(q("targetMarginDb")).toDouble(9.0);

  SpeechRanges sr;
  if (Result r = speechRanges(c, speech, explicitRanges, a.value(q("gapMs")).toDouble(300.0), &sr); !r.ok) return r;
  QJsonArray notes;
  QJsonObject o{{q("musicTrack"), music}, {q("speechTracks"), QJsonArray::fromStringList(speech)}, {q("rangeSource"), sr.source}, {q("ranges"), spansJson(sr.frames, f)},
                {q("attackMs"), attack}, {q("releaseMs"), release}, {q("targetMarginDb"), targetMargin}, {q("applied"), false}};
  if (sr.frames.empty()) {
    notes.append(q("No speech found: nothing to duck under."));
    o.insert(q("keyframes"), QJsonArray());
    o.insert(q("notes"), notes);
    return Result::success(o);
  }

  const TimelineDoc speechDoc = onlyTracks(d, speech.isEmpty() ? roles.speech : speech), musicDoc = onlyTracks(d, {music});
  const Margins before = measureMargins(c, speechDoc, musicDoc, sr.frames);
  o.insert(q("before"), marginJson(before));

  const bool depthGiven = a.contains(q("depthDb"));
  double depth = a.value(q("depthDb")).toDouble();
  if (!depthGiven) depth = std::isfinite(before.marginDb) ? std::clamp(targetMargin - before.marginDb, 0.0, 40.0) : 0.0;
  o.insert(q("depthDb"), fin(depth));
  o.insert(q("depthComputed"), !depthGiven);

  Mixer mixer = d.mixer.value_or(Mixer{});
  MixNode& strip = stripFor(mixer, music);
  const bool hadAutomation = strip.automation.count(q("volume")) && !strip.automation[q("volume")].empty();
  if (hadAutomation && !a.value(q("replaceExisting")).toBool())
    return invalid(QStringLiteral("Track %1 already has volume automation. Pass replaceExisting=true to overwrite it.").arg(music));
  audio::DuckParams dp;
  dp.depthDb = depth;
  dp.attackMs = attack;
  dp.releaseMs = release;
  dp.baseGain = strip.volume;
  const std::vector<audio::DuckKey> keys = audio::duckKeyframes(sr.frames, dp, f);
  QJsonArray kj;
  KeyframeList list;
  for (const audio::DuckKey& k : keys) {
    list.push_back({k.frame, k.value, Easing::Linear});
    kj.append(QJsonObject{{q("frame"), static_cast<double>(k.frame)}, {q("value"), std::round(k.value * 10000.0) / 10000.0}, {q("easing"), q("linear")}});
  }
  o.insert(q("keyframes"), kj);
  if (keys.empty()) {
    notes.append(depth <= 0 ? QStringLiteral("Speech already sits %1 dB over the music (target %2): no ducking needed.").arg(before.marginDb, 0, 'f', 1).arg(targetMargin, 0, 'f', 1)
                            : QStringLiteral("No keyframes produced."));
    o.insert(q("after"), marginJson(before));
    o.insert(q("notes"), notes);
    return Result::success(o);
  }
  if (hadAutomation) notes.append(q("Replaced the existing volume automation of the music track."));

  strip.automation[q("volume")] = list;
  TimelineDoc proposed = musicDoc;
  proposed.mixer = mixer;
  const Margins after = measureMargins(c, speechDoc, proposed, sr.frames);
  o.insert(q("after"), marginJson(after));
  if (std::isfinite(after.marginDb) && after.marginDb < targetMargin - 1.5 && !depthGiven) notes.append(q("The re-measured margin is under the target (ramps and music in the gaps): raise depthDb."));

  if (a.value(q("apply")).toBool()) {
    Result w = applyToSession(c, {Op(MixerSet{mixer})}, q("audio.duck"), {{q("musicTrack"), music}});
    if (!w.ok) return w;
    o.insert(q("applied"), true);
    o.insert(q("undoLabel"), w.value.toObject().value(q("undoLabel")));
  }
  o.insert(q("notes"), notes);
  return Result::success(o);
}

// ---------------------------------------------------------------- music.structure

Result structureCmd(CallContext& c, const QJsonObject& a) {
  const editor::Project& p = *c.session->project;
  const TimelineDoc& d = p.doc();
  const double f = fps(d);
  const Item* item = nullptr;
  const Asset* asset = nullptr;
  if (a.contains(q("itemId"))) {
    item = getItem(d, a.value(q("itemId")).toString());
    if (!item) return notFound(QStringLiteral("Item %1 not found.").arg(a.value(q("itemId")).toString()), QStringLiteral("Call timeline.get."));
    if (!audible(*item) || !item->assetId) return invalid(QStringLiteral("Item %1 has no audio asset.").arg(item->id));
    asset = p.asset(*item->assetId);
  } else if (a.contains(q("assetId"))) {
    asset = p.asset(a.value(q("assetId")).toString());
  } else {
    return invalid(QStringLiteral("Pass assetId or itemId."));
  }
  if (!asset) return notFound(QStringLiteral("Asset not found."), QStringLiteral("Call assets.list."));
  if (asset->kind == AssetKind::Image) return invalid(QStringLiteral("Asset %1 is an image: no audio.").arg(asset->id));
  QString err;
  auto dec = AudioDecoder::open(asset->path, &err);
  if (!dec) return Result::failure(q("unavailable"), err.isEmpty() ? QStringLiteral("Can't decode %1.").arg(asset->path) : err);
  qint64 from = 0, to = dec->info().totalSamples;
  if (item) {
    from = static_cast<qint64>(std::llround(static_cast<double>(item->sourceInFrame.value_or(0)) / f * kRate));
    to = std::min(to, static_cast<qint64>(std::llround(static_cast<double>(sourceOutFrame(*item)) / f * kRate)));
  }
  to = std::min<qint64>(to, from + static_cast<qint64>(kRate) * 600);
  if (to <= from) return invalid(QStringLiteral("No audio to analyse."));
  std::vector<float> buf(static_cast<size_t>(to - from) * 2);
  dec->read(from, to - from, buf.data());
  const audio::MusicStructure ms = audio::analyseMusic(buf.data(), to - from);

  QJsonObject o{{q("assetId"), asset->id}, {q("valid"), ms.valid}, {q("note"), QString::fromStdString(ms.note)}, {q("durationSec"), fin(ms.durationSec, 1000.0)}};
  if (item) {
    o.insert(q("itemId"), item->id);
    o.insert(q("analysedFrom"), q("the item's source range; times are seconds from its in point"));
  }
  if (ms.bpm > 0) {
    o.insert(q("bpm"), fin(ms.bpm));
    o.insert(q("tempoConfidence"), fin(ms.tempoConfidence));
    o.insert(q("beatPeriodSec"), fin(ms.beatPeriodSec, 10000.0));
    o.insert(q("timeSignature"), q("4/4 (assumed)"));
    o.insert(q("downbeatPhase"), ms.downbeatPhase);
    o.insert(q("downbeatEvidence"), QString::fromStdString(ms.downbeatEvidence));
  }
  const double speed = item && item->speed > 0 ? item->speed : 1.0;
  const auto timeArray = [&](const std::vector<double>& v, QJsonArray* frames) {
    QJsonArray t;
    for (double s : v) {
      t.append(fin(s, 1000.0));
      if (item && frames) frames->append(static_cast<double>(item->startFrame + static_cast<Frame>(std::llround(s / speed * f))));
    }
    return t;
  };
  QJsonArray beatFrames, downFrames;
  o.insert(q("beats"), timeArray(ms.beats, &beatFrames));
  o.insert(q("downbeats"), timeArray(ms.downbeats, &downFrames));
  if (item) {
    o.insert(q("beatFrames"), beatFrames);
    o.insert(q("downbeatFrames"), downFrames);
  }
  o.insert(q("bars"), static_cast<int>(ms.downbeats.size()));
  QJsonArray secs;
  for (const audio::MusicSection& s : ms.sections)
    secs.append(QJsonObject{{q("startSec"), fin(s.startSec, 1000.0)}, {q("endSec"), fin(s.endSec, 1000.0)}, {q("startBar"), s.startBar}, {q("endBar"), s.endBar}, {q("label"), QString::fromStdString(s.label)},
                            {q("energyDb"), fin(s.energyDb)}, {q("relEnergy"), fin(s.relEnergy)}});
  o.insert(q("sections"), secs);
  return Result::success(o);
}

// ---------------------------------------------------------------- audio.check

QJsonObject violation(const char* rule, const QJsonValue& frame, const QJsonValue& timeSec, const QJsonValue& measured, const QJsonValue& limit, const QString& message) {
  return QJsonObject{{q("rule"), q(rule)}, {q("frame"), frame}, {q("timeSec"), timeSec}, {q("measured"), measured}, {q("limit"), limit}, {q("message"), message}};
}

Result checkCmd(CallContext& c, const QJsonObject& a) {
  const TimelineDoc& d = c.session->project->doc();
  const double f = fps(d);
  const double target = a.value(q("target")).toDouble(-14.0), tol = a.value(q("tolerance")).toDouble(1.0), ceiling = a.value(q("truePeakCeiling")).toDouble(-1.0),
               minMargin = a.value(q("minSpeechMarginDb")).toDouble(6.0);
  Scope master;
  if (Result r = resolveScope(c, q("master"), &master); !r.ok) return r;
  audio::MixMeasurement m;
  if (Result r = measureScope(c, master, &m); !r.ok) return r;
  QJsonArray v;
  const audio::LoudnessResult& l = m.loudness;
  if (!std::isfinite(l.integrated)) {
    v.append(violation("loudness", QJsonValue::Null, QJsonValue::Null, QJsonValue::Null, target, q("The mix is silent.")));
  } else if (std::fabs(l.integrated - target) > tol) {
    v.append(violation("loudness", QJsonValue::Null, QJsonValue::Null, fin(l.integrated), target,
                       QStringLiteral("Integrated loudness is %1 LUFS, %2 LU %3 the %4 LUFS target (tolerance %5 LU). audio.normalize proposes the fix.")
                           .arg(l.integrated, 0, 'f', 1).arg(std::fabs(l.integrated - target), 0, 'f', 1).arg(l.integrated > target ? q("above") : q("below")).arg(target).arg(tol)));
  }
  if (std::isfinite(l.truePeakDb) && l.truePeakDb > ceiling + 1e-9)
    v.append(violation("true_peak", QJsonValue::Null, QJsonValue::Null, fin(l.truePeakDb), ceiling,
                       QStringLiteral("True peak is %1 dBTP, over the %2 dBTP ceiling.").arg(l.truePeakDb, 0, 'f', 1).arg(ceiling)));
  if (m.clippedSamples > 0) {
    const audio::ClipRun& r0 = m.clipRuns.front();
    const double t = static_cast<double>(r0.startSample) / kRate;
    v.append(violation("clipping", static_cast<double>(static_cast<Frame>(std::floor(t * f))), fin(t, 1000.0), static_cast<double>(m.clippedSamples), 0,
                       QStringLiteral("%1 clipped samples in %2 run(s), the first at %3 s.").arg(m.clippedSamples).arg(m.clipRuns.size() >= audio::kMaxClipRuns ? q("64+") : QString::number(m.clipRuns.size())).arg(t, 0, 'f', 2)));
  }
  // abrupt end: the lint rule audio_abrupt_end (copilot/lint_engine.cpp): an audible audio item that ends at volume without any fade out
  for (const Item& it : d.items) {
    if (it.type() != ItemType::Audio || it.muted || it.volume < 0.5) continue;
    if (std::get<AudioProps>(it.props).fadeOutFrames >= 1) continue;
    const Frame end = itemEnd(it);
    QJsonObject ab = violation("abrupt_end", static_cast<double>(end), fin(static_cast<double>(end) / f, 1000.0), 0, 1,
                               QStringLiteral("Audio clip %1 ends at full volume without a fade-out (item.audio.set fadeOutFrames).").arg(it.id));
    ab.insert(q("itemId"), it.id);
    v.append(ab);
  }

  // speech over music, per speech range
  QStringList speech = listArg(a, "speechTrack", "speechTracks");
  QString music = a.value(q("musicTrack")).toString();
  const Roles roles = defaultRoles(d);
  if (speech.isEmpty()) speech = roles.speech;
  if (music.isEmpty() && !roles.music.isEmpty()) music = roles.music.front();
  QJsonObject marginInfo{{q("checked"), false}, {q("minSpeechMarginDb"), minMargin}};
  if (const Result r = needTracks(d, speech + (music.isEmpty() ? QStringList{} : QStringList{music})); !r.ok) return r;
  if (speech.isEmpty() || music.isEmpty() || speech.contains(music)) {
    marginInfo.insert(q("reason"), q("no separate dialogue and music tracks (pass speechTracks and musicTrack)"));
  } else {
    SpeechRanges sr;
    if (Result r = speechRanges(c, speech, a.value(q("ranges")).toArray(), a.value(q("gapMs")).toDouble(300.0), &sr); !r.ok) return r;
    const Margins mg = measureMargins(c, onlyTracks(d, speech), onlyTracks(d, {music}), sr.frames);
    marginInfo.insert(q("checked"), true);
    marginInfo.insert(q("speechTracks"), QJsonArray::fromStringList(speech));
    marginInfo.insert(q("musicTrack"), music);
    marginInfo.insert(q("rangeSource"), sr.source);
    marginInfo.insert(q("pooled"), marginJson(mg));
    int low = 0;
    for (const Margin& r : mg.perRange) {
      if (!std::isfinite(r.marginDb) || r.marginDb >= minMargin) continue;
      if (++low <= 50)
        v.append(violation("speech_margin", static_cast<double>(r.range.start), fin(static_cast<double>(r.range.start) / f, 1000.0), fin(r.marginDb), minMargin,
                           QStringLiteral("Speech at %1 s sits only %2 dB over the music (minimum %3 dB). audio.duck proposes the fix.").arg(static_cast<double>(r.range.start) / f, 0, 'f', 2).arg(r.marginDb, 0, 'f', 1).arg(minMargin)));
    }
    marginInfo.insert(q("rangesBelowMinimum"), low);
    marginInfo.insert(q("rangesChecked"), static_cast<int>(mg.perRange.size()));
  }
  return Result::success(QJsonObject{{q("pass"), v.isEmpty()}, {q("violations"), v}, {q("measured"), summary(m)}, {q("speechMargin"), marginInfo},
                                     {q("limits"), QJsonObject{{q("target"), target}, {q("tolerance"), tol}, {q("truePeakCeiling"), ceiling}, {q("minSpeechMarginDb"), minMargin}}}});
}

} // namespace

void registerAudioCommands(Engine& e) {
  const QString scopeDoc = QStringLiteral("\"master\" (the whole mix, default), a track id, an item id or (audio.measure only) an asset id.");
  e.add(spec("audio.measure", "", "audio", false,
             QStringLiteral("Measure audio per ITU-R BS.1770 / EBU R128 on the rendered timeline mix (the same offline mixer path export uses: mixer, effects, fades, automation). `scope`: ") + scopeDoc +
                 QStringLiteral(" A track or item is measured alone through the mixer. Returns integratedLufs, loudnessRangeLu, truePeakDbtp (4x oversampled), samplePeakDb, clippedSamples (values at or above 0.9999) "
                                "with clipRuns[{frame, timeSec, durationMs, samples}], noiseFloorDb (10th percentile of 50 ms RMS blocks, dBFS, -100 = digital silence), shortTermLufs{stepSec: 1, values} "
                                "(3 s windows, null until 3 s have played) and silent. null means silence. Deterministic: measure before and after every audio decision. Optional startFrame / endFrame limit the range (not for assets)."),
             js::obj({{q("scope"), js::str(scopeDoc)}, {q("startFrame"), js::integer(QString(), 0)}, {q("endFrame"), js::integer(QString(), 0)}}), measureCmd));
  e.add(spec("audio.normalize", "", "audio", true,
             QStringLiteral("Propose the static gain that brings `scope` (master | track id | item id) to `target` LUFS (default -14: streaming; -16 podcast, -23 EBU R128) without the true peak passing `truePeakCeiling` dBTP "
                            "(default -1): the gain is reduced if needed (proposal.limitedByTruePeak). Returns before, proposal{gainDb, predictedLufs, predictedTruePeakDbtp, writes}. With apply=true it writes the gain as ONE undo step "
                            "(master or track: the mixer fader and its volume automation; item: item volume and volume keyframes, capped at unity by the mixer) and measures again: after, achievedGainDb, verified "
                            "(achieved within 0.3 dB of the prediction). Not written when the gain is under 0.05 dB."),
             js::obj({{q("target"), js::number(QStringLiteral("LUFS, default -14"), -70, 0)}, {q("truePeakCeiling"), js::number(QStringLiteral("dBTP, default -1"), -20, 3)},
                      {q("scope"), js::str(QStringLiteral("master (default), a track id or an item id"))}, {q("apply"), js::boolean(QStringLiteral("Write the gain (undoable)"))}}),
             normalizeCmd));
  e.add(spec("audio.duck", "", "audio", true,
             QStringLiteral("Duck the music under speech. Speech ranges come from `ranges` ([{startFrame, endFrame}]) or from the dialogue track(s) (`speechTrack` / `speechTracks`; guessed from track names and kinds when omitted): the footage index's "
                            "speech spans of the items on them when the index knows the asset, else an energy gate on the rendered dialogue (rangeSource says which). Gaps under gapMs (300) are merged. `depthDb` defaults to what makes speech sit "
                            "targetMarginDb (9) over the music, measured as the RMS level of dialogue vs music over the speech ranges. The keyframes are absolute volume automation for the music track's mixer strip (linear ramps: down over attackMs "
                            "(150) ending when speech starts, up over releaseMs (500) after it ends). Returns ranges, keyframes, and the measured speech-over-music margin before and after (the proposal is rendered and measured, so `after` is real). "
                            "apply=true writes it as ONE undo step; existing volume automation on that strip needs replaceExisting=true."),
             js::obj({{q("speechTrack"), js::str()}, {q("speechTracks"), js::array(js::str())}, {q("musicTrack"), js::str()},
                      {q("ranges"), js::array(js::obj({{q("startFrame"), js::integer(QString(), 0)}, {q("endFrame"), js::integer(QString(), 0)}}, {q("startFrame"), q("endFrame")}))},
                      {q("depthDb"), js::number(QStringLiteral("dB the music dips; default computed"), 0, 60)}, {q("attackMs"), js::number(QString(), 0, 5000)}, {q("releaseMs"), js::number(QString(), 0, 10000)},
                      {q("targetMarginDb"), js::number(QStringLiteral("Wanted speech-over-music margin when depthDb is computed, default 9"), 0, 40)}, {q("gapMs"), js::number(QStringLiteral("Merge speech gaps shorter than this, default 300"), 0, 5000)},
                      {q("replaceExisting"), js::boolean()}, {q("apply"), js::boolean(QStringLiteral("Write the keyframes (undoable)"))}}),
             duckCmd));
  e.add(spec("music.structure", "", "audio", false,
             QStringLiteral("Tempo, beats and sections of a piece of music (`assetId`, or `itemId` for the part of it that item plays; first 10 minutes). Deterministic DSP, no model: spectral-flux onset envelope, autocorrelation for the tempo "
                            "(60-200 BPM, octave chosen around 120), a comb fit for exact BPM and phase, downbeats every 4th beat (4/4 assumed) with the bar phase scored by low-band (< 150 Hz, kick) onsets plus how many section changes land on its bar lines (downbeatEvidence says how), and sections from bar level changes of 3 dB or more "
                            "in the full or the low band, snapped to downbeats and preferably to 8- or 4-bar multiples, labelled intro / build / drop / breakdown (low band 6 dB under the loudest section) / outro, or main when the levels never move. Times are seconds from the start of the analysed audio; with itemId also beatFrames / downbeatFrames on the timeline. "
                            "tempoConfidence is the normalised autocorrelation at the beat period (0..1); below about 0.3 treat the grid as a guess."),
             js::obj({{q("assetId"), js::str()}, {q("itemId"), js::str()}}), structureCmd));
  e.add(spec("audio.check", "", "audio", false,
             QStringLiteral("Verify the mix against a delivery spec. Returns {pass, violations[], measured, speechMargin, limits}; each violation is {rule, frame, timeSec, measured, limit, message} (frame / timeSec null when the rule is about the whole mix). "
                            "Rules: loudness (integrated LUFS off `target` by more than `tolerance`, defaults -14 and 1), true_peak (over `truePeakCeiling`, default -1), clipping (any sample at or above 0.9999), speech_margin (a speech range sits less than "
                            "`minSpeechMarginDb` (6) over the music, per range, checked when dialogue and music tracks exist: pass speechTracks / musicTrack or let them be guessed; ranges as in audio.duck), abrupt_end (the copilot lint rule: an audio item ends at "
                            "volume with no fade-out). Run it before and after a change."),
             js::obj({{q("target"), js::number(QStringLiteral("LUFS, default -14"), -70, 0)}, {q("tolerance"), js::number(QStringLiteral("LU, default 1"), 0, 20)}, {q("truePeakCeiling"), js::number(QStringLiteral("dBTP, default -1"), -20, 3)},
                      {q("minSpeechMarginDb"), js::number(QStringLiteral("dB, default 6"), 0, 40)}, {q("speechTrack"), js::str()}, {q("speechTracks"), js::array(js::str())}, {q("musicTrack"), js::str()},
                      {q("ranges"), js::array(js::obj({{q("startFrame"), js::integer(QString(), 0)}, {q("endFrame"), js::integer(QString(), 0)}}, {q("startFrame"), q("endFrame")}))}, {q("gapMs"), js::number(QString(), 0, 5000)}}),
             checkCmd));
}

} // namespace sf::engine
