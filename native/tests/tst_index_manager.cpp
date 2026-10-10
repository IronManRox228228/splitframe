#include "index/index_manager.h"
#include "agent/llm_client.h"

#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

class TstIndexManager : public QObject {
  Q_OBJECT

  QTemporaryDir dir_;
  QString testVideoPath_;

private slots:
  void initTestCase() {
    QVERIFY(dir_.isValid());
    const QString ffmpeg = qEnvironmentVariable("SF_FFMPEG_EXE");
    QVERIFY2(!ffmpeg.isEmpty(), "SF_FFMPEG_EXE not set");

    testVideoPath_ = dir_.filePath(QStringLiteral("manager_test.mp4"));
    QProcess p;
    p.start(ffmpeg, {
      QStringLiteral("-v"), QStringLiteral("error"),
      QStringLiteral("-f"), QStringLiteral("lavfi"),
      QStringLiteral("-i"), QStringLiteral("color=c=red:s=320x240:r=25:d=1"),
      QStringLiteral("-f"), QStringLiteral("lavfi"),
      QStringLiteral("-i"), QStringLiteral("color=c=blue:s=320x240:r=25:d=1"),
      QStringLiteral("-f"), QStringLiteral("lavfi"),
      QStringLiteral("-i"), QStringLiteral("sine=frequency=440:duration=2"),
      QStringLiteral("-filter_complex"), QStringLiteral("[0:v][1:v]concat=n=2:v=1:a=0[v]"),
      QStringLiteral("-map"), QStringLiteral("[v]"),
      QStringLiteral("-map"), QStringLiteral("2:a"),
      QStringLiteral("-c:v"), QStringLiteral("mpeg4"),
      QStringLiteral("-q:v"), QStringLiteral("2"),
      QStringLiteral("-c:a"), QStringLiteral("aac"),
      QStringLiteral("-y"), testVideoPath_
    });
    QVERIFY(p.waitForFinished(30000));
    QCOMPARE(p.exitCode(), 0);
  }

  void syncAnalysisPipeline() {
    sf::index::IndexManager manager;
    QVERIFY(manager.openCatalog(QStringLiteral(":memory:")));

    sf::index::AnalysisOptions opts;
    opts.detectScenes = true;
    opts.detectAudio = true;
    opts.ocrKeyframes = true;
    opts.vlmSummarize = false; // deterministic testing
    opts.cacheDir = dir_.filePath(QStringLiteral("thumbs_sync"));

    QVERIFY(manager.analyzeAssetSync(QStringLiteral("asset_sync"), testVideoPath_, opts));

    // 1. Verify scenes
    const auto scenes = manager.scenes(QStringLiteral("asset_sync"));
    QCOMPARE(static_cast<int>(scenes.size()), 2);
    QCOMPARE(scenes[0].startFrame, 0);

    // 2. Verify speech spans
    const auto spans = manager.speechSpans(QStringLiteral("asset_sync"));
    QVERIFY(!spans.empty());

    // 3. Insert and search a transcript
    sf::index::TranscriptRecord tr;
    tr.id = QStringLiteral("tr_sync");
    tr.assetId = QStringLiteral("asset_sync");
    tr.startMs = 500;
    tr.endMs = 1500;
    tr.text = QStringLiteral("Neural rendering and video intelligence indexing");
    manager.catalog()->insertTranscript(tr);

    auto hits = manager.search(QStringLiteral("Neural rendering"));
    QCOMPARE(static_cast<int>(hits.size()), 1);
    QCOMPARE(hits[0].assetId, QStringLiteral("asset_sync"));
  }

  void asyncAnalysisSignals() {
    sf::index::IndexManager manager;
    QVERIFY(manager.openCatalog(QStringLiteral(":memory:")));

    QSignalSpy startSpy(&manager, &sf::index::IndexManager::analysisStarted);
    QSignalSpy progressSpy(&manager, &sf::index::IndexManager::analysisProgress);
    QSignalSpy finishSpy(&manager, &sf::index::IndexManager::analysisFinished);

    sf::index::AnalysisOptions opts;
    opts.detectScenes = true;
    opts.detectAudio = true;
    opts.ocrKeyframes = false;
    opts.vlmSummarize = false;

    manager.analyzeAssetAsync(QStringLiteral("asset_async"), testVideoPath_, opts);

    QVERIFY(startSpy.wait(5000));
    QCOMPARE(startSpy.count(), 1);
    QCOMPARE(startSpy.takeFirst().at(0).toString(), QStringLiteral("asset_async"));

    QVERIFY(finishSpy.wait(10000));
    QCOMPARE(finishSpy.count(), 1);
    QCOMPARE(finishSpy.takeFirst().at(0).toString(), QStringLiteral("asset_async"));

    QVERIFY(progressSpy.count() > 0);

    // Verify scenes populated
    const auto scenes = manager.scenes(QStringLiteral("asset_async"));
    QCOMPARE(static_cast<int>(scenes.size()), 2);
  }
};

QTEST_MAIN(TstIndexManager)
#include "tst_index_manager.moc"
