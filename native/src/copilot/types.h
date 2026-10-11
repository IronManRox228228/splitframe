#pragma once

#include "core/schema.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <vector>

namespace sf::copilot {

enum class LintSeverity { Error, Warning, Info };

inline QString severityToString(LintSeverity s) {
  switch (s) {
    case LintSeverity::Error: return QStringLiteral("error");
    case LintSeverity::Warning: return QStringLiteral("warning");
    case LintSeverity::Info: return QStringLiteral("info");
  }
  return QStringLiteral("info");
}

inline LintSeverity severityFromString(const QString& s) {
  const QString l = s.toLower();
  if (l == QStringLiteral("error")) return LintSeverity::Error;
  if (l == QStringLiteral("warning")) return LintSeverity::Warning;
  return LintSeverity::Info;
}

struct LintIssue {
  QString id;                      // e.g. "lint-item1-flash-frame"
  QString ruleId;                  // e.g. "flash_frame"
  LintSeverity severity = LintSeverity::Warning;
  QString trackId;
  QString itemId;
  std::int64_t startMs = 0;
  std::int64_t durationMs = 0;
  QString message;                 // human-readable message
  QString explanation;             // rationale or context
  bool autoFixAvailable = false;
  QList<QJsonObject> fixOps;       // atomic ops ready to execute via Project::apply

  QJsonObject toJson() const;
  static LintIssue fromJson(const QJsonObject& obj);
};

enum class SuggestionKind { GhostClip, GhostTrim, GhostTransition, GhostOp };

inline QString suggestionKindToString(SuggestionKind k) {
  switch (k) {
    case SuggestionKind::GhostClip: return QStringLiteral("clip");
    case SuggestionKind::GhostTrim: return QStringLiteral("trim");
    case SuggestionKind::GhostTransition: return QStringLiteral("transition");
    case SuggestionKind::GhostOp: return QStringLiteral("op");
  }
  return QStringLiteral("op");
}

inline SuggestionKind suggestionKindFromString(const QString& s) {
  const QString l = s.toLower();
  if (l == QStringLiteral("clip")) return SuggestionKind::GhostClip;
  if (l == QStringLiteral("trim")) return SuggestionKind::GhostTrim;
  if (l == QStringLiteral("transition")) return SuggestionKind::GhostTransition;
  return SuggestionKind::GhostOp;
}

// Next-edit suggestion. Composes with the UI ghost overlay without carrying a duplicate live schema Item.
struct Suggestion {
  QString id;
  SuggestionKind kind = SuggestionKind::GhostOp;
  QString title;                   // e.g. "Tab: Ripple delete pause (0.45s)"
  QString description;             // detailed rationale
  QString targetTrackId;
  QString targetItemId;
  std::int64_t startMs = 0;
  std::int64_t durationMs = 0;
  QString ghostKind = QStringLiteral("clip"); // "clip", "trim", "transition", "gap"
  QList<QJsonObject> ops;          // applied when user presses Tab
  float confidence = 1.0f;

  QJsonObject toJson() const;
  static Suggestion fromJson(const QJsonObject& obj);
};

struct BranchDiff {
  std::int64_t durationDeltaMs = 0;
  QStringList addedItemIds;
  QStringList removedItemIds;
  QStringList modifiedItemIds;
  QStringList trackChanges;

  QJsonObject toJson() const;
};

struct ReviewResult {
  bool passed = true;
  double score = 1.0;              // 0.0 .. 1.0
  std::vector<LintIssue> issues;
  bool autoFixApplied = false;
  QList<QJsonObject> autoFixOps;
  QString summary;

  QJsonObject toJson() const;
};

} // namespace sf::copilot
