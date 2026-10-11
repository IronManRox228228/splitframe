#include "copilot/lint_engine.h"
#include "core/time.h"

#include <cmath>
#include <algorithm>
#include <map>

namespace sf::copilot {

std::vector<LintIssue> LintEngine::analyze(const TimelineDoc& doc,
                                           const std::vector<Asset>& assets,
                                           const index::Catalog* catalog,
                                           const LintOptions& options) const {
  std::vector<LintIssue> issues;
  checkFlashFrames(doc, options, issues);
  checkUnintentionalGaps(doc, options, issues);
  checkMidWordCuts(doc, catalog, options, issues);
  checkJumpCuts(doc, options, issues);
  checkAudioClipping(doc, assets, options, issues);
  checkAudioAbruptEnd(doc, options, issues);
  checkCaptionSafeZone(doc, options, issues);
  checkMissingMedia(doc, assets, options, issues);
  return issues;
}

void LintEngine::checkFlashFrames(const TimelineDoc& doc, const LintOptions& opts, std::vector<LintIssue>& out) const {
  const std::int64_t fps = doc.project.fps > 0 ? doc.project.fps : 30;
  for (const auto& item : doc.items) {
    const std::int64_t durMs = (item.durationFrames * 1000) / fps;
    if (item.durationFrames < 3 || durMs < opts.flashFrameThresholdMs) {
      LintIssue iss;
      iss.id = QStringLiteral("lint-flash-%1").arg(item.id);
      iss.ruleId = QStringLiteral("flash_frame");
      iss.severity = LintSeverity::Warning;
      iss.trackId = item.trackId;
      iss.itemId = item.id;
      iss.startMs = (item.startFrame * 1000) / fps;
      iss.durationMs = durMs;
      iss.message = QStringLiteral("Flash frame: clip '%1' is only %2 frames (%3ms)")
                        .arg(item.id).arg(item.durationFrames).arg(durMs);
      iss.explanation = QStringLiteral("Accidental micro-clip remnant left after razor cut or ripple.");
      iss.autoFixAvailable = true;
      iss.fixOps.append(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("item.remove")},
        {QStringLiteral("itemIds"), QJsonArray{item.id}},
        {QStringLiteral("ripple"), false}
      });
      out.push_back(iss);
    }
  }
}

void LintEngine::checkUnintentionalGaps(const TimelineDoc& doc, const LintOptions& opts, std::vector<LintIssue>& out) const {
  const std::int64_t fps = doc.project.fps > 0 ? doc.project.fps : 30;

  // Find primary storyline track (first video track)
  QString primaryTrackId;
  for (const auto& track : doc.tracks) {
    if (track.kind == TrackKind::Video) {
      primaryTrackId = track.id;
      break;
    }
  }
  if (primaryTrackId.isEmpty()) return;

  std::vector<Item> trackItems;
  for (const auto& item : doc.items) {
    if (item.trackId == primaryTrackId) {
      trackItems.push_back(item);
    }
  }
  std::sort(trackItems.begin(), trackItems.end(), [](const Item& a, const Item& b) {
    return a.startFrame < b.startFrame;
  });

  for (size_t i = 0; i + 1 < trackItems.size(); ++i) {
    const auto& curr = trackItems[i];
    const auto& next = trackItems[i + 1];
    const Frame endCurr = curr.startFrame + curr.durationFrames;
    if (next.startFrame > endCurr) {
      const Frame gapFrames = next.startFrame - endCurr;
      const std::int64_t gapMs = (gapFrames * 1000) / fps;
      if (gapMs > 0 && gapMs <= opts.gapThresholdMs) {
        LintIssue iss;
        iss.id = QStringLiteral("lint-gap-%1-%2").arg(curr.id, next.id);
        iss.ruleId = QStringLiteral("unintentional_gap");
        iss.severity = LintSeverity::Info;
        iss.trackId = primaryTrackId;
        iss.itemId = next.id;
        iss.startMs = (endCurr * 1000) / fps;
        iss.durationMs = gapMs;
        iss.message = QStringLiteral("Unintentional gap: %1ms black frame gap between '%2' and '%3'")
                          .arg(gapMs).arg(curr.id).arg(next.id);
        iss.explanation = QStringLiteral("Accidental small gap on primary video storyline.");
        iss.autoFixAvailable = true;

        // Shift next and all following items left by gapFrames
        for (size_t j = i + 1; j < trackItems.size(); ++j) {
          iss.fixOps.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("item.move")},
            {QStringLiteral("itemId"), trackItems[j].id},
            {QStringLiteral("startFrame"), static_cast<qint64>(trackItems[j].startFrame - gapFrames)}
          });
        }
        out.push_back(iss);
      }
    }
  }
}

