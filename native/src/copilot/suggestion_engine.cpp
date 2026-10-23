#include "copilot/suggestion_engine.h"
#include "core/json_reader.h"
#include "core/ops.h"
#include "core/time.h"

#include <cmath>
#include <algorithm>

namespace sf::copilot {

SuggestionEngine::SuggestionEngine(QObject* parent) : QObject(parent) {}

void SuggestionEngine::setSuggestion(const Suggestion& s) {
  current_ = s;
  emit suggestionAvailable(s);
}

void SuggestionEngine::dismiss() {
  if (current_) {
    current_.reset();
    emit suggestionDismissed();
  }
}

bool SuggestionEngine::accept(editor::Project* project) {
  if (!current_ || !project) return false;

  const Suggestion s = *current_;
  if (s.ops.isEmpty()) {
    dismiss();
    return true;
  }

  std::vector<Op> ops;
  for (const auto& opJson : s.ops) {
    try {
      ops.push_back(parseOp(Rd(QJsonValue(opJson))));
    } catch (...) {
      return false;
    }
  }

  if (ops.empty()) return false;

  const bool ok = project->apply(ops, s.title);
  if (ok) {
    current_.reset();
    emit suggestionAccepted(s);
  }
  return ok;
}

std::optional<Suggestion> SuggestionEngine::detectPauseAfterSplit(const TimelineDoc& doc,
                                                                 const QString& itemId,
                                                                 const std::vector<index::SpeechSpan>& spans,
                                                                 qint64 minPauseMs) {
  const Item* item = nullptr;
  for (const auto& it : doc.items) {
    if (it.id == itemId) {
      item = &it;
      break;
    }
  }
  if (!item || !item->assetId) return std::nullopt;

  const qint64 fps = doc.project.fps > 0 ? doc.project.fps : 30;
  const Frame inFrame = item->sourceInFrame.value_or(0);
  const double inSec = static_cast<double>(inFrame) / fps;

  // Check if at the beginning of this cut segment there is a silence span
  for (const auto& span : spans) {
    if (span.assetId != *item->assetId) continue;
    if (!span.isSpeech) {
      // Span is silence: check if inSec falls inside this silence
      if (inSec >= span.startSec - 0.05 && inSec < span.endSec - 0.1) {
        const double pauseSec = span.endSec - inSec;
        const qint64 pauseMs = static_cast<qint64>(pauseSec * 1000);
        if (pauseMs >= minPauseMs) {
          const Frame pauseFrames = static_cast<Frame>(pauseSec * fps);
          // item.trim takes TIMELINE frames (core/apply.cpp computeTrim): the pause is
          // pauseFrames of source, which is pauseFrames/speed timeline frames of head cut.
          const qint64 cutFrames = std::llround(static_cast<double>(pauseFrames) / (item->speed > 0 ? item->speed : 1.0));
          if (cutFrames <= 0 || cutFrames >= item->durationFrames) continue;

          Suggestion s;
          s.id = QStringLiteral("sug-pause-%1").arg(itemId);
          s.kind = SuggestionKind::GhostOp;
          s.ghostKind = QStringLiteral("gap");
          s.title = QStringLiteral("Tab: Ripple delete pause (%1s)").arg(pauseSec, 0, 'f', 2);
          s.description = QStringLiteral("Remove dead silence of %1s at the start of clip '%2'").arg(pauseSec, 0, 'f', 2).arg(itemId);
          s.targetTrackId = item->trackId;
          s.targetItemId = item->id;
          s.startMs = (item->startFrame * 1000) / fps;
          s.durationMs = pauseMs;
          s.confidence = 0.95f;

          // Trim head of item by cutFrames with ripple; the frame is a timeline frame.
          s.ops.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("item.trim")},
            {QStringLiteral("itemId"), item->id},
            {QStringLiteral("edge"), QStringLiteral("in")},
            {QStringLiteral("frame"), item->startFrame + cutFrames},
            {QStringLiteral("ripple"), true}
          });
          return s;
        }
      }
    }
  }
  return std::nullopt;
}

std::optional<Suggestion> SuggestionEngine::detectLcutAudioExtension(const TimelineDoc& doc,
                                                                    const QString& outgoingItemId,
                                                                    qint64 leadMs) {
  const Item* item = nullptr;
  for (const auto& it : doc.items) {
    if (it.id == outgoingItemId) {
      item = &it;
      break;
    }
  }
  if (!item || item->type() != ItemType::Video) return std::nullopt;

  const qint64 fps = doc.project.fps > 0 ? doc.project.fps : 30;
  const Frame leadFrames = (leadMs * fps) / 1000;
  if (leadFrames <= 0) return std::nullopt;

  Suggestion s;
  s.id = QStringLiteral("sug-lcut-%1").arg(outgoingItemId);
  s.kind = SuggestionKind::GhostTrim;
  s.ghostKind = QStringLiteral("trim");
  s.title = QStringLiteral("Tab: Create L-cut audio extension (%1ms)").arg(leadMs);
  s.description = QStringLiteral("Extend outgoing dialogue audio under the incoming video for a split edit.");
  s.targetTrackId = item->trackId;
  s.targetItemId = item->id;
  s.startMs = ((item->startFrame + item->durationFrames) * 1000) / fps;
  s.durationMs = leadMs;
  s.confidence = 0.90f;

  // item.trim takes a TIMELINE frame: the extension is leadFrames of timeline tail
  // (the extra source playback consumes follows from the item's speed inside core).
  s.ops.append(QJsonObject{
    {QStringLiteral("type"), QStringLiteral("item.trim")},
    {QStringLiteral("itemId"), item->id},
    {QStringLiteral("edge"), QStringLiteral("out")},
    {QStringLiteral("frame"), item->startFrame + item->durationFrames + leadFrames},
    {QStringLiteral("ripple"), false}
  });

  return s;
}

