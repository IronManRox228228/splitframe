#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <vector>

namespace sf::index {

struct SceneRecord {
  QString id;
  QString assetId;
  qint64 startFrame = 0;
  qint64 endFrame = 0;
  double startSec = 0.0;
  double endSec = 0.0;
  double sharpness = 0.0;       // Laplacian variance: higher = sharper focus
  double lumaAvg = 0.0;         // 0..255 average luminance
  int lumaClippedPct = 0;       // Percentage of clipped highlights (>245) or crushed blacks (<10)
  bool isFreeze = false;        // Sustained static/frozen frame
  bool isBlack = false;         // Sustained black frame
  double motionScore = 0.0;     // Mean absolute frame difference: higher = more movement/shake
  QString thumbnailPath;

  QJsonObject toJson() const;
  static SceneRecord fromJson(const QJsonObject& obj);
};

struct SpeechSpan {
  QString id;
  QString assetId;
  qint64 startFrame = 0;
  qint64 endFrame = 0;
  double startSec = 0.0;
  double endSec = 0.0;
  bool isSpeech = true;         // true = speech/dialogue; false = silence/pause
  double meanVolumeDb = -100.0; // RMS volume in dBFS
  double maxVolumeDb = -100.0;

  QJsonObject toJson() const;
  static SpeechSpan fromJson(const QJsonObject& obj);
};

struct TranscriptRecord {
  QString id;
  QString assetId;
  qint64 startMs = 0;
  qint64 endMs = 0;
  QString text;
  QString speaker;
  double confidence = 1.0;

  QJsonObject toJson() const;
  static TranscriptRecord fromJson(const QJsonObject& obj);
};

struct OcrRecord {
  QString id;
  QString assetId;
  qint64 frame = 0;
  double timeSec = 0.0;
  QString text;
  double x = 0.0;               // Normalized bounding box 0.0 .. 1.0
  double y = 0.0;
  double w = 0.0;
  double h = 0.0;
  double confidence = 1.0;

  QJsonObject toJson() const;
  static OcrRecord fromJson(const QJsonObject& obj);
};

struct VlmTag {
  QString id;
  QString assetId;
  QString sceneId;
  QString summary;              // e.g. "Wide shot of software engineer typing at laptop terminal"
  QString shotType;             // "wide", "medium", "close-up"
  QString subject;              // e.g. "laptop", "person", "presentation"

  QJsonObject toJson() const;
  static VlmTag fromJson(const QJsonObject& obj);
};

struct SearchResult {
  QString assetId;
  QString itemType;             // "transcript", "ocr", "vlm", "scene"
  QString refId;
  double timeSec = 0.0;
  qint64 frame = 0;
  QString snippet;              // Highlighted text snippet
  double rank = 0.0;            // BM25 match score

  QJsonObject toJson() const;
};

struct AnalysisOptions {
  bool detectScenes = true;
  bool detectAudio = true;
  bool ocrKeyframes = true;
  bool vlmSummarize = false;     // Opt-in to avoid VRAM contention / LLM usage unless requested
  double sceneThreshold = 0.35;  // Cut sensitivity (0.0..1.0)
  double silenceThresholdDb = -40.0; // dBFS noise floor
  double minSilenceDurationSec = 0.3; // Min duration to flag a pause
  int ocrSampleIntervalSec = 3;  // Run OCR on keyframe + every N seconds
  QString cacheDir;              // Where to save thumbnails and index.db
};

} // namespace sf::index