void LintEngine::checkMidWordCuts(const TimelineDoc& doc,
                                  const index::Catalog* catalog,
                                  const LintOptions& opts,
                                  std::vector<LintIssue>& out) const {
  const std::int64_t fps = doc.project.fps > 0 ? doc.project.fps : 30;

  for (const auto& item : doc.items) {
    if (!item.assetId || item.assetId->isEmpty()) continue;
    const QString assetId = *item.assetId;

    std::vector<index::SpeechSpan> spans;
    if (!opts.speechSpans.empty()) {
      for (const auto& sp : opts.speechSpans) {
        if (sp.assetId == assetId && sp.isSpeech) spans.push_back(sp);
      }
    } else if (catalog) {
      spans = catalog->speechSpans(assetId);
    }
    if (spans.empty()) continue;

    const Frame inFrame = item.sourceInFrame.value_or(0);
    const Frame outFrame = inFrame + static_cast<Frame>(item.durationFrames * item.speed);
    const double inSec = static_cast<double>(inFrame) / fps;
    const double outSec = static_cast<double>(outFrame) / fps;

    for (const auto& span : spans) {
      if (!span.isSpeech) continue;

      // In-point lands inside active speech (> 0.05s from boundary)
      if (inSec > span.startSec + 0.05 && inSec < span.endSec - 0.05) {
        LintIssue iss;
        iss.id = QStringLiteral("lint-speech-in-%1").arg(item.id);
        iss.ruleId = QStringLiteral("mid_word_cut");
        iss.severity = LintSeverity::Error;
        iss.trackId = item.trackId;
        iss.itemId = item.id;
        iss.startMs = (item.startFrame * 1000) / fps;
        iss.durationMs = 100;
        iss.message = QStringLiteral("Mid-word cut: head of '%1' cuts inside active speech at %2s")
                          .arg(item.id).arg(inSec, 0, 'f', 2);
        iss.explanation = QStringLiteral("Cut point lands inside a spoken word instead of a silence gap.");
        iss.autoFixAvailable = true;

        // Snap head to start of silence gap before speech
        const Frame snappedIn = static_cast<Frame>(std::max(0.0, span.startSec) * fps);
        iss.fixOps.append(QJsonObject{
          {QStringLiteral("type"), QStringLiteral("item.trim")},
          {QStringLiteral("itemId"), item.id},
          {QStringLiteral("edge"), QStringLiteral("in")},
          {QStringLiteral("frame"), static_cast<qint64>(snappedIn)},
          {QStringLiteral("ripple"), false}
        });
        out.push_back(iss);
        break;
      }

      // Out-point lands inside active speech
      if (outSec > span.startSec + 0.05 && outSec < span.endSec - 0.05) {
        LintIssue iss;
        iss.id = QStringLiteral("lint-speech-out-%1").arg(item.id);
        iss.ruleId = QStringLiteral("mid_word_cut");
        iss.severity = LintSeverity::Error;
        iss.trackId = item.trackId;
        iss.itemId = item.id;
        iss.startMs = ((item.startFrame + item.durationFrames) * 1000) / fps;
        iss.durationMs = 100;
        iss.message = QStringLiteral("Mid-word cut: tail of '%1' truncates active speech at %2s")
                          .arg(item.id).arg(outSec, 0, 'f', 2);
        iss.explanation = QStringLiteral("Cut point abruptly cuts off a spoken word.");
        iss.autoFixAvailable = true;

        // Snap tail to end of speech
        const Frame snappedOut = static_cast<Frame>(span.endSec * fps);
        iss.fixOps.append(QJsonObject{
          {QStringLiteral("type"), QStringLiteral("item.trim")},
          {QStringLiteral("itemId"), item.id},
          {QStringLiteral("edge"), QStringLiteral("out")},
          {QStringLiteral("frame"), static_cast<qint64>(snappedOut)},
          {QStringLiteral("ripple"), false}
        });
        out.push_back(iss);
        break;
      }
    }
  }
}

