#include "copilot/types.h"

namespace sf::copilot {

QJsonObject LintIssue::toJson() const {
  QJsonObject o{
    {QStringLiteral("id"), id},
    {QStringLiteral("ruleId"), ruleId},
    {QStringLiteral("severity"), severityToString(severity)},
    {QStringLiteral("startMs"), static_cast<qint64>(startMs)},
    {QStringLiteral("durationMs"), static_cast<qint64>(durationMs)},
    {QStringLiteral("message"), message},
    {QStringLiteral("explanation"), explanation},
    {QStringLiteral("autoFixAvailable"), autoFixAvailable}
  };
  if (!trackId.isEmpty()) o.insert(QStringLiteral("trackId"), trackId);
  if (!itemId.isEmpty()) o.insert(QStringLiteral("itemId"), itemId);
  if (!fixOps.isEmpty()) {
    QJsonArray arr;
    for (const auto& op : fixOps) arr.append(op);
    o.insert(QStringLiteral("fixOps"), arr);
  }
  return o;
}

LintIssue LintIssue::fromJson(const QJsonObject& obj) {
  LintIssue issue;
  issue.id = obj.value(QStringLiteral("id")).toString();
  issue.ruleId = obj.value(QStringLiteral("ruleId")).toString();
  issue.severity = severityFromString(obj.value(QStringLiteral("severity")).toString());
  issue.trackId = obj.value(QStringLiteral("trackId")).toString();
  issue.itemId = obj.value(QStringLiteral("itemId")).toString();
  issue.startMs = obj.value(QStringLiteral("startMs")).toVariant().toLongLong();
  issue.durationMs = obj.value(QStringLiteral("durationMs")).toVariant().toLongLong();
  issue.message = obj.value(QStringLiteral("message")).toString();
  issue.explanation = obj.value(QStringLiteral("explanation")).toString();
  issue.autoFixAvailable = obj.value(QStringLiteral("autoFixAvailable")).toBool(false);
  const QJsonArray arr = obj.value(QStringLiteral("fixOps")).toArray();
  for (const auto& v : arr) {
    if (v.isObject()) issue.fixOps.append(v.toObject());
  }
  return issue;
}

QJsonObject Suggestion::toJson() const {
  QJsonObject o{
    {QStringLiteral("id"), id},
    {QStringLiteral("kind"), suggestionKindToString(kind)},
    {QStringLiteral("title"), title},
    {QStringLiteral("description"), description},
    {QStringLiteral("startMs"), static_cast<qint64>(startMs)},
    {QStringLiteral("durationMs"), static_cast<qint64>(durationMs)},
    {QStringLiteral("ghostKind"), ghostKind},
    {QStringLiteral("confidence"), confidence}
  };
  if (!targetTrackId.isEmpty()) o.insert(QStringLiteral("targetTrackId"), targetTrackId);
  if (!targetItemId.isEmpty()) o.insert(QStringLiteral("targetItemId"), targetItemId);
  if (!ops.isEmpty()) {
    QJsonArray arr;
    for (const auto& op : ops) arr.append(op);
    o.insert(QStringLiteral("ops"), arr);
  }
  return o;
}

Suggestion Suggestion::fromJson(const QJsonObject& obj) {
  Suggestion s;
  s.id = obj.value(QStringLiteral("id")).toString();
  s.kind = suggestionKindFromString(obj.value(QStringLiteral("kind")).toString());
  s.title = obj.value(QStringLiteral("title")).toString();
  s.description = obj.value(QStringLiteral("description")).toString();
  s.targetTrackId = obj.value(QStringLiteral("targetTrackId")).toString();
  s.targetItemId = obj.value(QStringLiteral("targetItemId")).toString();
  s.startMs = obj.value(QStringLiteral("startMs")).toVariant().toLongLong();
  s.durationMs = obj.value(QStringLiteral("durationMs")).toVariant().toLongLong();
  s.ghostKind = obj.value(QStringLiteral("ghostKind")).toString(QStringLiteral("clip"));
  s.confidence = static_cast<float>(obj.value(QStringLiteral("confidence")).toDouble(1.0));
  const QJsonArray arr = obj.value(QStringLiteral("ops")).toArray();
  for (const auto& v : arr) {
    if (v.isObject()) s.ops.append(v.toObject());
  }
  return s;
}

QJsonObject BranchDiff::toJson() const {
  QJsonObject o{
    {QStringLiteral("durationDeltaMs"), static_cast<qint64>(durationDeltaMs)},
    {QStringLiteral("addedItemIds"), QJsonArray::fromStringList(addedItemIds)},
    {QStringLiteral("removedItemIds"), QJsonArray::fromStringList(removedItemIds)},
    {QStringLiteral("modifiedItemIds"), QJsonArray::fromStringList(modifiedItemIds)},
    {QStringLiteral("trackChanges"), QJsonArray::fromStringList(trackChanges)}
  };
  return o;
}

QJsonObject ReviewResult::toJson() const {
  QJsonArray issuesArr;
  for (const auto& iss : issues) issuesArr.append(iss.toJson());

  QJsonArray fixesArr;
  for (const auto& op : autoFixOps) fixesArr.append(op);

  return QJsonObject{
    {QStringLiteral("passed"), passed},
    {QStringLiteral("score"), score},
    {QStringLiteral("issues"), issuesArr},
    {QStringLiteral("autoFixApplied"), autoFixApplied},
    {QStringLiteral("autoFixOps"), fixesArr},
    {QStringLiteral("summary"), summary}
  };
}

} // namespace sf::copilot
