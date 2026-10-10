#include "index/index_manager.h"

#include "media/probe.h"

#include <QMetaObject>
#include <QRunnable>
#include <QThread>
#include <cmath>

namespace sf::index {

IndexManager::IndexManager(QObject* parent)
  : QObject(parent) {
  threadPool_.setMaxThreadCount(4);
}

IndexManager::~IndexManager() {
  threadPool_.waitForDone();
  closeCatalog();
}

bool IndexManager::openCatalog(const QString& dbPath) {
  QMutexLocker lock(&mutex_);
  return catalog_.open(dbPath);
}

void IndexManager::closeCatalog() {
  QMutexLocker lock(&mutex_);
  catalog_.close();
}

void IndexManager::setLlmClient(std::shared_ptr<agent::LlmClient> llm) {
  QMutexLocker lock(&mutex_);
  vlmDescriptor_.setLlmClient(std::move(llm));
}

bool IndexManager::analyzeAssetSync(const QString& assetId,
                                    const QString& filePath,
                                    const AnalysisOptions& opts) {
  {
    QMutexLocker lock(&mutex_);
    activeAssets_.insert(assetId);
    cancelledAssets_.remove(assetId);
  }
  emit analysisStarted(assetId);

  QString probeErr;
  const auto info = sf::probeMedia(filePath, &probeErr);
  if (!info) {
    {
      QMutexLocker lock(&mutex_);
      activeAssets_.remove(assetId);
    }
    emit analysisFailed(assetId, QStringLiteral("Probe failed: %1").arg(probeErr));
    return false;
  }

  // Helper to ensure catalog updates execute on the IndexManager's owner thread
  auto runOnOwnerThread = [this](auto&& func) {
    if (QThread::currentThread() == this->thread()) {
      func();
    } else {
      QMetaObject::invokeMethod(this, func, Qt::BlockingQueuedConnection);
    }
  };

  // 1. Insert asset entry
  const int fpsNum = static_cast<int>(std::round(info->fps * 1000.0));
  constexpr int fpsDen = 1000;
  const qint64 totalFrames = (info->frameCount > 0)
    ? info->frameCount
    : static_cast<qint64>(std::round((info->durationMs / 1000.0) * info->fps));

  runOnOwnerThread([&]() {
    catalog_.insertAsset(assetId, filePath, info->width, info->height,
                         fpsNum, fpsDen, totalFrames,
                         info->audioSampleRate, info->audioChannels);
  });

  // 2. Deterministic video scene analysis
  std::vector<SceneRecord> scenes;
  if (opts.detectScenes && info->hasVideo) {
    emit analysisProgress(assetId, 0.1, QStringLiteral("Detecting scenes and focus"));
    scenes = videoAnalyzer_.analyzeVideo(filePath, assetId, opts, [&](double frac) {
      emit analysisProgress(assetId, 0.1 + (frac * 0.4), QStringLiteral("Analyzing video frames"));
    });

    {
      QMutexLocker lock(&mutex_);
      if (cancelledAssets_.contains(assetId)) {
        activeAssets_.remove(assetId);
        return false;
      }
    }
    runOnOwnerThread([&]() {
      catalog_.insertScenes(scenes);
    });
  }

  // 3. Deterministic audio & silence analysis
  if (opts.detectAudio && info->hasAudio) {
    emit analysisProgress(assetId, 0.55, QStringLiteral("Analyzing audio & speech intervals"));
    const auto audioRes = audioAnalyzer_.analyzeAudio(filePath, assetId, info->fps, opts, [&](double frac) {
      emit analysisProgress(assetId, 0.55 + (frac * 0.25), QStringLiteral("Segmenting speech vs silence"));
    });

    {
      QMutexLocker lock(&mutex_);
      if (cancelledAssets_.contains(assetId)) {
        activeAssets_.remove(assetId);
        return false;
      }
    }
    runOnOwnerThread([&]() {
      catalog_.insertSpeechSpans(audioRes.spans);
    });
  }

  // 4. OCR on keyframes
  if (opts.ocrKeyframes && ocrBridge_.isAvailable() && !scenes.empty()) {
    emit analysisProgress(assetId, 0.82, QStringLiteral("Scanning on-screen text with OCR"));
    for (size_t i = 0; i < scenes.size(); ++i) {
      {
        QMutexLocker lock(&mutex_);
        if (cancelledAssets_.contains(assetId)) {
          activeAssets_.remove(assetId);
          return false;
        }
      }

      const auto& sc = scenes[i];
      QString decErr;
      const qint64 midMs = static_cast<qint64>(sc.startSec * 1000.0);
      const QImage frame = sf::decodeFrame(filePath, midMs, &decErr);
      if (!frame.isNull()) {
        const auto ocrItems = ocrBridge_.recognize(frame, assetId, sc.startFrame, sc.startSec);
        if (!ocrItems.empty()) {
          runOnOwnerThread([&]() {
            catalog_.insertOcrRecords(ocrItems);
          });
        }
      }
    }
  }

  // 5. Opt-in VLM scene description
  if (opts.vlmSummarize && !scenes.empty()) {
    emit analysisProgress(assetId, 0.92, QStringLiteral("Generating visual scene summaries"));
    for (size_t i = 0; i < scenes.size(); ++i) {
      {
        QMutexLocker lock(&mutex_);
        if (cancelledAssets_.contains(assetId)) {
          activeAssets_.remove(assetId);
          return false;
        }
      }

      const auto& sc = scenes[i];
      QString decErr;
      const qint64 midMs = static_cast<qint64>(sc.startSec * 1000.0);
      const QImage frame = sf::decodeFrame(filePath, midMs, &decErr);
      if (!frame.isNull()) {
        const auto tag = vlmDescriptor_.describeKeyframe(frame, assetId, sc.id);
        runOnOwnerThread([&]() {
          catalog_.insertVlmTag(tag);
        });
      }
    }
  }

  {
    QMutexLocker lock(&mutex_);
    activeAssets_.remove(assetId);
  }
  emit analysisProgress(assetId, 1.0, QStringLiteral("Complete"));
  emit analysisFinished(assetId);
  return true;
}

void IndexManager::analyzeAssetAsync(const QString& assetId,
                                     const QString& filePath,
                                     const AnalysisOptions& opts) {
  struct AnalysisTask : public QRunnable {
    IndexManager* mgr = nullptr;
    QString assetId;
    QString filePath;
    AnalysisOptions opts;

    void run() override {
      if (mgr) {
        mgr->analyzeAssetSync(assetId, filePath, opts);
      }
    }
  };

  auto* task = new AnalysisTask();
  task->mgr = this;
  task->assetId = assetId;
  task->filePath = filePath;
  task->opts = opts;
  task->setAutoDelete(true);

  threadPool_.start(task);
}

bool IndexManager::isAnalyzing(const QString& assetId) const {
  QMutexLocker lock(&mutex_);
  return activeAssets_.contains(assetId);
}

void IndexManager::cancelAsset(const QString& assetId) {
  QMutexLocker lock(&mutex_);
  if (activeAssets_.contains(assetId)) {
    cancelledAssets_.insert(assetId);
  }
}

std::vector<SearchResult> IndexManager::search(const QString& query, int limit) const {
  std::vector<SearchResult> result;
  if (QThread::currentThread() == this->thread()) {
    result = catalog_.search(query, limit);
  } else {
    QMetaObject::invokeMethod(const_cast<IndexManager*>(this), [&]() {
      result = catalog_.search(query, limit);
    }, Qt::BlockingQueuedConnection);
  }
  return result;
}

std::vector<SceneRecord> IndexManager::scenes(const QString& assetId) const {
  std::vector<SceneRecord> result;
  if (QThread::currentThread() == this->thread()) {
    result = catalog_.scenes(assetId);
  } else {
    QMetaObject::invokeMethod(const_cast<IndexManager*>(this), [&]() {
      result = catalog_.scenes(assetId);
    }, Qt::BlockingQueuedConnection);
  }
  return result;
}

std::vector<SpeechSpan> IndexManager::speechSpans(const QString& assetId) const {
  std::vector<SpeechSpan> result;
  if (QThread::currentThread() == this->thread()) {
    result = catalog_.speechSpans(assetId);
  } else {
    QMetaObject::invokeMethod(const_cast<IndexManager*>(this), [&]() {
      result = catalog_.speechSpans(assetId);
    }, Qt::BlockingQueuedConnection);
  }
  return result;
}

std::vector<OcrRecord> IndexManager::ocrRecords(const QString& assetId, qint64 frame) const {
  std::vector<OcrRecord> result;
  if (QThread::currentThread() == this->thread()) {
    result = catalog_.ocrRecords(assetId, frame);
  } else {
    QMetaObject::invokeMethod(const_cast<IndexManager*>(this), [&]() {
      result = catalog_.ocrRecords(assetId, frame);
    }, Qt::BlockingQueuedConnection);
  }
  return result;
}

std::vector<VlmTag> IndexManager::vlmTags(const QString& assetId) const {
  std::vector<VlmTag> result;
  if (QThread::currentThread() == this->thread()) {
    result = catalog_.vlmTags(assetId);
  } else {
    QMetaObject::invokeMethod(const_cast<IndexManager*>(this), [&]() {
      result = catalog_.vlmTags(assetId);
    }, Qt::BlockingQueuedConnection);
  }
  return result;
}

std::vector<TranscriptRecord> IndexManager::transcripts(const QString& assetId) const {
  std::vector<TranscriptRecord> result;
  if (QThread::currentThread() == this->thread()) {
    result = catalog_.transcripts(assetId);
  } else {
    QMetaObject::invokeMethod(const_cast<IndexManager*>(this), [&]() {
      result = catalog_.transcripts(assetId);
    }, Qt::BlockingQueuedConnection);
  }
  return result;
}

} // namespace sf::index
