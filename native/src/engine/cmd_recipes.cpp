// Recipe execution, recipe listing, and reference video analysis.
// Recipes are named, data-only sequences of existing engine commands applied as ONE undo step.
// reference.analyze measures cuts, colour, loudness, tempo and ranks nearest styles deterministically.

#include "engine/cmd_util.h"
#include "engine/style_library.h"
#include "index/video_analyzer.h"
#include "media/video_decoder.h"
#include "media/audio_decoder.h"
#include "audio/audio_analysis.h"
#include "render/colour_analysis.h"
#include "render/effects.h"

#include <QFileInfo>
#include <algorithm>
#include <cmath>

namespace sf::engine {

using namespace style;

namespace {

QJsonValue fin(double v, double scale = 1000.0) {
  return std::isfinite(v) ? QJsonValue(std::round(v * scale) / scale) : QJsonValue(QJsonValue::Null);
}

// ---------------------------------------------------------------- recipe.list

Result recipeListCmd(CallContext&, const QJsonObject& a) {
  const QString tag = a.value(q("tag")).toString().trimmed().toLower();
  QJsonArray list;
  for (const Recipe& r : recipes()) {
    if (!tag.isEmpty()) {
      bool matched = false;
      for (const QString& w : r.whenToUse) {
        if (w.toLower() == tag || w.toLower().contains(tag)) {
          matched = true;
          break;
        }
      }
      if (!matched) continue;
    }
    list.append(QJsonObject{
      {q("name"), r.name},
      {q("description"), r.description},
      {q("whenToUse"), QJsonArray::fromStringList(r.whenToUse)},
      {q("avoidWhen"), QJsonArray::fromStringList(r.avoidWhen)},
      {q("steps"), static_cast<int>(r.steps.size())}
    });
  }
  return Result::success(QJsonObject{{q("recipes"), list}});
}

// ---------------------------------------------------------------- recipe.apply

struct ResolvedStep {
  QString command;
  QJsonObject args;
};

QJsonObject resolveArgs(const QJsonObject& in,
                        const QStringList& videoIds,
                        const QStringList& audioIds,
                        const QString& mainVideoTrack,
                        const QString& musicItem,
                        const QString& eachVideoId) {
  QJsonObject out;
  for (auto it = in.begin(); it != in.end(); ++it) {
    if (it.value().isString()) {
      const QString s = it.value().toString();
      if (s == QLatin1String("$videoItems")) {
        out.insert(it.key(), QJsonArray::fromStringList(videoIds));
      } else if (s == QLatin1String("$audioItems")) {
        out.insert(it.key(), QJsonArray::fromStringList(audioIds));
      } else if (s == QLatin1String("$mainVideoTrack")) {
        out.insert(it.key(), mainVideoTrack);
      } else if (s == QLatin1String("$musicItem")) {
        out.insert(it.key(), musicItem);
      } else if (s == QLatin1String("$eachVideoItem")) {
        out.insert(it.key(), eachVideoId);
      } else {
        out.insert(it.key(), s);
      }
    } else if (it.value().isObject()) {
      out.insert(it.key(), resolveArgs(it.value().toObject(), videoIds, audioIds, mainVideoTrack, musicItem, eachVideoId));
    } else {
      out.insert(it.key(), it.value());
    }
  }
  return out;
}

bool hasEachVideoPlaceholder(const QJsonObject& obj) {
  for (auto it = obj.begin(); it != obj.end(); ++it) {
    if (it.value().isString() && it.value().toString() == QLatin1String("$eachVideoItem")) return true;
    if (it.value().isObject() && hasEachVideoPlaceholder(it.value().toObject())) return true;
  }
  return false;
}

Result recipeApplyCmd(CallContext& c, const QJsonObject& a) {
  const QString name = a.value(q("name")).toString();
  const Recipe* recipe = findRecipe(name);
  if (!recipe) {
    return notFound(QStringLiteral("No recipe named \"%1\".").arg(name),
                    QStringLiteral("Names: %1. See recipe.list.").arg(recipeNames().join(QStringLiteral(", "))));
  }

  const bool dryRun = a.value(q("dryRun")).toBool(false);
  const TimelineDoc& d = c.session->project->doc();

  // Find main video track
  QString mainVideoTrack;
  for (const Track& t : d.tracks) {
    if (t.kind == TrackKind::Video) {
      mainVideoTrack = t.id;
      break;
    }
  }

  // Gather video and audio item IDs
  QStringList videoIds, audioIds;
  QString musicItemId;
  Frame maxAudioDuration = -1;
  for (const Item& it : d.items) {
    if (it.type() == ItemType::Video) {
      videoIds << it.id;
    } else if (it.type() == ItemType::Audio) {
      audioIds << it.id;
      if (it.durationFrames > maxAudioDuration) {
        maxAudioDuration = it.durationFrames;
        musicItemId = it.id;
      }
    }
  }

  // Resolve placeholders
  std::vector<ResolvedStep> resolved;
  for (const RecipeStep& step : recipe->steps) {
    if (hasEachVideoPlaceholder(step.args)) {
      if (videoIds.isEmpty()) {
        // Run once with empty ID if no video clips
        resolved.push_back({step.command, resolveArgs(step.args, videoIds, audioIds, mainVideoTrack, musicItemId, QString())});
      } else {
        for (const QString& vId : videoIds) {
          resolved.push_back({step.command, resolveArgs(step.args, videoIds, audioIds, mainVideoTrack, musicItemId, vId)});
        }
      }
    } else {
      resolved.push_back({step.command, resolveArgs(step.args, videoIds, audioIds, mainVideoTrack, musicItemId, QString())});
    }
  }

  if (dryRun) {
    QJsonArray stepsArr;
    for (const auto& rs : resolved) {
      stepsArr.append(QJsonObject{{q("command"), rs.command}, {q("args"), rs.args}});
    }
    return Result::success(QJsonObject{
      {q("name"), recipe->name},
      {q("dryRun"), true},
      {q("steps"), stepsArr}
    });
  }

  // Mutating execution under ONE undo group
  c.session->project->beginGroup(QStringLiteral("recipe %1").arg(recipe->name), c.actor());
  QJsonArray stepsOut;
  int stepIdx = 0;

  for (const auto& rs : resolved) {
    QJsonObject stepArgs = rs.args;
    stepArgs.insert(q("session"), c.session->id);
    if (!c.origin.isEmpty()) stepArgs.insert(q("origin"), c.origin);
    if (rs.command == QLatin1String("look.apply") || rs.command == QLatin1String("mix.applyProfile")) {
      if (!stepArgs.contains(q("apply"))) stepArgs.insert(q("apply"), true);
    }
    const Result r = c.engine->call(rs.command, stepArgs);
    if (!r.ok) {
      c.session->project->rollbackGroup();
      return Result::failure(q("recipe_failed"),
                             QStringLiteral("Step %1 (%2) failed: %3").arg(stepIdx).arg(rs.command, r.error.message),
                             QJsonObject{{q("stepIndex"), stepIdx}, {q("command"), rs.command}, {q("error"), r.error.message}});
    }
    QJsonObject summaryObj;
    if (r.value.isObject()) {
      summaryObj = r.value.toObject();
    } else {
      summaryObj.insert(q("value"), r.value);
    }
    stepsOut.append(QJsonObject{{q("command"), rs.command}, {q("args"), rs.args}, {q("ok"), true}, {q("summary"), summaryObj}});
    ++stepIdx;
  }
  c.session->project->endGroup();

  // Run verify steps
  QJsonArray verifyOut;
  for (const RecipeStep& vs : recipe->verify) {
    QJsonObject vArgs = vs.args;
    vArgs.insert(q("session"), c.session->id);
    const Result r = c.engine->call(vs.command, vArgs);
    verifyOut.append(QJsonObject{{q("command"), vs.command}, {q("args"), vs.args}, {q("ok"), r.ok}, {q("result"), r.ok ? r.value : QJsonValue(r.error.message)}});
  }

  return Result::success(QJsonObject{
    {q("name"), recipe->name},
    {q("steps"), stepsOut},
    {q("verify"), verifyOut},
    {q("undoSteps"), 1}
  });
}

// ---------------------------------------------------------------- reference.analyze

render::PixelSet makeMidGreyTestPatch() {
  std::vector<float> lin;
  const double greyLevels[] = {0.04, 0.08, 0.12, 0.18, 0.25, 0.35, 0.50, 0.70};
  for (double g : greyLevels) {
    for (int i = 0; i < 10; ++i) {
      lin.insert(lin.end(), {static_cast<float>(g), static_cast<float>(g), static_cast<float>(g)});
    }
  }
  const float colours[][3] = {
    {0.22f, 0.16f, 0.14f},
    {0.14f, 0.18f, 0.22f},
    {0.20f, 0.14f, 0.18f},
    {0.14f, 0.20f, 0.14f},
    {0.25f, 0.20f, 0.10f},
    {0.10f, 0.15f, 0.25f}
  };
  for (const auto& c : colours) {
    for (int i = 0; i < 5; ++i) {
      lin.insert(lin.end(), {c[0], c[1], c[2]});
    }
  }
  return render::pixelsFromLinear(lin);
}

render::PixelSet sampleFrameToPixelSet(const QImage& src) {
  const QImage img = (src.format() == QImage::Format_RGBA8888) ? src : src.convertToFormat(QImage::Format_RGBA8888);
  const int w = img.width(), h = img.height();
  const int step = std::max(1, (std::max(w, h) + render::kScopeMaxSide - 1) / render::kScopeMaxSide);
  render::PixelSet px;
  const size_t cap = static_cast<size_t>((w + step - 1) / step) * static_cast<size_t>((h + step - 1) / step);
  px.lin.reserve(cap * 3);
  px.disp.reserve(cap * 3);

  for (int y = 0; y < h; y += step) {
    const uchar* row = img.constScanLine(y);
    for (int x = 0; x < w; x += step) {
      const uchar* p = row + x * 4;
      const float rD = p[0] / 255.0f;
      const float gD = p[1] / 255.0f;
      const float bD = p[2] / 255.0f;
      const float rL = static_cast<float>(render::toLinear(render::Transfer::Srgb, rD));
      const float gL = static_cast<float>(render::toLinear(render::Transfer::Srgb, gD));
      const float bL = static_cast<float>(render::toLinear(render::Transfer::Srgb, bD));
      px.lin.insert(px.lin.end(), {rL, gL, bL});
      px.disp.insert(px.disp.end(), {rD, gD, bD});
    }
  }
  return px;
}

Result referenceAnalyzeCmd(CallContext&, const QJsonObject& a) {
  const QString path = a.value(q("path")).toString();
  if (path.isEmpty()) return invalid(QStringLiteral("Pass path: the video file to analyze."));
  const int maxSeconds = std::clamp(a.value(q("maxSeconds")).toInt(300), 1, 3600);

  // 1. Cuts via VideoAnalyzer
  index::VideoAnalyzer va;
  const std::vector<index::SceneRecord> scenes = va.analyzeVideo(path, QStringLiteral("ref"), {});
  const int shots = static_cast<int>(scenes.size());
  double totalDurationSec = 0.0;
  std::vector<double> lengths;
  QJsonArray shotLengthsArr;

  for (const auto& s : scenes) {
    const double len = std::max(0.0, s.endSec - s.startSec);
    lengths.push_back(len);
    if (shotLengthsArr.size() < 200) shotLengthsArr.append(fin(len));
    if (s.endSec > totalDurationSec) totalDurationSec = s.endSec;
  }

  double aslSec = 0.0;
  double minSec = 0.0, maxSec = 0.0, medianSec = 0.0;
  if (!lengths.empty()) {
    double sum = 0.0;
    for (double l : lengths) sum += l;
    aslSec = sum / lengths.size();
    std::vector<double> sorted = lengths;
    std::sort(sorted.begin(), sorted.end());
    minSec = sorted.front();
    maxSec = sorted.back();
    const size_t n = sorted.size();
    medianSec = (n % 2 == 1) ? sorted[n / 2] : ((sorted[n / 2 - 1] + sorted[n / 2]) / 2.0);
  }

  QJsonObject cutsObj{
    {q("shots"), shots},
    {q("aslSec"), fin(aslSec)},
    {q("medianSec"), fin(medianSec)},
    {q("minSec"), fin(minSec)},
    {q("maxSec"), fin(maxSec)},
    {q("shotLengths"), shotLengthsArr}
  };

  // 2. Colour: decode middle frames for up to 12 scenes spread evenly
  QString decErr;
  auto decoder = VideoDecoder::open(path, {}, &decErr);
  if (decoder && decoder->info().durationSec > totalDurationSec) {
    totalDurationSec = decoder->info().durationSec;
  }

  double sumLuma = 0, sumContrast = 0, sumSat = 0, sumTemp = 0, sumTint = 0;
  double sumClippedHigh = 0, sumClippedLow = 0;
  int colourSampleCount = 0;

  if (decoder && !scenes.empty()) {
    const size_t numSamples = std::min<size_t>(scenes.size(), 12);
    for (size_t k = 0; k < numSamples; ++k) {
      const size_t sceneIdx = (numSamples == 1) ? 0 : (k * (scenes.size() - 1) / (numSamples - 1));
      const auto& sc = scenes[sceneIdx];
      const double midSec = (sc.startSec + sc.endSec) / 2.0;
      const qint64 frameIdx = decoder->indexAtTime(midSec);
      const VideoFramePtr frame = decoder->frameAt(frameIdx);
      if (frame && !frame->image.isNull()) {
        const render::PixelSet px = sampleFrameToPixelSet(frame->image);
        const render::ColourMetrics cm = render::analyze(px);
        if (cm.valid) {
          sumLuma += cm.lumaMean;
          sumContrast += (cm.p95 - cm.p5);
          sumSat += cm.chromaMean;
          sumTemp += cm.castTemperature;
          sumTint += cm.castTint;
          sumClippedHigh += cm.clippedPct.all;
          sumClippedLow += cm.crushedPct.all;
          ++colourSampleCount;
        }
      }
    }
  }

  const double avgLuma = colourSampleCount > 0 ? (sumLuma / colourSampleCount) : 0.18;
  const double avgContrast = colourSampleCount > 0 ? (sumContrast / colourSampleCount) : 0.5;
  const double avgSat = colourSampleCount > 0 ? (sumSat / colourSampleCount) : 0.1;
  const double avgTemp = colourSampleCount > 0 ? (sumTemp / colourSampleCount) : 0.0;
  const double avgTint = colourSampleCount > 0 ? (sumTint / colourSampleCount) : 0.0;
  const double avgClippedHigh = colourSampleCount > 0 ? (sumClippedHigh / colourSampleCount) : 0.0;
  const double avgClippedLow = colourSampleCount > 0 ? (sumClippedLow / colourSampleCount) : 0.0;

  QJsonObject colourObj{
    {q("meanLuma"), fin(avgLuma)},
    {q("contrast"), fin(avgContrast)},
    {q("saturation"), fin(avgSat)},
    {q("castTemperature"), fin(avgTemp)},
    {q("castTint"), fin(avgTint)},
    {q("clippedHighPct"), fin(avgClippedHigh)},
    {q("clippedLowPct"), fin(avgClippedLow)},
    {q("samples"), colourSampleCount}
  };

  // 3. Audio: loudness and music structure
  QString audioErr;
  auto audioDec = AudioDecoder::open(path, &audioErr);
  QJsonValue audioVal = QJsonValue(QJsonValue::Null);
  double measuredLufs = -16.0;
  double measuredTempoConf = 0.0;
  bool haveAudio = false;

  if (audioDec && audioDec->info().totalSamples > 0) {
    haveAudio = true;
    const qint64 maxSamples = static_cast<qint64>(maxSeconds) * AudioDecoder::kSampleRate;
    const qint64 count = std::min(audioDec->info().totalSamples, maxSamples);
    std::vector<float> buf(static_cast<size_t>(count) * 2);
    audioDec->read(0, count, buf.data());

    audio::MixAnalyzer ma;
    ma.process(buf.data(), count);
    const audio::MixMeasurement mm = ma.result();
    measuredLufs = mm.loudness.integrated;

    const audio::MusicStructure ms = audio::analyseMusic(buf.data(), count);
    measuredTempoConf = ms.tempoConfidence;

    // Cut-to-beat alignment: % of cuts within 50 ms of a beat
    double cutAlignmentPct = 0.0;
    if (scenes.size() > 1 && !ms.beats.empty()) {
      int alignedCount = 0;
      const int cutCount = static_cast<int>(scenes.size()) - 1;
      for (size_t i = 1; i < scenes.size(); ++i) {
        const double cutSec = scenes[i].startSec;
        bool onBeat = false;
        for (double b : ms.beats) {
          if (std::fabs(cutSec - b) <= 0.050) {
            onBeat = true;
            break;
          }
        }
        if (onBeat) ++alignedCount;
      }
      cutAlignmentPct = (static_cast<double>(alignedCount) / cutCount) * 100.0;
    }

    QJsonArray sectionsArr;
    for (const auto& sec : ms.sections) {
      sectionsArr.append(QJsonObject{
        {q("startSec"), fin(sec.startSec)},
        {q("endSec"), fin(sec.endSec)},
        {q("startBar"), sec.startBar},
        {q("endBar"), sec.endBar},
        {q("label"), QString::fromStdString(sec.label)},
        {q("energyDb"), fin(sec.energyDb)},
        {q("relEnergy"), fin(sec.relEnergy)}
      });
    }

    audioVal = QJsonObject{
      {q("lufs"), fin(mm.loudness.integrated)},
      {q("truePeakDb"), fin(mm.loudness.truePeakDb)},
      {q("bpm"), fin(ms.bpm)},
      {q("tempoConfidence"), fin(ms.tempoConfidence)},
      {q("sections"), sectionsArr},
      {q("cutToBeatAlignmentPct"), fin(cutAlignmentPct)}
    };
  }

  // 4. Derived tags & Nearest rankings
  // Pacing ranking
  struct DistanceItem {
    QString name;
    double distance = 0;
    int score = 0;
  };
  std::vector<DistanceItem> pacingRanked;
  for (const PacingProfile& p : pacings()) {
    double dist = 0.0;
    if (aslSec < p.aslMin) dist = p.aslMin - aslSec;
    else if (aslSec > p.aslMax) dist = aslSec - p.aslMax;
    pacingRanked.push_back({p.name, dist, 0});
  }
  std::sort(pacingRanked.begin(), pacingRanked.end(), [](const DistanceItem& a, const DistanceItem& b) {
    return a.distance != b.distance ? a.distance < b.distance : a.name < b.name;
  });

  // Looks ranking: Euclidean distance on mid-grey test patch
  const render::PixelSet testPatch = makeMidGreyTestPatch();
  std::vector<DistanceItem> looksRanked;
  for (const Look& l : looks()) {
    const render::PixelSet graded = render::gradePixels(testPatch, l.grade);
    const render::ColourMetrics cm = render::analyze(graded);
    const double lookTemp = cm.castTemperature;
    const double lookTint = cm.castTint;
    const double lookContrast = cm.p95 - cm.p5;
    const double lookSat = cm.chromaMean;

    const double dT = avgTemp - lookTemp;
    const double dTint = avgTint - lookTint;
    const double dC = avgContrast - lookContrast;
    const double dS = avgSat - lookSat;
    const double dist = std::sqrt(dT * dT + dTint * dTint + dC * dC + dS * dS);
    looksRanked.push_back({l.name, dist, 0});
  }
  std::sort(looksRanked.begin(), looksRanked.end(), [](const DistanceItem& a, const DistanceItem& b) {
    return a.distance != b.distance ? a.distance < b.distance : a.name < b.name;
  });

  // Mix ranking: |LUFS - profile target|
  std::vector<DistanceItem> mixRanked;
  for (const MixProfile& m : mixes()) {
    const double dist = std::fabs(measuredLufs - m.targetLufs);
    mixRanked.push_back({m.name, dist, 0});
  }
  std::sort(mixRanked.begin(), mixRanked.end(), [](const DistanceItem& a, const DistanceItem& b) {
    return a.distance != b.distance ? a.distance < b.distance : a.name < b.name;
  });

  // Tags derived from measurements:
  // "high-energy" if ASL < 2.5, "calm" if ASL > 6, "music" if tempoConfidence > 0.5,
  // "warm"/"cool" from temperature +-0.1, "contrasty" if contrast > 0.75
  QStringList tags;
  if (aslSec < 2.5 && aslSec > 0) tags << QStringLiteral("high-energy");
  if (aslSec > 6.0) tags << QStringLiteral("calm");
  if (measuredTempoConf > 0.5) tags << QStringLiteral("music");
  if (avgTemp > 0.1) tags << QStringLiteral("warm");
  else if (avgTemp < -0.1) tags << QStringLiteral("cool");
  if (avgContrast > 0.75) tags << QStringLiteral("contrasty");

  // Recipes ranking via rankByTags
  std::vector<Candidate> recipeCands;
  for (const Recipe& r : recipes()) {
    recipeCands.push_back({r.name, r.whenToUse, r.avoidWhen});
  }
  const std::vector<Suggestion> recipeSugs = rankByTags(recipeCands, tags);

  QJsonArray pacingArr, looksArr, mixArr, recipesArr;
  for (const auto& item : pacingRanked) {
    pacingArr.append(QJsonObject{{q("name"), item.name}, {q("distance"), fin(item.distance)}});
  }
  for (const auto& item : looksRanked) {
    looksArr.append(QJsonObject{{q("name"), item.name}, {q("distance"), fin(item.distance)}});
  }
  for (const auto& item : mixRanked) {
    mixArr.append(QJsonObject{{q("name"), item.name}, {q("distance"), fin(item.distance)}});
  }
  for (const auto& sug : recipeSugs) {
    recipesArr.append(QJsonObject{{q("name"), sug.id}, {q("score"), sug.score}, {q("matched"), QJsonArray::fromStringList(sug.matched)}});
  }

  QJsonObject nearestObj{
    {q("pacing"), pacingArr},
    {q("looks"), looksArr},
    {q("mix"), mixArr},
    {q("recipes"), recipesArr},
    {q("formulas"), QJsonObject{
      {q("pacing"), q("distance from aslSec to profile [aslMin, aslMax], 0 if inside")},
      {q("looks"), q("Euclidean distance over (temperature, tint, contrast, saturation) measured on mid-grey test patch")},
      {q("mix"), q("|LUFS - targetLufs|")},
      {q("recipes"), q("style.suggest score from derived measurement tags (high-energy, calm, music, warm, cool, contrasty)")}
    }}
  };

  // 5. Build styleSpec validating against style.spec.schema
  QJsonObject specObj;
  if (!looksRanked.empty()) specObj.insert(q("look"), looksRanked.front().name);
  specObj.insert(q("intensity"), 1.0);
  specObj.insert(q("warmth"), fin(std::clamp(avgTemp, -kMaxWarmthOffset, kMaxWarmthOffset)));
  specObj.insert(q("contrast"), fin(std::clamp(avgContrast - 0.5, -kMaxContrastOffset, kMaxContrastOffset)));
  specObj.insert(q("saturation"), fin(std::clamp(avgSat - 0.1, -kMaxSaturationOffset, kMaxSaturationOffset)));
  if (!pacingRanked.empty()) specObj.insert(q("pacing"), pacingRanked.front().name);
  if (!mixRanked.empty()) specObj.insert(q("mix"), mixRanked.front().name);
  specObj.insert(q("source"), QStringLiteral("measured from %1").arg(QFileInfo(path).fileName()));

  QJsonArray notesArr;
  notesArr.append(QStringLiteral("Measured %1 shots over %2 s (ASL %3 s).").arg(shots).arg(totalDurationSec, 0, 'f', 1).arg(aslSec, 0, 'f', 2));
  if (haveAudio) {
    notesArr.append(QStringLiteral("Audio: %1 LUFS integrated, tempo confidence %2.").arg(measuredLufs, 0, 'f', 1).arg(measuredTempoConf, 0, 'f', 2));
  } else {
    notesArr.append(q("Audio stream not present or could not be decoded."));
  }

  return Result::success(QJsonObject{
    {q("path"), path},
    {q("durationSec"), fin(totalDurationSec)},
    {q("cuts"), cutsObj},
    {q("colour"), colourObj},
    {q("audio"), audioVal},
    {q("styleSpec"), specObj},
    {q("nearest"), nearestObj},
    {q("tags"), QJsonArray::fromStringList(tags)},
    {q("notes"), notesArr}
  });
}

} // namespace

void registerRecipesCommands(Engine& e) {
  e.add(spec("recipe.list", "", "style", false,
             QStringLiteral("List recipes from the style library. Optional `tag` filters by whenToUse/tags."),
             js::obj({{q("tag"), js::str()}}),
             recipeListCmd, false));

  e.add(spec("recipe.apply", "", "style", true,
             QStringLiteral("Apply a named recipe from the style library. Placeholders ($videoItems, $audioItems, $mainVideoTrack, $eachVideoItem, $musicItem) are resolved at apply time. dryRun returns resolved steps without executing. Execution runs in one undo group; on failure, rolls back and returns failing step index. Then runs verify steps."),
             js::obj({{q("name"), js::str(QStringLiteral("Name of the recipe from recipe.list"))}, {q("dryRun"), js::boolean()}}, {q("name")}),
             recipeApplyCmd, true));

  e.add(spec("reference.analyze", "", "style", false,
             QStringLiteral("Measure an existing video file (cuts, colour, loudness, tempo) and rank nearest styles deterministically. Deterministic formulas, no model hallucination."),
             js::obj({{q("path"), js::str(QStringLiteral("Path to video file"))}, {q("maxSeconds"), js::integer(QStringLiteral("Max audio seconds to decode, default 300"), 1, 3600)}}, {q("path")}),
             referenceAnalyzeCmd, false));
}

} // namespace sf::engine
