// ComfyUI workflow generation background job analyzer.

#include "comfy/comfy_generate.h"
#include "comfy/comfy_client.h"
#include "comfy/comfy_process.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <chrono>
#include <thread>

namespace sf::comfy {

jobs::AnalyzerResult ComfyGenerateAnalyzer::run(jobs::JobContext& ctx) {
  Client client;
  if (ctx.params.contains(QStringLiteral("baseUrl"))) {
    client = Client(QUrl(ctx.params.value(QStringLiteral("baseUrl")).toString()));
  }

  QJsonObject graph = ctx.params.value(QStringLiteral("graph")).toObject();
  const QString outputDir = ctx.params.value(QStringLiteral("outputDir")).toString();
  const QJsonObject uploads = ctx.params.value(QStringLiteral("uploads")).toObject();

  // 1. Upload images
  for (auto it = uploads.begin(); it != uploads.end(); ++it) {
    const QString key = it.key();
    const QString localPath = it.value().toString();
    QString upErr;
    auto upName = client.uploadImage(localPath, &upErr);
    if (!upName) {
      return jobs::AnalyzerResult::failure(QStringLiteral("Upload failed for %1: %2").arg(localPath, upErr));
    }
    const int dot = key.indexOf(QLatin1Char('.'));
    if (dot > 0) {
      const QString nodeId = key.left(dot);
      const QString inputName = key.mid(dot + 1);
      if (graph.contains(nodeId) && graph[nodeId].isObject()) {
        QJsonObject nodeObj = graph[nodeId].toObject();
        QJsonObject inputsObj = nodeObj.value(QStringLiteral("inputs")).toObject();
        inputsObj.insert(inputName, *upName);
        nodeObj.insert(QStringLiteral("inputs"), inputsObj);
        graph.insert(nodeId, nodeObj);
      }
    }
  }

  // 2. Queue prompt
  QueueResult qres = client.queuePrompt(graph);
  if (!qres.error.isEmpty() || qres.promptId.isEmpty()) {
    return jobs::AnalyzerResult::failure(qres.error.isEmpty() ? QStringLiteral("Failed to queue prompt") : qres.error);
  }

  // 3. Poll history
  const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
  qint64 timeoutMs = 900000;
  if (ctx.params.contains(QStringLiteral("timeoutMs"))) {
    timeoutMs = ctx.params.value(QStringLiteral("timeoutMs")).toVariant().toLongLong();
  } else if (ctx.params.value(QStringLiteral("params")).toObject().contains(QStringLiteral("timeoutMs"))) {
    timeoutMs = ctx.params.value(QStringLiteral("params")).toObject().value(QStringLiteral("timeoutMs")).toVariant().toLongLong();
  }
  if (timeoutMs <= 0) timeoutMs = 900000;

  int consecutiveNetErrors = 0;

  auto checkAbnormalTermination = [&]() -> std::optional<jobs::AnalyzerResult> {
    if (ctx.stop()) {
      QString ierr;
      client.interrupt(&ierr, 1000);
      return jobs::AnalyzerResult::failure(QStringLiteral("cancelled"));
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - startMs >= timeoutMs) {
      QString ierr;
      client.interrupt(&ierr, 1000);
      return jobs::AnalyzerResult::failure(QStringLiteral("comfy_timeout"));
    }

    if (provider_.serverState) {
      const auto st = provider_.serverState();
      if (st == ComfyServerState::Crashed || st == ComfyServerState::Stopped) {
        const auto exitInfo = provider_.lastExit ? provider_.lastExit() : QJsonObject{};
        const int code = exitInfo.value(QStringLiteral("code")).toInt(-1);
        const auto logs = provider_.recentLogs ? provider_.recentLogs(20) : QStringList{};
        QString msg = QStringLiteral("comfy_crashed: exit %1").arg(code);
        if (!logs.isEmpty()) {
          msg += QLatin1Char('\n') + logs.join(QLatin1Char('\n'));
        }
        return jobs::AnalyzerResult::failure(msg);
      }
      if (st == ComfyServerState::Unreachable) {
        return jobs::AnalyzerResult::failure(QStringLiteral("comfy_unreachable: server became unreachable"));
      }
    }

    return std::nullopt;
  };

  while (true) {
    if (auto res = checkAbnormalTermination()) {
      return *res;
    }

    const double elapsedSec = (QDateTime::currentMSecsSinceEpoch() - startMs) / 1000.0;
    const double totalSec = std::max(1.0, timeoutMs / 1000.0);
    ctx.report(std::min(0.95, elapsedSec / totalSec));

    QString netErr;
    HistoryResult hist = client.history(qres.promptId, &netErr, 1000);
    if (!netErr.isEmpty()) {
      consecutiveNetErrors++;
      if (consecutiveNetErrors >= 3) {
        return jobs::AnalyzerResult::failure(QStringLiteral("comfy_unreachable: %1").arg(netErr));
      }
    } else {
      consecutiveNetErrors = 0;
      if (!hist.finished) {
        QString qErr;
        QSet<QString> queued = client.queue(&qErr, 1000);
        if (qErr.isEmpty() && !queued.contains(qres.promptId)) {
          // If prompt_id is in neither list, re-check history once.
          QString recheckNetErr;
          HistoryResult recheck = client.history(qres.promptId, &recheckNetErr, 1000);
          if (recheck.finished) {
            hist = recheck;
          } else {
            return jobs::AnalyzerResult::failure(QStringLiteral("comfy_lost: job no longer known to ComfyUI"));
          }
        }
      }
    }

    if (hist.finished) {
      if (!hist.success) {
        return jobs::AnalyzerResult::failure(hist.error.isEmpty() ? QStringLiteral("ComfyUI execution failed") : hist.error);
      }

      // 4. Download outputs and write sidecars
      QDir outDir(outputDir);
      outDir.mkpath(QStringLiteral("."));
      QStringList downloadedFiles;

      for (const auto& out : hist.outputs) {
        QString safeName = QFileInfo(out.filename).fileName();
        safeName.remove(QStringLiteral(".."));
        safeName.remove(QLatin1Char('/'));
        safeName.remove(QLatin1Char('\\'));
        safeName.remove(QLatin1Char(':'));
        if (safeName.isEmpty()) safeName = QStringLiteral("output.bin");
        const QString destFile = outDir.filePath(QStringLiteral("%1_%2").arg(qres.promptId, safeName));
        QString dlErr;
        if (!client.download(out, destFile, &dlErr)) {
          return jobs::AnalyzerResult::failure(QStringLiteral("Failed to download %1: %2").arg(out.filename, dlErr));
        }
        downloadedFiles.append(destFile);

        QJsonObject sidecar;
        sidecar.insert(QStringLiteral("graph"), graph);
        sidecar.insert(QStringLiteral("promptId"), qres.promptId);
        sidecar.insert(QStringLiteral("outputNode"), out.nodeId);
        sidecar.insert(QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        sidecar.insert(QStringLiteral("params"), ctx.params.value(QStringLiteral("params")).toObject());

        QFile scFile(destFile + QStringLiteral(".sfgen.json"));
        if (scFile.open(QIODevice::WriteOnly)) {
          scFile.write(QJsonDocument(sidecar).toJson(QJsonDocument::Indented));
        }
      }

      QJsonArray filesArr;
      for (const QString& f : downloadedFiles) filesArr.append(f);

      ctx.report(1.0);
      QJsonObject resVal{
        {QStringLiteral("promptId"), qres.promptId},
        {QStringLiteral("files"), filesArr}
      };
      if (ctx.params.contains(QStringLiteral("seed"))) {
        resVal.insert(QStringLiteral("seed"), ctx.params.value(QStringLiteral("seed")));
      }
      return jobs::AnalyzerResult::success(resVal);
    }

    for (int i = 0; i < 10; ++i) {
      if (auto res = checkAbnormalTermination()) {
        return *res;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

} // namespace sf::comfy
