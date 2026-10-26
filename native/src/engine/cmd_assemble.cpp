// edit.assemble: deterministic rough cut from shots + music + pacing profile.

#include "engine/assembly.h"
#include "engine/cmd_util.h"
#include "engine/style_library.h"
#include "audio/audio_analysis.h"

#include <cmath>
#include <vector>

namespace sf::engine {

namespace {

Result assembleCmd(CallContext& c, const QJsonObject& a) {
  const QJsonArray assetIdsArr = a.value(q("assetIds")).toArray();
  if (assetIdsArr.isEmpty()) {
    return invalid(QStringLiteral("Argument 'assetIds' must be non-empty"));
  }
  QStringList assetIds;
  for (const auto& v : assetIdsArr) assetIds.append(v.toString());

  const QString musicItemId = a.value(q("musicItemId")).toString();
  if (musicItemId.isEmpty()) {
    return invalid(QStringLiteral("Argument 'musicItemId' is required"));
  }

  const bool apply = a.value(q("apply")).toBool(false);

  const QString pacingName = a.value(q("pacing")).toString();
  if (pacingName.isEmpty()) {
    return invalid(QStringLiteral("Argument 'pacing' is required"));
  }

  const auto* prof = style::findPacing(pacingName);
  if (!prof) {
    return notFound(QStringLiteral("Pacing profile '%1' not found").arg(pacingName));
  }

  const TimelineDoc& doc = c.session->project->doc();
  const double fps = static_cast<double>(doc.project.fps);

  const Item* musicItem = getItem(doc, musicItemId);
  QString resolvedMusicId = musicItemId;
  if (!musicItem) {
    // Not a timeline item: maybe it is a music asset id from assets.list.
    const Asset* musicAsset = c.session->project->asset(musicItemId);
    if (musicAsset && (musicAsset->kind == AssetKind::Audio || musicAsset->hasAudio)) {
      if (!apply) {
        return invalid(QStringLiteral("Music item '%1' is not on the timeline: call audio.add with asset id '%1' first, or set apply=true").arg(musicItemId));
      }
      // Place the asset with the same command the caller would use, then assemble onto it. The caller's origin goes along:
      // without it the inner call is "api" and the copilot lock refuses it while that agent holds the project.
      QJsonObject placeArgs{{q("assetId"), musicItemId}, {q("session"), c.session->id}};
      if (!c.origin.isEmpty()) placeArgs.insert(q("origin"), c.origin);
      Result rPlace = c.engine->call(q("audio.add"), placeArgs);
      if (!rPlace.ok) return rPlace;
      const QJsonArray createdItems = rPlace.value.toObject().value(q("created")).toObject().value(q("items")).toArray();
      if (createdItems.isEmpty()) {
        return Result::failure(q("edit_rejected"), QStringLiteral("audio.add placed asset '%1' but created no item").arg(musicItemId));
      }
      resolvedMusicId = createdItems.first().toString();
      musicItem = getItem(doc, resolvedMusicId);
      if (!musicItem) {
        return Result::failure(q("edit_rejected"), QStringLiteral("Placed music asset '%1' but item '%2' is missing").arg(musicItemId, resolvedMusicId));
      }
    } else {
      return notFound(QStringLiteral("Music item '%1' not found: pass the id of an audio item on the timeline (audio.add returns it) or a music asset id").arg(musicItemId));
    }
  }
  if (!musicItem->assetId || (musicItem->type() != ItemType::Audio && musicItem->type() != ItemType::Video)) {
    return invalid(QStringLiteral("Item '%1' is not an audio item").arg(musicItemId));
  }

  // 1. Get scene candidates
  const bool hasIndex = (c.engine->find(q("index.scenes")) != nullptr);
  std::vector<ShotCandidate> pool;
  QString indexNote;

  for (const QString& aid : assetIds) {
    if (hasIndex) {
      Result rScenes = c.engine->call(q("index.scenes"), {{q("assetId"), aid}});
      if (!rScenes.ok || !rScenes.value.isArray() || rScenes.value.toArray().isEmpty()) {
        return invalid(QStringLiteral("Asset %1 is not analysed: call index.analyze first").arg(aid));
      }
      for (const auto& scVal : rScenes.value.toArray()) {
        const QJsonObject scObj = scVal.toObject();
        ShotCandidate cand;
        cand.assetId = aid;
        cand.startSec = scObj.value(q("startSec")).toDouble();
        cand.endSec = scObj.value(q("endSec")).toDouble();
        cand.sharpness = scObj.value(q("sharpness")).toDouble();
        cand.lumaAvg = scObj.value(q("lumaAvg")).toDouble(128.0);
        cand.clippedPct = scObj.value(q("lumaClippedPct")).toDouble();
        cand.motion = scObj.value(q("motionScore")).toDouble();
        cand.freeze = scObj.value(q("isFreeze")).toBool();
        cand.black = scObj.value(q("isBlack")).toBool();
        pool.push_back(cand);
      }
    } else {
      const Asset* as = c.session->project->asset(aid);
      if (!as) {
        return notFound(QStringLiteral("Asset %1 not found").arg(aid));
      }
      const double durSec = as->durationMs > 0 ? (static_cast<double>(as->durationMs) / 1000.0) : 30.0;
      ShotCandidate cand;
      cand.assetId = aid;
      cand.startSec = 0.0;
      cand.endSec = durSec;
      cand.sharpness = 50.0;
      cand.lumaAvg = 128.0;
      cand.clippedPct = 0.0;
      cand.motion = 10.0;
      cand.freeze = false;
      cand.black = false;
      pool.push_back(cand);
      indexNote = QStringLiteral("Index manager unavailable: using whole assets as neutral candidates.");
    }
  }

  // 2. Music analysis
  Result rMusic = c.engine->call(q("music.structure"), {{q("itemId"), resolvedMusicId}, {q("session"), c.session->id}});
  if (!rMusic.ok) {
    return Result::failure(q("audio_error"), QStringLiteral("Failed to analyse music: %1").arg(rMusic.error.message));
  }

  const QJsonObject msObj = rMusic.value.toObject();
  MusicGrid grid;

  const double speed = (musicItem->speed > 0) ? musicItem->speed : 1.0;
  const Frame atFrame = static_cast<Frame>(a.value(q("atFrame")).toInteger(0));
  const double offsetSec = static_cast<double>(musicItem->startFrame - atFrame) / fps;

  const double musicDurTimeline = static_cast<double>(musicItem->durationFrames) / fps;
  grid.durationSec = std::max(0.0, offsetSec + musicDurTimeline);

  for (const auto& v : msObj.value(q("beats")).toArray()) {
    const double b = offsetSec + (v.toDouble() / speed);
    if (b >= 0.0) {
      grid.beats.push_back(b);
    }
  }
  for (const auto& v : msObj.value(q("downbeats")).toArray()) {
    const double d = offsetSec + (v.toDouble() / speed);
    if (d >= 0.0) {
      grid.downbeats.push_back(d);
    }
  }
  for (const auto& sVal : msObj.value(q("sections")).toArray()) {
    const QJsonObject sObj = sVal.toObject();
    const double sStart = offsetSec + (sObj.value(q("startSec")).toDouble() / speed);
    const double sEnd = offsetSec + (sObj.value(q("endSec")).toDouble() / speed);
    if (sEnd > 0.0) {
      grid.sections.push_back({
        std::max(0.0, sStart),
        sEnd,
        sObj.value(q("relEnergy")).toDouble(0.5),
        sObj.value(q("label")).toString()
      });
    }
  }

  // 3. Pacing target & planAssembly
  PacingTarget target;
  target.aslMin = prof->aslMin;
  target.aslMax = prof->aslMax;
  target.minShot = prof->minShotSec;
  target.maxShot = prof->maxShotSec;
  target.hookMax = prof->hookMaxSec;
  target.cutsOnBeatMinPct = static_cast<int>(prof->cutsOnBeatMinPct);

  double targetSec = 0.0;
  if (a.contains(q("targetFrames"))) {
    targetSec = static_cast<double>(a.value(q("targetFrames")).toInteger()) / fps;
  }
  const quint32 seed = static_cast<quint32>(a.value(q("seed")).toInt(1));

  AssemblyPlan plan = planAssembly(pool, grid, target, targetSec, seed);
  if (!plan.ok) {
    return Result::failure(q("plan_failed"), plan.error);
  }
  if (!indexNote.isEmpty()) {
    plan.notes.append(indexNote);
  }

  // 4. Build output shots with frames
  QString trackId = a.value(q("trackId")).toString();
  if (trackId.isEmpty()) {
    for (const Track& t : doc.tracks) {
      if (t.kind == TrackKind::Video && !t.locked) {
        trackId = t.id;
        break;
      }
    }
  }
  const bool needAddTrack = trackId.isEmpty();
  if (needAddTrack) {
    trackId = newId(q("trk"));
  }

  Frame curFrame = atFrame;
  QJsonArray shotsArr;
  struct ShotFrameInfo {
    QString assetId;
    double srcInSec;
    double srcOutSec;
    Frame startF;
    Frame endF;
    QString reason;
  };
  std::vector<ShotFrameInfo> frameShots;

  for (const auto& shot : plan.shots) {
    const Frame startF = curFrame;
    const Frame endF = atFrame + static_cast<Frame>(std::llround(shot.timelineEndSec * fps));
    const Frame durF = std::max<Frame>(1, endF - startF);
    const Frame actualEndF = startF + durF;
    curFrame = actualEndF;

    frameShots.push_back({shot.assetId, shot.srcInSec, shot.srcOutSec, startF, actualEndF, shot.reason});

    shotsArr.append(QJsonObject{
      {q("assetId"), shot.assetId},
      {q("srcInSec"), shot.srcInSec},
      {q("srcOutSec"), shot.srcOutSec},
      {q("startFrame"), static_cast<double>(startF)},
      {q("endFrame"), static_cast<double>(actualEndF)},
      {q("reason"), shot.reason}
    });
  }

  const double cutsOnBeatPct = plan.cuts > 0 ? (100.0 * plan.cutsOnBeat / plan.cuts) : 100.0;
  QJsonObject planObj{
    {q("shots"), shotsArr},
    {q("cuts"), plan.cuts},
    {q("cutsOnBeat"), plan.cutsOnBeat},
    {q("cutsOnBeatPct"), cutsOnBeatPct},
    {q("aslSec"), plan.aslSec},
    {q("totalSec"), plan.totalSec},
    {q("notes"), QJsonArray::fromStringList(plan.notes)},
    {q("warnings"), QJsonArray::fromStringList(plan.warnings)}
  };

  QJsonObject out{
    {q("plan"), planObj},
    {q("applied"), false}
  };
  if (!plan.warnings.isEmpty()) {
    out.insert(q("warnings"), QJsonArray::fromStringList(plan.warnings));
  }

  // 5. Apply if requested
  const bool replaceTrack = a.value(q("replaceTrack")).toBool(false);

  if (apply) {
    std::vector<QString> itemsToRemove;
    for (const Item& it : doc.items) {
      if (it.trackId == trackId) {
        if (itemEnd(it) > atFrame && it.startFrame < curFrame) {
          if (!replaceTrack) {
            return invalid(QStringLiteral("Overlap with item %1 on track %2").arg(it.id, trackId));
          }
          itemsToRemove.push_back(it.id);
        }
      }
    }

    std::vector<Op> ops;
    if (needAddTrack) {
      ops.push_back(Op(TrackAdd{trackId, TrackKind::Video, QStringLiteral("Video")}));
    }
    if (!itemsToRemove.empty()) {
      ops.push_back(Op(ItemRemove{itemsToRemove, false}));
    }

    for (const auto& fs : frameShots) {
      ItemInit init;
      init.id = newId(q("itm"));
      init.trackId = trackId;
      init.startFrame = fs.startF;
      init.durationFrames = fs.endF - fs.startF;
      init.assetId = fs.assetId;
      init.sourceInFrame = static_cast<Frame>(std::llround(fs.srcInSec * fps));
      init.labels = Labels{QStringLiteral("clip"), std::nullopt};

      Item itm = createItem(ItemType::Video, init);
      ops.push_back(Op(ItemAdd{itm, false}));
    }

    Result rApply = applyToSession(c, ops, QStringLiteral("edit.assemble"), {{q("trackId"), trackId}});
    if (!rApply.ok) return rApply;

    out.insert(q("applied"), true);

    // 6. After applying: call pacing.check
    Result rCheck = c.engine->call(q("pacing.check"), {{q("profile"), pacingName}, {q("session"), c.session->id}});
    if (rCheck.ok) {
      out.insert(q("check"), rCheck.value);
    }
  }

  return Result::success(out);
}

} // namespace

void registerAssembleCommands(Engine& e) {
  e.add(spec("edit.assemble", "", "editor", true,
             QStringLiteral("Build deterministic rough cut from shots, music item, and pacing profile. Cuts on beats and sections. Deterministic code, not a model."),
             js::obj({{q("assetIds"), js::array(js::str(), QStringLiteral("List of asset IDs to cut between"))},
                      {q("musicItemId"), js::str(QStringLiteral("ID of an audio item on the timeline (audio.add returns it), or a music asset id from assets.list"))},
                      {q("pacing"), js::str(QStringLiteral("Name of pacing profile from pacing.json"))},
                      {q("targetFrames"), js::integer(QStringLiteral("Target duration in frames (default music length)"), 1)},
                      {q("trackId"), js::str(QStringLiteral("Target video track ID (default first video track)"))},
                      {q("atFrame"), js::integer(QStringLiteral("Start frame on timeline (default 0)"), 0)},
                      {q("seed"), js::integer(QStringLiteral("Deterministic tie-breaker seed (default 1)"), 0)},
                      {q("apply"), js::boolean(QStringLiteral("Apply assembly onto timeline as ONE undo step (default false)"))},
                      {q("replaceTrack"), js::boolean(QStringLiteral("Replace overlapping items on target track (default false)"))}},
                     {q("assetIds"), q("musicItemId"), q("pacing")}),
             assembleCmd));
}

} // namespace sf::engine