void LintEngine::checkJumpCuts(const TimelineDoc& doc, const LintOptions& opts, std::vector<LintIssue>& out) const {
  const std::int64_t fps = doc.project.fps > 0 ? doc.project.fps : 30;

  for (const auto& track : doc.tracks) {
    if (track.kind != TrackKind::Video) continue;

    std::vector<Item> items;
    for (const auto& it : doc.items) {
      if (it.trackId == track.id) items.push_back(it);
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
      return a.startFrame < b.startFrame;
    });

    for (size_t i = 0; i + 1 < items.size(); ++i) {
      const auto& a = items[i];
      const auto& b = items[i + 1];
      if (!a.assetId || !b.assetId || *a.assetId != *b.assetId) continue;

      const Frame aOut = a.sourceInFrame.value_or(0) + static_cast<Frame>(a.durationFrames * a.speed);
      const Frame bIn = b.sourceInFrame.value_or(0);
      const std::int64_t deltaMs = std::abs(static_cast<qint64>(bIn - aOut)) * 1000 / fps;

      if (deltaMs < opts.jumpCutWindowMs) {
        // Check if there is an overlapping clip on a different track covering the cut point
        const Frame cutPoint = a.startFrame + a.durationFrames;
        bool hasCutaway = false;
        for (const auto& other : doc.items) {
          if (other.trackId != track.id && other.type() == ItemType::Video) {
            if (other.startFrame <= cutPoint && (other.startFrame + other.durationFrames) >= cutPoint) {
              hasCutaway = true;
              break;
            }
          }
        }

        if (!hasCutaway) {
          LintIssue iss;
          iss.id = QStringLiteral("lint-jump-%1-%2").arg(a.id, b.id);
          iss.ruleId = QStringLiteral("jump_cut_same_angle");
          iss.severity = LintSeverity::Warning;
          iss.trackId = track.id;
          iss.itemId = b.id;
          iss.startMs = (cutPoint * 1000) / fps;
          iss.durationMs = (b.durationFrames * 1000) / fps;
          iss.message = QStringLiteral("Jump cut: adjacent clips from same take '%1' without cutaway").arg(*a.assetId);
          iss.explanation = QStringLiteral("Near-identical framing between cuts produces a jarring jump.");
          iss.autoFixAvailable = true;
          // Suggest inserting a crossfade transition or trimming
          out.push_back(iss);
        }
      }
    }
  }
}

void LintEngine::checkAudioClipping(const TimelineDoc& doc,
                                    const std::vector<Asset>&,
                                    const LintOptions&,
                                    std::vector<LintIssue>& out) const {
  const std::int64_t fps = doc.project.fps > 0 ? doc.project.fps : 30;

  double masterVol = 1.0;
  std::map<QString, double> stripVols;
  if (doc.mixer) {
    masterVol = doc.mixer->master.volume;
    for (const auto& strip : doc.mixer->strips) {
      stripVols[strip.id] = strip.volume;
    }
  }

  for (const auto& item : doc.items) {
    if (item.type() != ItemType::Audio && item.type() != ItemType::Video) continue;

    double stripVol = 1.0;
    if (auto it = stripVols.find(item.trackId); it != stripVols.end()) {
      stripVol = it->second;
    }

    const double compoundGain = item.volume * stripVol * masterVol;
    // Compound gain exceeding 1.0 (0 dBFS boost)
    if (compoundGain > 1.001) {
      LintIssue iss;
      iss.id = QStringLiteral("lint-clip-%1").arg(item.id);
      iss.ruleId = QStringLiteral("audio_clipping");
      iss.severity = LintSeverity::Error;
      iss.trackId = item.trackId;
      iss.itemId = item.id;
      iss.startMs = (item.startFrame * 1000) / fps;
      iss.durationMs = (item.durationFrames * 1000) / fps;
      const double dbGain = 20.0 * std::log10(compoundGain);
      iss.message = QStringLiteral("Audio clipping risk: compound track gain is +%1 dBFS").arg(dbGain, 0, 'f', 1);
      iss.explanation = QStringLiteral("Compound item and mixer volume exceeds safe 0 dBFS ceiling.");
      iss.autoFixAvailable = true;

      // Safe reduction: if item volume is boosted, reduce item volume to 1.0
      if (item.volume > 1.0) {
        QJsonObject patch{{QStringLiteral("volume"), 1.0}};
        iss.fixOps.append(QJsonObject{
          {QStringLiteral("type"), QStringLiteral("item.update")},
          {QStringLiteral("itemId"), item.id},
          {QStringLiteral("patch"), patch}
        });
      }
      out.push_back(iss);
    }
  }
}

