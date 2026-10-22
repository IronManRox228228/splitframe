#pragma once

// The Meterless task router (meterless-power-prompt.md, exposed via agent.provider.route): a pure
// module with no fetch and no storage, testable alone.
//
// Meterless model ids are meterless/<class>.<disposition>. planRoute(task, attachments, options)
// picks the class from the task (never from whatever model is selected), the disposition from the
// user's single pick with per-task pins beating both, applies the attachment rule (only tasks whose
// output is text get reclassified), the meterless-accept rule, the 3d route rule and the fusion and
// cluster downgrade rules. Returns {id, class, disposition, route, source, downgrade} (plus "error"
// when a requested disposition is refused).

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <optional>

namespace sf::meterless_router {

using Attachments = QStringList; // "image" | "audio" | "video" | "pdf" | "document"

// Classes served by the meterless/3d endpoint instead of chat.
inline QStringList threeDClasses() { return {QStringLiteral("mesh"), QStringLiteral("texture")}; }

// The classes that never allow fusion (the official prompt's single-core list; the fallback for
// when GET /v1/meterless/modes does not answer).
inline QStringList fusionSingleCoreClasses() {
  return {QStringLiteral("video"), QStringLiteral("music"), QStringLiteral("speech"), QStringLiteral("embed"),
          QStringLiteral("mesh"), QStringLiteral("texture")};
}

inline QStringList perceptionLikeClasses() { return {QStringLiteral("vision"), QStringLiteral("perception"), QStringLiteral("transcribe"), QStringLiteral("embed")}; }

struct TaskRoute {
  QString className; // text, code, vision, judge, perception, image, video, speech, transcribe, music, embed, mesh, texture
  QString disposition;
  QString accept;    // meterless-accept value for the media this route produces ("" = none)
  bool threeD = false;
};

struct RouterOptions {
  // The user's single pick in Settings; SplitFrame's default is `fast` (the lead's scoping note).
  QString disposition = QStringLiteral("fast");
  // Per-task pins (beats the user pick for that task only), e.g. "still" → "meterless/image.local".
  std::optional<QJsonObject> pins;
  // Runtime state from GET /v1/meterless/modes when it answers: {"fusion":[allowed classes...],
  // "cluster":bool, "unavailable":["cluster"]}. Absent = the hardcoded fallback lists.
  std::optional<QJsonObject> runtimeModes;
};

// SplitFrame's real Meterless call sites (the lead's scoping notes) mapped to tasks:
// agent planning and tool-calling → text, generation-prompt and storyboard writing → text (deep),
// peer review → judge, frame grabs and screenshots → vision via the attachment rule.
TaskRoute taskRoute(const QString& task);

QJsonObject planRoute(const QString& task, const Attachments& attachments = {}, const RouterOptions& options = {});

// Renders the routing table for Settings: one row per call-site task, reflecting the default
// options (user disposition + pins). Read-only display data.
QJsonArray routingTable();

} // namespace sf::meterless_router
