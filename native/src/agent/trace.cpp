#include "agent/trace.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtGlobal>

namespace sf::agent {

QString sanitizeTraceString(const QString& str) {
  if (str.isEmpty()) return str;
  QString out = str;

  // Strip explicit API keys from known environment variables if set
  const QString envKey = qEnvironmentVariable("SF_LLM_API_KEY").trimmed();
  if (!envKey.isEmpty()) {
    out.replace(envKey, QStringLiteral("[REDACTED]"));
  }

  // Strip Bearer tokens
  static const QRegularExpression bearerRe(QStringLiteral("Bearer\\s+[^\\s\"'}{,]+"), QRegularExpression::CaseInsensitiveOption);
  out.replace(bearerRe, QStringLiteral("Bearer [REDACTED]"));

  // Strip JSON-style token/key pairs: "token": "...", "apiKey": "...", etc.
  static const QRegularExpression jsonSecretRe(
    QStringLiteral("(\"(?:api_?key|token|secret|password|auth|runtime_token)\"\\s*:\\s*\")[^\"]+(\")"),
    QRegularExpression::CaseInsensitiveOption);
  out.replace(jsonSecretRe, QStringLiteral("\\1[REDACTED]\\2"));

  // Strip common token patterns (e.g., sk-..., tok-...)
  static const QRegularExpression skRe(QStringLiteral("\\b(?:sk|tok)-[a-zA-Z0-9_-]{8,}\\b"));
  out.replace(skRe, QStringLiteral("[REDACTED]"));

  return out;
}

static QString normalizeStatus(const QString& s) {
  const QString lower = s.trimmed().toLower();
  if (lower == QStringLiteral("needs_revision") || lower == QStringLiteral("revision")) {
    return QStringLiteral("needs_revision");
  }
  if (lower == QStringLiteral("error") || lower == QStringLiteral("failed")) {
    return QStringLiteral("error");
  }
  return QStringLiteral("success");
}

QJsonObject AgentTraceRecord::toJson() const {
  const QString ts = timestamp.isEmpty() ? QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) : timestamp;
  return QJsonObject{
    {QStringLiteral("timestamp"), sanitizeTraceString(ts)},
    {QStringLiteral("run_id"), sanitizeTraceString(runId)},
    {QStringLiteral("agent_name"), sanitizeTraceString(agentName)},
    {QStringLiteral("role"), sanitizeTraceString(role)},
    {QStringLiteral("lane"), sanitizeTraceString(lane.isEmpty() ? QStringLiteral("fast") : lane)},
    {QStringLiteral("action"), sanitizeTraceString(action)},
    {QStringLiteral("input_summary"), sanitizeTraceString(inputSummary)},
    {QStringLiteral("output_summary"), sanitizeTraceString(outputSummary)},
    {QStringLiteral("target_agent"), sanitizeTraceString(targetAgent)},
    {QStringLiteral("tool"), sanitizeTraceString(tool)},
    {QStringLiteral("model"), sanitizeTraceString(model)},
    {QStringLiteral("status"), normalizeStatus(status)},
    {QStringLiteral("retry_count"), retryCount},
    {QStringLiteral("error"), sanitizeTraceString(error)},
    {QStringLiteral("service"), sanitizeTraceString(service.isEmpty() ? QStringLiteral("none") : service)}
  };
}

AgentTraceRecord AgentTraceRecord::fromJson(const QJsonObject& o) {
  AgentTraceRecord r;
  r.timestamp = o.value(QStringLiteral("timestamp")).toString();
  r.runId = o.value(QStringLiteral("run_id")).toString();
  r.agentName = o.value(QStringLiteral("agent_name")).toString();
  r.role = o.value(QStringLiteral("role")).toString();
  r.lane = o.value(QStringLiteral("lane")).toString();
  r.action = o.value(QStringLiteral("action")).toString();
  r.inputSummary = o.value(QStringLiteral("input_summary")).toString();
  r.outputSummary = o.value(QStringLiteral("output_summary")).toString();
  r.targetAgent = o.value(QStringLiteral("target_agent")).toString();
  r.tool = o.value(QStringLiteral("tool")).toString();
  r.model = o.value(QStringLiteral("model")).toString();
  r.status = o.value(QStringLiteral("status")).toString();
  r.retryCount = o.value(QStringLiteral("retry_count")).toInt();
  r.error = o.value(QStringLiteral("error")).toString();
  r.service = o.value(QStringLiteral("service")).toString();
  return r;
}

QString resolveTracePath(const QString& projectDir) {
  const QString overridePath = qEnvironmentVariable("SF_AGENT_TRACE").trimmed();
  if (!overridePath.isEmpty()) {
    if (overridePath.compare(QLatin1String("off"), Qt::CaseInsensitive) == 0) {
      return QString();
    }
    return overridePath;
  }

  if (!projectDir.isEmpty()) {
    return QDir(projectDir).filePath(QStringLiteral("agent-trace.jsonl"));
  }

  const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  if (appData.isEmpty()) return QString();
  return QDir(appData).filePath(QStringLiteral("agent-trace.jsonl"));
}

void logAgentTrace(const AgentTraceRecord& record, const QString& projectDir) {
  const QString path = resolveTracePath(projectDir);
  if (path.isEmpty()) return;

  const QFileInfo fi(path);
  QDir().mkpath(fi.absolutePath());

  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;

  f.write(QJsonDocument(record.toJson()).toJson(QJsonDocument::Compact));
  f.write("\n");
}

QJsonObject readAgentTrace(int limit, const QString& projectDir) {
  const QString path = resolveTracePath(projectDir);
  QJsonObject out{
    {QStringLiteral("ok"), true},
    {QStringLiteral("path"), path}
  };

  if (path.isEmpty()) {
    out.insert(QStringLiteral("disabled"), true);
    out.insert(QStringLiteral("entries"), QJsonArray());
    out.insert(QStringLiteral("lines"), QJsonArray());
    out.insert(QStringLiteral("count"), 0);
    return out;
  }

  QFile f(path);
  if (!f.exists() || !f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    out.insert(QStringLiteral("entries"), QJsonArray());
    out.insert(QStringLiteral("lines"), QJsonArray());
    out.insert(QStringLiteral("count"), 0);
    return out;
  }

  QByteArrayList lines;
  while (!f.atEnd()) {
    QByteArray line = f.readLine().trimmed();
    if (!line.isEmpty()) lines.append(line);
  }
  f.close();

  if (limit <= 0) limit = 50;
  int start = 0;
  if (lines.size() > limit) {
    start = lines.size() - limit;
  }

  QJsonArray entries;
  for (int i = start; i < lines.size(); ++i) {
    QJsonDocument doc = QJsonDocument::fromJson(lines.at(i));
    if (doc.isObject()) {
      entries.append(doc.object());
    } else {
      entries.append(QString::fromUtf8(lines.at(i)));
    }
  }

  out.insert(QStringLiteral("entries"), entries);
  out.insert(QStringLiteral("lines"), entries);
  out.insert(QStringLiteral("count"), entries.size());
  return out;
}

} // namespace sf::agent
