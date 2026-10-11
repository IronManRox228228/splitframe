#include "copilot/review_engine.h"

#include <algorithm>

namespace sf::copilot {

ReviewResult ReviewEngine::reviewTimeline(const TimelineDoc& doc,
                                          const std::vector<Asset>& assets,
                                          const index::Catalog* catalog,
                                          const ReviewOptions& options) const {
  ReviewResult res;

  LintOptions lOpts;
  lOpts.speechSpans = options.speechSpans;

  res.issues = lintEngine_.analyze(doc, assets, catalog, lOpts);

  int errorCount = 0;
  int warningCount = 0;
  for (const auto& iss : res.issues) {
    if (iss.severity == LintSeverity::Error) errorCount++;
    else if (iss.severity == LintSeverity::Warning) warningCount++;

    if (options.applySafeFixes && iss.autoFixAvailable && !iss.fixOps.isEmpty()) {
      // Collect safe fixes (fade-out, volume normalization, gap close, safe zone)
      for (const auto& op : iss.fixOps) {
        res.autoFixOps.append(op);
      }
    }
  }

  if (options.applySafeFixes && !res.autoFixOps.isEmpty()) {
    res.autoFixApplied = true;
  }

  // Calculate quality score
  double penalty = (errorCount * 0.35) + (warningCount * 0.10);
  res.score = std::max(0.0, std::min(1.0, 1.0 - penalty));
  res.passed = (errorCount == 0 && res.score >= options.passThreshold);

  if (res.passed) {
    res.summary = QStringLiteral("Self-review passed (score: %1/1.0, %2 warnings). Cut is technically clean.")
                      .arg(res.score, 0, 'f', 2).arg(warningCount);
  } else {
    res.summary = QStringLiteral("Self-review flagged %1 errors and %2 warnings (score: %3/1.0). Requires refinement.")
                      .arg(errorCount).arg(warningCount).arg(res.score, 0, 'f', 2);
  }

  return res;
}

} // namespace sf::copilot
