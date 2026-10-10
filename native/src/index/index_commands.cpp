#include "index/index_commands.h"

#include <QJsonArray>

namespace sf::index {

void registerIndexCommands(engine::Engine* engine, IndexManager* manager) {
  if (!engine || !manager) return;

  // 1. index.analyze
  engine->add(engine::CommandSpec{
    QStringLiteral("index.analyze"),
    QStringLiteral("analyzeFootage"),
    QStringLiteral("Analyzes media asset footage for scenes, focus sharpness, speech/silence spans, and OCR text."),
    QStringLiteral("engine"),
    false,
    false,
    QJsonObject{
      {QStringLiteral("type"), QStringLiteral("object")},
      {QStringLiteral("properties"), QJsonObject{
        {QStringLiteral("assetId"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("path"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("detectScenes"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("detectAudio"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("ocr"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("vlm"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("sync"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}}
      }},
      {QStringLiteral("required"), QJsonArray{QStringLiteral("assetId"), QStringLiteral("path")}}
    },
    [manager](engine::CallContext&, const QJsonObject& args) -> engine::Result {
      const QString assetId = args.value(QStringLiteral("assetId")).toString();
      const QString path = args.value(QStringLiteral("path")).toString();
      if (assetId.isEmpty() || path.isEmpty()) {
        return engine::Result::failure(QStringLiteral("invalid_params"), QStringLiteral("assetId and path are required"));
      }

      AnalysisOptions opts;
      if (args.contains(QStringLiteral("detectScenes"))) opts.detectScenes = args.value(QStringLiteral("detectScenes")).toBool();
      if (args.contains(QStringLiteral("detectAudio"))) opts.detectAudio = args.value(QStringLiteral("detectAudio")).toBool();
      if (args.contains(QStringLiteral("ocr"))) opts.ocrKeyframes = args.value(QStringLiteral("ocr")).toBool();
      if (args.contains(QStringLiteral("vlm"))) opts.vlmSummarize = args.value(QStringLiteral("vlm")).toBool();

      const bool sync = args.value(QStringLiteral("sync")).toBool(false);
      if (sync) {
        const bool ok = manager->analyzeAssetSync(assetId, path, opts);
        if (!ok) return engine::Result::failure(QStringLiteral("index_failed"), QStringLiteral("Analysis failed for %1").arg(assetId));
        return engine::Result::success(QJsonObject{{QStringLiteral("status"), QStringLiteral("done")}});
      } else {
        manager->analyzeAssetAsync(assetId, path, opts);
        return engine::Result::success(QJsonObject{{QStringLiteral("status"), QStringLiteral("started")}});
      }
    }
  });

  // 2. index.search
  engine->add(engine::CommandSpec{
    QStringLiteral("index.search"),
    QStringLiteral("searchFootage"),
    QStringLiteral("Performs full-text search across indexed transcripts, on-screen OCR text, and visual scene descriptions."),
    QStringLiteral("engine"),
    false,
    false,
    QJsonObject{
      {QStringLiteral("type"), QStringLiteral("object")},
      {QStringLiteral("properties"), QJsonObject{
        {QStringLiteral("query"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("limit"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
      }},
      {QStringLiteral("required"), QJsonArray{QStringLiteral("query")}}
    },
    [manager](engine::CallContext&, const QJsonObject& args) -> engine::Result {
      const QString query = args.value(QStringLiteral("query")).toString();
      const int limit = args.value(QStringLiteral("limit")).toInt(20);

      const auto results = manager->search(query, limit);
      QJsonArray arr;
      for (const auto& r : results) {
        arr.append(r.toJson());
      }
      return engine::Result::success(arr);
    }
  });

  // 3. index.scenes
  engine->add(engine::CommandSpec{
    QStringLiteral("index.scenes"),
    QStringLiteral("getScenes"),
    QStringLiteral("Retrieves detected scene intervals, sharpness focus scores, luma exposure, and freeze/black flags for an asset."),
    QStringLiteral("engine"),
    false,
    false,
    QJsonObject{
      {QStringLiteral("type"), QStringLiteral("object")},
      {QStringLiteral("properties"), QJsonObject{
        {QStringLiteral("assetId"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
      }},
      {QStringLiteral("required"), QJsonArray{QStringLiteral("assetId")}}
    },
    [manager](engine::CallContext&, const QJsonObject& args) -> engine::Result {
      const QString assetId = args.value(QStringLiteral("assetId")).toString();
      const auto scenes = manager->scenes(assetId);
      QJsonArray arr;
      for (const auto& s : scenes) {
        arr.append(s.toJson());
      }
      return engine::Result::success(arr);
    }
  });

  // 4. index.speech
  engine->add(engine::CommandSpec{
    QStringLiteral("index.speech"),
    QStringLiteral("getSpeechSpans"),
    QStringLiteral("Retrieves speech vs silence spans with dBFS loudness levels for clean audio pause removal."),
    QStringLiteral("engine"),
    false,
    false,
    QJsonObject{
      {QStringLiteral("type"), QStringLiteral("object")},
      {QStringLiteral("properties"), QJsonObject{
        {QStringLiteral("assetId"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
      }},
      {QStringLiteral("required"), QJsonArray{QStringLiteral("assetId")}}
    },
    [manager](engine::CallContext&, const QJsonObject& args) -> engine::Result {
      const QString assetId = args.value(QStringLiteral("assetId")).toString();
      const auto spans = manager->speechSpans(assetId);
      QJsonArray arr;
      for (const auto& sp : spans) {
        arr.append(sp.toJson());
      }
      return engine::Result::success(arr);
    }
  });

  // 5. index.ocr
  engine->add(engine::CommandSpec{
    QStringLiteral("index.ocr"),
    QStringLiteral("getOcrText"),
    QStringLiteral("Retrieves recognized on-screen text with normalized bounding box coordinates for an asset."),
    QStringLiteral("engine"),
    false,
    false,
    QJsonObject{
      {QStringLiteral("type"), QStringLiteral("object")},
      {QStringLiteral("properties"), QJsonObject{
        {QStringLiteral("assetId"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("frame"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
      }},
      {QStringLiteral("required"), QJsonArray{QStringLiteral("assetId")}}
    },
    [manager](engine::CallContext&, const QJsonObject& args) -> engine::Result {
      const QString assetId = args.value(QStringLiteral("assetId")).toString();
      const qint64 frame = args.contains(QStringLiteral("frame")) ? args.value(QStringLiteral("frame")).toInteger() : -1;
      const auto records = manager->ocrRecords(assetId, frame);
      QJsonArray arr;
      for (const auto& r : records) {
        arr.append(r.toJson());
      }
      return engine::Result::success(arr);
    }
  });

  // 6. index.status
  engine->add(engine::CommandSpec{
    QStringLiteral("index.status"),
    QStringLiteral("getIndexStatus"),
    QStringLiteral("Checks if an asset is currently undergoing background analysis."),
    QStringLiteral("engine"),
    false,
    false,
    QJsonObject{
      {QStringLiteral("type"), QStringLiteral("object")},
      {QStringLiteral("properties"), QJsonObject{
        {QStringLiteral("assetId"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
      }},
      {QStringLiteral("required"), QJsonArray{QStringLiteral("assetId")}}
    },
    [manager](engine::CallContext&, const QJsonObject& args) -> engine::Result {
      const QString assetId = args.value(QStringLiteral("assetId")).toString();
      const bool active = manager->isAnalyzing(assetId);
      return engine::Result::success(QJsonObject{
        {QStringLiteral("assetId"), assetId},
        {QStringLiteral("isAnalyzing"), active}
      });
    }
  });
}

} // namespace sf::index
