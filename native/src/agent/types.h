#pragma once

// Common types for M5b: specialist kinds, agent modes, chat messages, and peer bus protocol.

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace sf::agent {

// Three specialists with strict domain ownership.
enum class SpecialistKind {
  Editor,    // Owns timeline structure: cuts, trims, speed, order, clips, captions, titles, canvas, export
  Colourist, // Owns colour grading, LUTs, colour spaces, OCIO
  Audio      // Owns audio tracks, music, ducking, volume/pan, audio effects, mixer, CLAP plugins, beats
};

inline QString specialistName(SpecialistKind k) {
  switch (k) {
    case SpecialistKind::Editor: return QStringLiteral("editor");
    case SpecialistKind::Colourist: return QStringLiteral("colourist");
    case SpecialistKind::Audio: return QStringLiteral("audio");
  }
  return QStringLiteral("unknown");
}

inline std::optional<SpecialistKind> parseSpecialistKind(const QString& s) {
  const QString l = s.trimmed().toLower();
  if (l == QStringLiteral("editor")) return SpecialistKind::Editor;
  if (l == QStringLiteral("colourist") || l == QStringLiteral("colorist") || l == QStringLiteral("colour") || l == QStringLiteral("color")) {
    return SpecialistKind::Colourist;
  }
  if (l == QStringLiteral("audio") || l == QStringLiteral("sound") || l == QStringLiteral("music")) {
    return SpecialistKind::Audio;
  }
  return std::nullopt;
}

// Agent modes controlling user approval requirements.
enum class AgentMode {
  Plan,    // Reads and plans only; changes nothing until plan is run
  Ask,     // Shows what each change will do and waits for Apply / Skip
  Default, // Small clear requests apply immediately; big jobs show a plan and wait
  Auto     // Runs the whole job without stopping; still verifies work
};

inline QString modeName(AgentMode m) {
  switch (m) {
    case AgentMode::Plan: return QStringLiteral("plan");
    case AgentMode::Ask: return QStringLiteral("ask");
    case AgentMode::Default: return QStringLiteral("default");
    case AgentMode::Auto: return QStringLiteral("auto");
  }
  return QStringLiteral("default");
}

inline std::optional<AgentMode> parseAgentMode(const QString& s) {
  const QString l = s.trimmed().toLower();
  if (l == QStringLiteral("plan")) return AgentMode::Plan;
  if (l == QStringLiteral("ask")) return AgentMode::Ask;
  if (l == QStringLiteral("default")) return AgentMode::Default;
  if (l == QStringLiteral("auto")) return AgentMode::Auto;
  return std::nullopt;
}

// In-memory chat message for specialist conversations.
struct ChatMessage {
  QString role;          // "system", "user", "assistant", "tool"
  QString content;
  QString name;          // optional: sender name or tool name
  QJsonArray toolCalls;  // optional tool calls in assistant responses
  QString toolCallId;    // for role == "tool"

  QJsonObject toJson() const {
    QJsonObject o{{QStringLiteral("role"), role}, {QStringLiteral("content"), content}};
    if (!name.isEmpty()) o.insert(QStringLiteral("name"), name);
    if (!toolCalls.isEmpty()) o.insert(QStringLiteral("tool_calls"), toolCalls);
    if (!toolCallId.isEmpty()) o.insert(QStringLiteral("tool_call_id"), toolCallId);
    return o;
  }

  static ChatMessage fromJson(const QJsonObject& o) {
    ChatMessage m;
    m.role = o.value(QStringLiteral("role")).toString();
    m.content = o.value(QStringLiteral("content")).toString();
    m.name = o.value(QStringLiteral("name")).toString();
    m.toolCalls = o.value(QStringLiteral("tool_calls")).toArray();
    m.toolCallId = o.value(QStringLiteral("tool_call_id")).toString();
    return m;
  }
};

// Peer request sent from one specialist to another across the Peer Bus.
struct PeerRequest {
  QString id;
  SpecialistKind from;
  SpecialistKind to;
  QString kind;            // e.g. "fit_to_beats", "duck_music", "match_shots", "grade_clip", "cut_timeline"
  QJsonObject args;        // parameters for the action
  QJsonObject constraints; // e.g. maxSpeedDelta, preserveAudio, targetLufs
  QString reason;          // <= 1 line summary of why this request is made
  bool await = true;       // blocking or async
  int depth = 0;           // chain depth guard (max 3)

  QJsonObject toJson() const {
    return QJsonObject{
      {QStringLiteral("id"), id},
      {QStringLiteral("from"), specialistName(from)},
      {QStringLiteral("to"), specialistName(to)},
      {QStringLiteral("kind"), kind},
      {QStringLiteral("args"), args},
      {QStringLiteral("constraints"), constraints},
      {QStringLiteral("reason"), reason},
      {QStringLiteral("await"), await},
      {QStringLiteral("depth"), depth}
    };
  }
};

// Peer response returned from recipient specialist.
struct PeerResponse {
  QString requestId;
  QString status;          // "done" | "declined" | "counter"
  QJsonArray ops;          // operations proposed or performed
  QJsonObject outputs;     // structured outputs (e.g. cut frames, applied values)
  QString note;            // <= 1 line summary of response or rejection reason

  QJsonObject toJson() const {
    return QJsonObject{
      {QStringLiteral("requestId"), requestId},
      {QStringLiteral("status"), status},
      {QStringLiteral("ops"), ops},
      {QStringLiteral("outputs"), outputs},
      {QStringLiteral("note"), note}
    };
  }
};

// Log entry for inter-agent messages in the team thread.
struct PeerLogEntry {
  qint64 timestampMs = 0;
  QString requestId;
  SpecialistKind from;
  SpecialistKind to;
  QString kind;
  QString reason;
  QJsonObject args;
  QString status;
  QString note;
  QJsonObject outputs;

  QJsonObject toJson() const {
    return QJsonObject{
      {QStringLiteral("timestampMs"), timestampMs},
      {QStringLiteral("requestId"), requestId},
      {QStringLiteral("from"), specialistName(from)},
      {QStringLiteral("to"), specialistName(to)},
      {QStringLiteral("kind"), kind},
      {QStringLiteral("reason"), reason},
      {QStringLiteral("args"), args},
      {QStringLiteral("status"), status},
      {QStringLiteral("note"), note},
      {QStringLiteral("outputs"), outputs}
    };
  }
};

} // namespace sf::agent
