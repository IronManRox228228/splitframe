#pragma once

#include "index/types.h"

#include <QSqlDatabase>
#include <QString>
#include <vector>

namespace sf::index {

class Catalog {
public:
  Catalog();
  ~Catalog();

  Catalog(const Catalog&) = delete;
  Catalog& operator=(const Catalog&) = delete;

  // Open database at filePath (or ":memory:" for in-memory catalog).
  bool open(const QString& dbPath = QStringLiteral(":memory:"));
  void close();
  bool isOpen() const;

  // Schema initialization
  bool initSchema();

  // Asset metadata
  bool insertAsset(const QString& id, const QString& path, int width, int height,
                   int fpsNum, int fpsDen, qint64 durationFrames,
                   int sampleRate, int channels);

  // Scenes
  bool insertScene(const SceneRecord& scene);
  bool insertScenes(const std::vector<SceneRecord>& scenes);
  std::vector<SceneRecord> scenes(const QString& assetId) const;

  // Speech / Silence spans
  bool insertSpeechSpan(const SpeechSpan& span);
  bool insertSpeechSpans(const std::vector<SpeechSpan>& spans);
  std::vector<SpeechSpan> speechSpans(const QString& assetId) const;

  // Transcripts
  bool insertTranscript(const TranscriptRecord& tr);
  bool insertTranscripts(const std::vector<TranscriptRecord>& trs);
  std::vector<TranscriptRecord> transcripts(const QString& assetId) const;

  // OCR
  bool insertOcrRecord(const OcrRecord& ocr);
  bool insertOcrRecords(const std::vector<OcrRecord>& records);
  std::vector<OcrRecord> ocrRecords(const QString& assetId, qint64 frame = -1) const;

  // VLM tags
  bool insertVlmTag(const VlmTag& tag);
  std::vector<VlmTag> vlmTags(const QString& assetId) const;

  // Lexical FTS5 Search across transcripts, OCR items, and scene descriptions
  std::vector<SearchResult> search(const QString& query, int limit = 20) const;

  // Clean / remove asset records
  bool clearAsset(const QString& assetId);

  // Connection name for Qt SQL
  const QString& connectionName() const { return connectionName_; }

private:
  QString connectionName_;
  bool open_ = false;

  QSqlDatabase db() const;
};

} // namespace sf::index
