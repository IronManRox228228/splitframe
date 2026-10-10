#pragma once

#include "index/audio_analyzer.h"
#include "index/catalog.h"
#include "index/ocr_bridge.h"
#include "index/types.h"
#include "index/video_analyzer.h"
#include "index/vlm_descriptor.h"

#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <memory>

namespace sf::index {

class IndexManager : public QObject {
  Q_OBJECT

public:
  explicit IndexManager(QObject* parent = nullptr);
  ~IndexManager() override;

  IndexManager(const IndexManager&) = delete;
  IndexManager& operator=(const IndexManager&) = delete;

  // Open / initialize persistent or in-memory catalog
  bool openCatalog(const QString& dbPath = QStringLiteral(":memory:"));
  void closeCatalog();
  Catalog* catalog() { return &catalog_; }

  // Set shared LLM client for VLM descriptors
  void setLlmClient(std::shared_ptr<agent::LlmClient> llm);

  // Synchronous analysis (ideal for worker threads or CLI tests)
  bool analyzeAssetSync(const QString& assetId,
                        const QString& filePath,
                        const AnalysisOptions& opts = {});

  // Asynchronous analysis (runs in thread pool, emits signals)
  void analyzeAssetAsync(const QString& assetId,
                         const QString& filePath,
                         const AnalysisOptions& opts = {});

  bool isAnalyzing(const QString& assetId) const;
  void cancelAsset(const QString& assetId);

  // Query API
  std::vector<SearchResult> search(const QString& query, int limit = 20) const;
  std::vector<SceneRecord> scenes(const QString& assetId) const;
  std::vector<SpeechSpan> speechSpans(const QString& assetId) const;
  std::vector<OcrRecord> ocrRecords(const QString& assetId, qint64 frame = -1) const;
  std::vector<VlmTag> vlmTags(const QString& assetId) const;
  std::vector<TranscriptRecord> transcripts(const QString& assetId) const;

signals:
  void analysisStarted(const QString& assetId);
  void analysisProgress(const QString& assetId, double fraction, const QString& stage);
  void analysisFinished(const QString& assetId);
  void analysisFailed(const QString& assetId, const QString& error);

private:
  mutable QMutex mutex_;
  Catalog catalog_;
  VideoAnalyzer videoAnalyzer_;
  AudioAnalyzer audioAnalyzer_;
  OcrBridge ocrBridge_;
  VlmDescriptor vlmDescriptor_;
  QThreadPool threadPool_;
  QSet<QString> activeAssets_;
  QSet<QString> cancelledAssets_;
};

} // namespace sf::index
