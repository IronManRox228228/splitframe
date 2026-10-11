#pragma once

#include "copilot/types.h"
#include "core/schema.h"
#include "editor/project.h"
#include "index/catalog.h"
#include "index/types.h"

#include <QObject>
#include <optional>
#include <vector>

namespace sf::copilot {

class SuggestionEngine : public QObject {
  Q_OBJECT

public:
  explicit SuggestionEngine(QObject* parent = nullptr);

  std::optional<Suggestion> currentSuggestion() const { return current_; }
  void setSuggestion(const Suggestion& s);
  void dismiss();
  bool accept(editor::Project* project);

  // ---- Tier 1: Deterministic fast heuristics ----
  std::optional<Suggestion> detectPauseAfterSplit(const TimelineDoc& doc,
                                                 const QString& itemId,
                                                 const std::vector<index::SpeechSpan>& spans,
                                                 qint64 minPauseMs = 250);

  std::optional<Suggestion> detectLcutAudioExtension(const TimelineDoc& doc,
                                                    const QString& outgoingItemId,
                                                    qint64 leadMs = 250);

  std::optional<Suggestion> detectBeatAlignment(const TimelineDoc& doc,
                                               const QString& itemId,
                                               bool inEdge,
                                               const std::vector<double>& beatsMs,
                                               qint64 thresholdMs = 80);

  std::optional<Suggestion> detectBrollOverlay(const TimelineDoc& doc,
                                              const QString& primaryItemId,
                                              const QString& brollAssetId,
                                              const QString& overlayTrackId,
                                              std::int64_t brollDurationFrames = 90);

signals:
  void suggestionAvailable(const Suggestion& s);
  void suggestionDismissed();
  void suggestionAccepted(const Suggestion& s);

private:
  std::optional<Suggestion> current_;
};

} // namespace sf::copilot
