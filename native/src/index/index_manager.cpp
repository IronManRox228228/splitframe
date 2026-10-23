#include "index/index_manager.h"

#include "media/probe.h"

#include <QEventLoop>
#include <QMetaObject>
#include <QRunnable>
#include <QThread>
#include <algorithm>
#include <cmath>

namespace sf::index {

IndexManager::IndexManager(QObject* parent)
  : QObject(parent) {
  threadPool_.setMaxThreadCount(4);
}

IndexManager::~IndexManager() {
  // Workers never block on this thread any more (non-blocking marshalling below), so draining
  // the pool cannot deadlock here. Queued catalog writes that never ran die with this object.
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
  if (QThread::currentThread() == this->thread()) {
    // Run the analysis on the pool while the owner thread's event loop keeps pumping: a sync
    // analysis (FFmpeg + OCR + LLM) no longer freezes command/API processing. The completion
    // lambda lands behind the catalog-write lambdas on the same queue, so this returns only
    // after the writes flushed.
    bool ok = false;
    bool done = false;
    QEventLoop loop;
    threadPool_.start([this, &ok, &done, &loop, assetId, filePath, opts] {
      ok = analyzeAssetImpl(assetId, filePath, opts);
      QMetaObject::invokeMethod(this, [&] {
        done = true;
        loop.quit();
      }, Qt::QueuedConnection);
    });
    while (!done) {
      loop.exec();
    }
    return ok;
  }
  // Worker/other thread: run in place (catalog writes land on the owner thread asynchronously).
  return analyzeAssetImpl(assetId, filePath, opts);
}

bool IndexManager::analyzeAssetImpl(const QString& assetId,
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

  // Helper: ensure catalog updates execute on the IndexManager's owner thread. Non-blocking:
  // a worker must never park on this thread (the dtor drains the pool — a blocking marshal
  // here would wait for an event loop that is itself waiting for the pool: deadlock).
  auto runOnOwnerThread = [this](auto&& func) {
    if (QThread::currentThread() == this->thread()) {
      func();
    } else {
      QMetaObject::invokeMethod(this, func, Qt::QueuedConnection);
    }
  };

  // 1. Insert asset entry
  const int fpsNum = static_cast<int>(std::round(info->fps * 1000.0));
  constexpr int fpsDen = 1000;
  const qint64 totalFrames = (info->frameCount > 0)
    ? info->frameCount
    : static_cast<qint64>(std::round((info->durationMs / 1000.0) * info->fps));

  // Queued writes execute long after a phase's stack frame may be gone, so every
  // call-site lambda must capture by value — never [&] over block-scoped locals.
  runOnOwnerThread([this, assetId, filePath, w = info->width, h = info->height, fpsNum, fpsDen, totalFrames,
                    ar = info->audioSampleRate, ac = info->audioChannels]() {
    catalog_.insertAsset(assetId, filePath, w, h, fpsNum, fpsDen, totalFrames, ar, ac);
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
        emit analysisFailed(assetId, QStringLiteral("cancelled"));
        return false;
      }
    }
    runOnOwnerThread([this, scenes]() {
      catalog_.insertScenes(scenes);
    });
  }

  // OCR and VLM no longer depend on scene detection (they previously silently skipped when
  // detectScenes was off): without scenes they sample evenly spaced keyframes instead, so a
  // speech-only or LLM-only index keeps its text/visual evidence.
  struct KeyframeHit {
    qint64 frame;
    double sec;
    QString sceneId;
  };
  std::vector<KeyframeHit> keyframeHits;
  keyframeHits.reserve(scenes.size());
  for (const auto& sc : scenes) {
    keyframeHits.push_back({sc.startFrame, sc.startSec, sc.id});
  }
  if (keyframeHits.empty() && info->hasVideo) {
    const qint64 sampleCount = std::clamp<qint64>(info->durationMs / 10000, 1, 24); // 1 sample per ~10 s, capped
    for (qint64 i = 0; i < sampleCount; ++i) {
      const qint64 fr = totalFrames > 0 ? (totalFrames - 1) * i / std::max<qint64>(sampleCount - 1, 1) : 0;
      keyframeHits.push_back({fr, static_cast<double>(fr) / std::max<double>(info->fps, 1.0), QString()});
    }
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
        emit analysisFailed(assetId, QStringLiteral("cancelled"));
        return false;
      }
    }
    runOnOwnerThread([this, spans = audioRes.spans]() {
      catalog_.insertSpeechSpans(spans);
    });
  }

  // 4. OCR on keyframes
  if (opts.ocrKeyframes && ocrBridge_.isAvailable() && !keyframeHits.empty()) {
    emit analysisProgress(assetId, 0.82, QStringLiteral("Scanning on-screen text with OCR"));
    for (size_t i = 0; i < keyframeHits.size(); ++i) {
      {
        QMutexLocker lock(&mutex_);
        if (cancelledAssets_.contains(assetId)) {
          activeAssets_.remove(assetId);
          emit analysisFailed(assetId, QStringLiteral("cancelled"));
          return false;
        }
      }

      const auto& kf = keyframeHits[i];
      QString decErr;
      const qint64 midMs = static_cast<qint64>(kf.sec * 1000.0);
      const QImage frame = sf::decodeFrame(filePath, midMs, &decErr);
      if (!frame.isNull()) {
        const auto ocrItems = ocrBridge_.recognize(frame, assetId, kf.frame, kf.sec);
        if (!ocrItems.empty()) {
          runOnOwnerThread([this, ocrItems]() {
            catalog_.insertOcrRecords(ocrItems);
          });
        }
      }
    }
  }

  // 5. Opt-in VLM scene description
  if (opts.vlmSummarize && !keyframeHits.empty()) {
    emit analysisProgress(assetId, 0.92, QStringLiteral("Generating visual scene summaries"));
    for (size_t i = 0; i < keyframeHits.size(); ++i) {
      {
        QMutexLocker lock(&mutex_);
        if (cancelledAssets_.contains(assetId)) {
          activeAssets_.remove(assetId);
          emit analysisFailed(assetId, QStringLiteral("cancelled"));
          return false;
        }
      }

      const auto& kf = keyframeHits[i];
      QString decErr;
      const qint64 midMs = static_cast<qint64>(kf.sec * 1000.0);
      const QImage frame = sf::decodeFrame(filePath, midMs, &decErr);
      if (!frame.isNull()) {
        auto tag = vlmDescriptor_.describeKeyframe(frame, assetId, kf.sceneId);
        tag.frame = kf.frame;
        tag.timeSec = kf.sec;
        if (!tag.summary.isEmpty()) { // empty = no real description produced: do not store a fake one
          runOnOwnerThread([this, tag]() {
            catalog_.insertVlmTag(tag);
          });
        }
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