std::optional<Suggestion> SuggestionEngine::detectBeatAlignment(const TimelineDoc& doc,
                                                               const QString& itemId,
                                                               bool inEdge,
                                                               const std::vector<double>& beatsMs,
                                                               qint64 thresholdMs) {
  const Item* item = nullptr;
  for (const auto& it : doc.items) {
    if (it.id == itemId) {
      item = &it;
      break;
    }
  }
  if (!item || beatsMs.empty()) return std::nullopt;

  const qint64 fps = doc.project.fps > 0 ? doc.project.fps : 30;
  const Frame cutFrame = inEdge ? item->startFrame : (item->startFrame + item->durationFrames);
  const qint64 cutMs = (cutFrame * 1000) / fps;

  double nearestBeatMs = -1.0;
  double minDiff = 1e9;
  for (double bMs : beatsMs) {
    const double diff = std::abs(bMs - cutMs);
    if (diff < minDiff) {
      minDiff = diff;
      nearestBeatMs = bMs;
    }
  }

  if (nearestBeatMs >= 0.0 && minDiff > 0.0 && minDiff <= thresholdMs) {
    const Frame beatFrame = static_cast<Frame>(std::round((nearestBeatMs * fps) / 1000.0));
    if (beatFrame == cutFrame) return std::nullopt;

    Suggestion s;
    s.id = QStringLiteral("sug-beat-%1").arg(itemId);
    s.kind = SuggestionKind::GhostTrim;
    s.ghostKind = QStringLiteral("trim");
    s.title = QStringLiteral("Tab: Snap cut to beat at %1s").arg(nearestBeatMs / 1000.0, 0, 'f', 2);
    s.description = QStringLiteral("Align cut boundary exactly with musical transient beat.");
    s.targetTrackId = item->trackId;
    s.targetItemId = item->id;
    s.startMs = static_cast<qint64>(nearestBeatMs);
    s.durationMs = static_cast<qint64>(minDiff);
    s.confidence = 0.92f;

    // item.trim takes a TIMELINE frame on both edges (core/apply.cpp): snap the cut to the
    // beat frame directly; core itself converts the in-edge delta into source frames via speed.
    s.ops.append(QJsonObject{
      {QStringLiteral("type"), QStringLiteral("item.trim")},
      {QStringLiteral("itemId"), item->id},
      {QStringLiteral("edge"), inEdge ? QStringLiteral("in") : QStringLiteral("out")},
      {QStringLiteral("frame"), static_cast<qint64>(beatFrame)},
      {QStringLiteral("ripple"), false}
    });

    return s;
  }

  return std::nullopt;
}

std::optional<Suggestion> SuggestionEngine::detectBrollOverlay(const TimelineDoc& doc,
                                                              const QString& primaryItemId,
                                                              const QString& brollAssetId,
                                                              const QString& overlayTrackId,
                                                              std::int64_t brollDurationFrames) {
  const Item* primary = nullptr;
  for (const auto& it : doc.items) {
    if (it.id == primaryItemId) {
      primary = &it;
      break;
    }
  }
  if (!primary || brollAssetId.isEmpty() || overlayTrackId.isEmpty()) return std::nullopt;

  const qint64 fps = doc.project.fps > 0 ? doc.project.fps : 30;
  const Frame dur = std::min(brollDurationFrames, primary->durationFrames);
  if (dur <= 0) return std::nullopt;

  const QString newItemId = newId(QStringLiteral("itm"));

  Suggestion s;
  s.id = QStringLiteral("sug-broll-%1").arg(newItemId);
  s.kind = SuggestionKind::GhostClip;
  s.ghostKind = QStringLiteral("clip");
  s.title = QStringLiteral("Tab: Insert B-roll overlay over dialogue");
  s.description = QStringLiteral("Insert matching cutaway clip on track '%1' to cover dialogue pacing.").arg(overlayTrackId);
  s.targetTrackId = overlayTrackId;
  s.targetItemId = newItemId;
  s.startMs = (primary->startFrame * 1000) / fps;
  s.durationMs = (dur * 1000) / fps;
  s.confidence = 0.88f;

  QJsonObject itemObj{
    {QStringLiteral("id"), newItemId},
    {QStringLiteral("trackId"), overlayTrackId},
    {QStringLiteral("type"), QStringLiteral("video")},
    {QStringLiteral("startFrame"), static_cast<qint64>(primary->startFrame)},
    {QStringLiteral("durationFrames"), static_cast<qint64>(dur)},
    {QStringLiteral("assetId"), brollAssetId},
    {QStringLiteral("sourceInFrame"), 0},
    {QStringLiteral("speed"), 1.0},
    {QStringLiteral("volume"), 0.0},
    {QStringLiteral("muted"), true}
  };

  s.ops.append(QJsonObject{
    {QStringLiteral("type"), QStringLiteral("item.add")},
    {QStringLiteral("item"), itemObj}
  });

  return s;
}

} // namespace sf::copilot
