#pragma once

#include "copilot/types.h"
#include "core/schema.h"
#include "index/catalog.h"
#include "index/types.h"

#include <vector>

namespace sf::copilot {

struct LintOptions {
  std::int64_t flashFrameThresholdMs = 100;
  std::int64_t gapThresholdMs = 250;
  double clippingCeilingDb = 0.0;
  std::int64_t jumpCutWindowMs = 3000;
  // Pre-loaded speech spans for testing or offline checking
  std::vector<index::SpeechSpan> speechSpans;
};

class LintEngine {
public:
  explicit LintEngine() = default;

  std::vector<LintIssue> analyze(const TimelineDoc& doc,
                                 const std::vector<Asset>& assets,
                                 const index::Catalog* catalog = nullptr,
                                 const LintOptions& options = {}) const;

private:
  void checkFlashFrames(const TimelineDoc& doc, const LintOptions& opts, std::vector<LintIssue>& out) const;
  void checkUnintentionalGaps(const TimelineDoc& doc, const LintOptions& opts, std::vector<LintIssue>& out) const;
  void checkMidWordCuts(const TimelineDoc& doc, const index::Catalog* catalog, const LintOptions& opts, std::vector<LintIssue>& out) const;
  void checkJumpCuts(const TimelineDoc& doc, const LintOptions& opts, std::vector<LintIssue>& out) const;
  void checkAudioClipping(const TimelineDoc& doc, const std::vector<Asset>& assets, const LintOptions& opts, std::vector<LintIssue>& out) const;
  void checkAudioAbruptEnd(const TimelineDoc& doc, const LintOptions& opts, std::vector<LintIssue>& out) const;
  void checkCaptionSafeZone(const TimelineDoc& doc, const LintOptions& opts, std::vector<LintIssue>& out) const;
  void checkMissingMedia(const TimelineDoc& doc, const std::vector<Asset>& assets, const LintOptions& opts, std::vector<LintIssue>& out) const;
};

} // namespace sf::copilot
