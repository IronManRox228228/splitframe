#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>

namespace sf::agent {

// A single record in the agent trace log (agent-trace.jsonl).
struct AgentTraceRecord {
  QString timestamp;     // ISO 8601 UTC timestamp with ms
  QString runId;
  QString agentName;
  QString role;
  QString action;        // plan, tool_call, handoff, critique, retry, escalate, message, error
  QString inputSummary;
  QString outputSummary;
  QString targetAgent;
  QString tool;
  QString model;         // observed model id from the response, not requested
  QString status;        // success | needs_revision | error
  int retryCount = 0;
  QString error;

  QJsonObject toJson() const;
  static AgentTraceRecord fromJson(const QJsonObject& o);
};

// Resolves trace log file path from SF_AGENT_TRACE ("off" disables), default
// <project dir>/agent-trace.jsonl, else QStandardPaths::AppLocalDataLocation/agent-trace.jsonl.
QString resolveTracePath(const QString& projectDir = QString());

// Appends one sanitized JSON line to the resolved trace file.
void logAgentTrace(const AgentTraceRecord& record, const QString& projectDir = QString());

// Reads back the last N trace entries from the resolved trace file.
QJsonObject readAgentTrace(int limit = 50, const QString& projectDir = QString());

// Strips sensitive tokens and secrets before writing to trace.
QString sanitizeTraceString(const QString& str);

} // namespace sf::agent
