#include "index/types.h"

namespace sf::index {

QJsonObject SceneRecord::toJson() const {
  return QJsonObject{
    {QStringLiteral("id"), id},
    {QStringLiteral("assetId"), assetId},
    {QStringLiteral("startFrame"), startFrame},
    {QStringLiteral("endFrame"), endFrame},
    {QStringLiteral("startSec"), startSec},
    {QStringLiteral("endSec"), endSec},
    {QStringLiteral("sharpness"), sharpness},
    {QStringLiteral("lumaAvg"), lumaAvg},
    {QStringLiteral("lumaClippedPct"), lumaClippedPct},
    {QStringLiteral("isFreeze"), isFreeze},
    {QStringLiteral("isBlack"), isBlack},
    {QStringLiteral("motionScore"), motionScore},
    {QStringLiteral("thumbnailPath"), thumbnailPath}
  };
}

SceneRecord SceneRecord::fromJson(const QJsonObject& obj) {
  SceneRecord r;
  r.id = obj.value(QStringLiteral("id")).toString();
  r.assetId = obj.value(QStringLiteral("assetId")).toString();
  r.startFrame = obj.value(QStringLiteral("startFrame")).toInteger();
  r.endFrame = obj.value(QStringLiteral("endFrame")).toInteger();
  r.startSec = obj.value(QStringLiteral("startSec")).toDouble();
  r.endSec = obj.value(QStringLiteral("endSec")).toDouble();
  r.sharpness = obj.value(QStringLiteral("sharpness")).toDouble();
  r.lumaAvg = obj.value(QStringLiteral("lumaAvg")).toDouble();
  r.lumaClippedPct = obj.value(QStringLiteral("lumaClippedPct")).toInt();
  r.isFreeze = obj.value(QStringLiteral("isFreeze")).toBool();
  r.isBlack = obj.value(QStringLiteral("isBlack")).toBool();
  r.motionScore = obj.value(QStringLiteral("motionScore")).toDouble();
  r.thumbnailPath = obj.value(QStringLiteral("thumbnailPath")).toString();
  return r;
}

QJsonObject SpeechSpan::toJson() const {
  return QJsonObject{
    {QStringLiteral("id"), id},
    {QStringLiteral("assetId"), assetId},
    {QStringLiteral("startFrame"), startFrame},
    {QStringLiteral("endFrame"), endFrame},
    {QStringLiteral("startSec"), startSec},
    {QStringLiteral("endSec"), endSec},
    {QStringLiteral("isSpeech"), isSpeech},
    {QStringLiteral("meanVolumeDb"), meanVolumeDb},
    {QStringLiteral("maxVolumeDb"), maxVolumeDb}
  };
}

SpeechSpan SpeechSpan::fromJson(const QJsonObject& obj) {
  SpeechSpan r;
  r.id = obj.value(QStringLiteral("id")).toString();
  r.assetId = obj.value(QStringLiteral("assetId")).toString();
  r.startFrame = obj.value(QStringLiteral("startFrame")).toInteger();
  r.endFrame = obj.value(QStringLiteral("endFrame")).toInteger();
  r.startSec = obj.value(QStringLiteral("startSec")).toDouble();
  r.endSec = obj.value(QStringLiteral("endSec")).toDouble();
  r.isSpeech = obj.value(QStringLiteral("isSpeech")).toBool();
  r.meanVolumeDb = obj.value(QStringLiteral("meanVolumeDb")).toDouble();
  r.maxVolumeDb = obj.value(QStringLiteral("maxVolumeDb")).toDouble();
  return r;
}

QJsonObject TranscriptRecord::toJson() const {
  return QJsonObject{
    {QStringLiteral("id"), id},
    {QStringLiteral("assetId"), assetId},
    {QStringLiteral("startMs"), startMs},
    {QStringLiteral("endMs"), endMs},
    {QStringLiteral("text"), text},
    {QStringLiteral("speaker"), speaker},
    {QStringLiteral("confidence"), confidence}
  };
}

TranscriptRecord TranscriptRecord::fromJson(const QJsonObject& obj) {
  TranscriptRecord r;
  r.id = obj.value(QStringLiteral("id")).toString();
  r.assetId = obj.value(QStringLiteral("assetId")).toString();
  r.startMs = obj.value(QStringLiteral("startMs")).toInteger();
  r.endMs = obj.value(QStringLiteral("endMs")).toInteger();
  r.text = obj.value(QStringLiteral("text")).toString();
  r.speaker = obj.value(QStringLiteral("speaker")).toString();
  r.confidence = obj.value(QStringLiteral("confidence")).toDouble();
  return r;
}

QJsonObject OcrRecord::toJson() const {
  return QJsonObject{
    {QStringLiteral("id"), id},
    {QStringLiteral("assetId"), assetId},
    {QStringLiteral("frame"), frame},
    {QStringLiteral("timeSec"), timeSec},
    {QStringLiteral("text"), text},
    {QStringLiteral("x"), x},
    {QStringLiteral("y"), y},
    {QStringLiteral("w"), w},
    {QStringLiteral("h"), h},
    {QStringLiteral("confidence"), confidence}
  };
}

OcrRecord OcrRecord::fromJson(const QJsonObject& obj) {
  OcrRecord r;
  r.id = obj.value(QStringLiteral("id")).toString();
  r.assetId = obj.value(QStringLiteral("assetId")).toString();
  r.frame = obj.value(QStringLiteral("frame")).toInteger();
  r.timeSec = obj.value(QStringLiteral("timeSec")).toDouble();
  r.text = obj.value(QStringLiteral("text")).toString();
  r.x = obj.value(QStringLiteral("x")).toDouble();
  r.y = obj.value(QStringLiteral("y")).toDouble();
  r.w = obj.value(QStringLiteral("w")).toDouble();
  r.h = obj.value(QStringLiteral("h")).toDouble();
  r.confidence = obj.value(QStringLiteral("confidence")).toDouble();
  return r;
}

QJsonObject VlmTag::toJson() const {
  return QJsonObject{
    {QStringLiteral("id"), id},
    {QStringLiteral("assetId"), assetId},
    {QStringLiteral("sceneId"), sceneId},
    {QStringLiteral("summary"), summary},
    {QStringLiteral("shotType"), shotType},
    {QStringLiteral("subject"), subject}
  };
}

VlmTag VlmTag::fromJson(const QJsonObject& obj) {
  VlmTag r;
  r.id = obj.value(QStringLiteral("id")).toString();
  r.assetId = obj.value(QStringLiteral("assetId")).toString();
  r.sceneId = obj.value(QStringLiteral("sceneId")).toString();
  r.summary = obj.value(QStringLiteral("summary")).toString();
  r.shotType = obj.value(QStringLiteral("shotType")).toString();
  r.subject = obj.value(QStringLiteral("subject")).toString();
  return r;
}

QJsonObject SearchResult::toJson() const {
  return QJsonObject{
    {QStringLiteral("assetId"), assetId},
    {QStringLiteral("itemType"), itemType},
    {QStringLiteral("refId"), refId},
    {QStringLiteral("timeSec"), timeSec},
    {QStringLiteral("frame"), frame},
    {QStringLiteral("snippet"), snippet},
    {QStringLiteral("rank"), rank}
  };
}

} // namespace sf::index
