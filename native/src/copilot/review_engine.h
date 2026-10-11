#pragma once

#include "copilot/lint_engine.h"
#include "copilot/types.h"
#include "core/schema.h"
#include "index/catalog.h"

#include <vector>

namespace sf::copilot {

struct ReviewOptions {
  bool applySafeFixes = false;     // whether to automatically bundle safe auto-fixes
  double passThreshold = 0.70;     // minimum score to pass
  std::vector<index::SpeechSpan> speechSpans; // optional mock or preloaded spans
};

class ReviewEngine {
public:
  explicit ReviewEngine() = default;

  ReviewResult reviewTimeline(const TimelineDoc& doc,
                              const std::vector<Asset>& assets,
                              const index::Catalog* catalog = nullptr,
                              const ReviewOptions& options = {}) const;

private:
  LintEngine lintEngine_;
};

} // namespace sf::copilot