void LintEngine::checkAudioAbruptEnd(const TimelineDoc& doc, const LintOptions&, std::vector<LintIssue>& out) const {
  const std::int64_t fps = doc.project.fps > 0 ? doc.project.fps : 30;

  for (const auto& item : doc.items) {
    if (item.type() != ItemType::Audio) continue;
    if (item.muted || item.volume < 0.5) continue;

    Frame fadeOut = 0;
    if (std::holds_alternative<AudioProps>(item.props)) {
      fadeOut = std::get<AudioProps>(item.props).fadeOutFrames;
    } else {
      continue;
    }

    if (fadeOut < 1) {
      LintIssue iss;
      iss.id = QStringLiteral("lint-abrupt-%1").arg(item.id);
      iss.ruleId = QStringLiteral("audio_abrupt_end");
      iss.severity = LintSeverity::Warning;
      iss.trackId = item.trackId;
      iss.itemId = item.id;
      iss.startMs = (item.startFrame * 1000) / fps;
      iss.durationMs = (item.durationFrames * 1000) / fps;
      iss.message = QStringLiteral("Abrupt audio cut: clip '%1' ends at high volume without fade-out").arg(item.id);
      iss.explanation = QStringLiteral("Lack of a micro-fade (>=20ms) can create audible clicks and pops at cut boundaries.");
      iss.autoFixAvailable = true;

      // Apply 2-frame fade-out (~66ms at 30fps)
      QJsonObject propPatch{{QStringLiteral("fadeOutFrames"), 2}};
      QJsonObject patch{{QStringLiteral("props"), propPatch}};
      iss.fixOps.append(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("item.update")},
        {QStringLiteral("itemId"), item.id},
        {QStringLiteral("patch"), patch}
      });
      out.push_back(iss);
    }
  }
}

void LintEngine::checkCaptionSafeZone(const TimelineDoc& doc, const LintOptions&, std::vector<LintIssue>& out) const {
  const std::int64_t fps = doc.project.fps > 0 ? doc.project.fps : 30;

  for (const auto& item : doc.items) {
    if (item.type() != ItemType::Caption) continue;
    if (!std::holds_alternative<CaptionProps>(item.props)) continue;

    const auto& cp = std::get<CaptionProps>(item.props);
    if (cp.style.placementY < 0.5) {
      LintIssue iss;
      iss.id = QStringLiteral("lint-caption-%1").arg(item.id);
      iss.ruleId = QStringLiteral("caption_safe_zone");
      iss.severity = LintSeverity::Warning;
      iss.trackId = item.trackId;
      iss.itemId = item.id;
      iss.startMs = (item.startFrame * 1000) / fps;
      iss.durationMs = (item.durationFrames * 1000) / fps;
      iss.message = QStringLiteral("Caption unsafe zone: caption '%1' placed in upper half (y=%2)")
                        .arg(item.id).arg(cp.style.placementY, 0, 'f', 2);
      iss.explanation = QStringLiteral("Captions placed in upper half risk occluding faces and focal elements.");
      iss.autoFixAvailable = true;

      QJsonObject stylePatch{{QStringLiteral("placementY"), 0.82}};
      QJsonObject propPatch{{QStringLiteral("style"), stylePatch}};
      QJsonObject patch{{QStringLiteral("props"), propPatch}};
      iss.fixOps.append(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("item.update")},
        {QStringLiteral("itemId"), item.id},
        {QStringLiteral("patch"), patch}
      });
      out.push_back(iss);
    }
  }
}

void LintEngine::checkMissingMedia(const TimelineDoc& doc,
                                   const std::vector<Asset>& assets,
                                   const LintOptions&,
                                   std::vector<LintIssue>& out) const {
  const std::int64_t fps = doc.project.fps > 0 ? doc.project.fps : 30;

  for (const auto& item : doc.items) {
    if (!item.assetId || item.assetId->isEmpty()) continue;
    const QString assetId = *item.assetId;

    bool found = false;
    bool broken = false;
    for (const auto& asset : assets) {
      if (asset.id == assetId) {
        found = true;
        if (asset.status == AssetStatus::Missing || asset.status == AssetStatus::Failed || asset.durationMs <= 0) {
          broken = true;
        }
        break;
      }
    }

    if (!found || broken) {
      LintIssue iss;
      iss.id = QStringLiteral("lint-missing-%1").arg(item.id);
      iss.ruleId = QStringLiteral("missing_media");
      iss.severity = LintSeverity::Error;
      iss.trackId = item.trackId;
      iss.itemId = item.id;
      iss.startMs = (item.startFrame * 1000) / fps;
      iss.durationMs = (item.durationFrames * 1000) / fps;
      iss.message = QStringLiteral("Missing media: item '%1' references offline or missing asset '%2'")
                        .arg(item.id, assetId);
      iss.explanation = QStringLiteral("The referenced asset file is missing or unlinked.");
      iss.autoFixAvailable = false;
      out.push_back(iss);
    }
  }
}

} // namespace sf::copilot
