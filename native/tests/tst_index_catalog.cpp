#include "index/catalog.h"

#include <QTemporaryDir>
#include <QTest>

class TstIndexCatalog : public QObject {
  Q_OBJECT

private slots:
  void openInMemory() {
    sf::index::Catalog catalog;
    QVERIFY(catalog.open(QStringLiteral(":memory:")));
    QVERIFY(catalog.isOpen());
    catalog.close();
    QVERIFY(!catalog.isOpen());
  }

  void openOnFile() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("test_index.db"));

    sf::index::Catalog catalog;
    QVERIFY(catalog.open(dbPath));
    QVERIFY(catalog.isOpen());

    // Insert an asset
    QVERIFY(catalog.insertAsset(QStringLiteral("a1"), QStringLiteral("C:/test.mp4"),
                                1920, 1080, 30000, 1000, 900, 48000, 2));

    catalog.close();
    QVERIFY(!catalog.isOpen());

    // Reopen and check persistence
    sf::index::Catalog reloaded;
    QVERIFY(reloaded.open(dbPath));
    QVERIFY(reloaded.isOpen());
    reloaded.close();
  }

  void scenesAndMetrics() {
    sf::index::Catalog catalog;
    QVERIFY(catalog.open());

    sf::index::SceneRecord s1;
    s1.id = QStringLiteral("sc1");
    s1.assetId = QStringLiteral("a1");
    s1.startFrame = 0;
    s1.endFrame = 150;
    s1.startSec = 0.0;
    s1.endSec = 5.0;
    s1.sharpness = 420.5;
    s1.lumaAvg = 115.0;
    s1.lumaClippedPct = 2;
    s1.isFreeze = false;
    s1.isBlack = false;
    s1.motionScore = 12.3;
    s1.thumbnailPath = QStringLiteral("C:/thumb1.jpg");

    sf::index::SceneRecord s2;
    s2.id = QStringLiteral("sc2");
    s2.assetId = QStringLiteral("a1");
    s2.startFrame = 151;
    s2.endFrame = 300;
    s2.startSec = 5.033;
    s2.endSec = 10.0;
    s2.sharpness = 580.2;
    s2.lumaAvg = 140.2;
    s2.lumaClippedPct = 4;
    s2.isFreeze = true;
    s2.isBlack = false;
    s2.motionScore = 0.1;
    s2.thumbnailPath = QStringLiteral("C:/thumb2.jpg");

    QVERIFY(catalog.insertScenes({s1, s2}));

    const auto scenes = catalog.scenes(QStringLiteral("a1"));
    QCOMPARE(static_cast<int>(scenes.size()), 2);
    QCOMPARE(scenes[0].id, QStringLiteral("sc1"));
    QCOMPARE(scenes[0].sharpness, 420.5);
    QCOMPARE(scenes[1].id, QStringLiteral("sc2"));
    QVERIFY(scenes[1].isFreeze);
  }

  void speechSpans() {
    sf::index::Catalog catalog;
    QVERIFY(catalog.open());

    sf::index::SpeechSpan sp1;
    sp1.id = QStringLiteral("sp1");
    sp1.assetId = QStringLiteral("a1");
    sp1.startFrame = 0;
    sp1.endFrame = 90;
    sp1.startSec = 0.0;
    sp1.endSec = 3.0;
    sp1.isSpeech = true;
    sp1.meanVolumeDb = -18.5;
    sp1.maxVolumeDb = -12.0;

    sf::index::SpeechSpan sp2;
    sp2.id = QStringLiteral("sp2");
    sp2.assetId = QStringLiteral("a1");
    sp2.startFrame = 91;
    sp2.endFrame = 120;
    sp2.startSec = 3.033;
    sp2.endSec = 4.0;
    sp2.isSpeech = false;
    sp2.meanVolumeDb = -52.0;
    sp2.maxVolumeDb = -48.0;

    QVERIFY(catalog.insertSpeechSpans({sp1, sp2}));

    const auto spans = catalog.speechSpans(QStringLiteral("a1"));
    QCOMPARE(static_cast<int>(spans.size()), 2);
    QVERIFY(spans[0].isSpeech);
    QVERIFY(!spans[1].isSpeech);
  }

  void ftsSearchTranscriptsAndOcr() {
    sf::index::Catalog catalog;
    QVERIFY(catalog.open());

    // Insert transcript
    sf::index::TranscriptRecord tr1;
    tr1.id = QStringLiteral("tr1");
    tr1.assetId = QStringLiteral("a1");
    tr1.startMs = 1200;
    tr1.endMs = 4500;
    tr1.text = QStringLiteral("Welcome to the video editing masterclass on high performance GPU rendering");
    tr1.speaker = QStringLiteral("Host");
    tr1.confidence = 0.98;
    QVERIFY(catalog.insertTranscript(tr1));

    // Insert OCR
    sf::index::OcrRecord ocr1;
    ocr1.id = QStringLiteral("ocr1");
    ocr1.assetId = QStringLiteral("a1");
    ocr1.frame = 150;
    ocr1.timeSec = 5.0;
    ocr1.text = QStringLiteral("Terminal output showing cargo build and cmake preset debug");
    ocr1.x = 0.1;
    ocr1.y = 0.2;
    ocr1.w = 0.8;
    ocr1.h = 0.6;
    QVERIFY(catalog.insertOcrRecord(ocr1));

    // Insert VLM
    sf::index::VlmTag vlm1;
    vlm1.id = QStringLiteral("vlm1");
    vlm1.assetId = QStringLiteral("a1");
    vlm1.sceneId = QStringLiteral("sc1");
    vlm1.summary = QStringLiteral("Close-up shot of hands typing on mechanical keyboard");
    vlm1.shotType = QStringLiteral("close-up");
    vlm1.subject = QStringLiteral("keyboard");
    QVERIFY(catalog.insertVlmTag(vlm1));

    // Search 1: transcript keyword
    auto hits1 = catalog.search(QStringLiteral("masterclass"));
    QCOMPARE(static_cast<int>(hits1.size()), 1);
    QCOMPARE(hits1[0].assetId, QStringLiteral("a1"));
    QCOMPARE(hits1[0].itemType, QStringLiteral("transcript"));
    QVERIFY(hits1[0].snippet.contains(QStringLiteral("masterclass")));

    // Search 2: OCR keyword
    auto hits2 = catalog.search(QStringLiteral("cmake preset"));
    QCOMPARE(static_cast<int>(hits2.size()), 1);
    QCOMPARE(hits2[0].itemType, QStringLiteral("ocr"));

    // Search 3: VLM keyword
    auto hits3 = catalog.search(QStringLiteral("mechanical keyboard"));
    QCOMPARE(static_cast<int>(hits3.size()), 1);
    QCOMPARE(hits3[0].itemType, QStringLiteral("vlm"));

    // Search 4: No match
    auto hits4 = catalog.search(QStringLiteral("nonexistent astronaut query"));
    QCOMPARE(static_cast<int>(hits4.size()), 0);
  }

  void clearAssetRemovesEverything() {
    sf::index::Catalog catalog;
    QVERIFY(catalog.open());

    sf::index::TranscriptRecord tr;
    tr.id = QStringLiteral("t_clear");
    tr.assetId = QStringLiteral("asset_del");
    tr.text = QStringLiteral("Delete me please");
    QVERIFY(catalog.insertTranscript(tr));

    QCOMPARE(static_cast<int>(catalog.search(QStringLiteral("Delete")).size()), 1);

    QVERIFY(catalog.clearAsset(QStringLiteral("asset_del")));

    QCOMPARE(static_cast<int>(catalog.search(QStringLiteral("Delete")).size()), 0);
    QCOMPARE(static_cast<int>(catalog.transcripts(QStringLiteral("asset_del")).size()), 0);
  }
};

QTEST_MAIN(TstIndexCatalog)
#include "tst_index_catalog.moc"
