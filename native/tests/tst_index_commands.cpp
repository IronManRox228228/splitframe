#include "index/index_commands.h"
#include "index/index_manager.h"
#include "agent/specialist.h"
#include "agent/peer_bus.h"
#include "engine/engine.h"

#include <QJsonArray>
#include <QTest>

class TstIndexCommands : public QObject {
  Q_OBJECT

private slots:
  void registrationAndCommandCalls() {
    sf::engine::Engine engine;
    sf::index::IndexManager manager;
    QVERIFY(manager.openCatalog(QStringLiteral(":memory:")));

    sf::index::registerIndexCommands(&engine, &manager);

    // Verify command registry
    QVERIFY(engine.find(QStringLiteral("index.analyze")) != nullptr);
    QVERIFY(engine.find(QStringLiteral("index.search")) != nullptr);
    QVERIFY(engine.find(QStringLiteral("index.scenes")) != nullptr);
    QVERIFY(engine.find(QStringLiteral("index.speech")) != nullptr);
    QVERIFY(engine.find(QStringLiteral("index.ocr")) != nullptr);
    QVERIFY(engine.find(QStringLiteral("index.status")) != nullptr);

    // Populate catalog with test items
    sf::index::TranscriptRecord tr;
    tr.id = QStringLiteral("tr1");
    tr.assetId = QStringLiteral("asset_demo");
    tr.startMs = 0;
    tr.endMs = 2000;
    tr.text = QStringLiteral("Welcome to SplitFrame video indexing");
    manager.catalog()->insertTranscript(tr);

    sf::index::SceneRecord sc;
    sc.id = QStringLiteral("sc1");
    sc.assetId = QStringLiteral("asset_demo");
    sc.startFrame = 0;
    sc.endFrame = 60;
    sc.startSec = 0.0;
    sc.endSec = 2.0;
    sc.sharpness = 450.0;
    manager.catalog()->insertScene(sc);

    sf::index::SpeechSpan sp;
    sp.id = QStringLiteral("sp1");
    sp.assetId = QStringLiteral("asset_demo");
    sp.startFrame = 0;
    sp.endFrame = 60;
    sp.isSpeech = true;
    sp.meanVolumeDb = -16.0;
    manager.catalog()->insertSpeechSpan(sp);

    sf::index::OcrRecord ocr;
    ocr.id = QStringLiteral("ocr1");
    ocr.assetId = QStringLiteral("asset_demo");
    ocr.frame = 15;
    ocr.timeSec = 0.5;
    ocr.text = QStringLiteral("GPU Acceleration");
    ocr.x = 0.1;
    ocr.y = 0.2;
    ocr.w = 0.3;
    ocr.h = 0.1;
    manager.catalog()->insertOcrRecord(ocr);

    // 1. Call index.search
    auto searchRes = engine.call(QStringLiteral("index.search"), QJsonObject{
      {QStringLiteral("query"), QStringLiteral("SplitFrame")}
    });
    QVERIFY(searchRes.ok);
    QJsonArray hits = searchRes.value.toArray();
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits[0].toObject().value(QStringLiteral("assetId")).toString(), QStringLiteral("asset_demo"));

    // 2. Call index.scenes
    auto scenesRes = engine.call(QStringLiteral("index.scenes"), QJsonObject{
      {QStringLiteral("assetId"), QStringLiteral("asset_demo")}
    });
    QVERIFY(scenesRes.ok);
    QCOMPARE(scenesRes.value.toArray().size(), 1);
    QCOMPARE(scenesRes.value.toArray()[0].toObject().value(QStringLiteral("id")).toString(), QStringLiteral("sc1"));

    // 3. Call index.speech
    auto speechRes = engine.call(QStringLiteral("index.speech"), QJsonObject{
      {QStringLiteral("assetId"), QStringLiteral("asset_demo")}
    });
    QVERIFY(speechRes.ok);
    QCOMPARE(speechRes.value.toArray().size(), 1);
    QVERIFY(speechRes.value.toArray()[0].toObject().value(QStringLiteral("isSpeech")).toBool());

    // 4. Call index.ocr
    auto ocrRes = engine.call(QStringLiteral("index.ocr"), QJsonObject{
      {QStringLiteral("assetId"), QStringLiteral("asset_demo")}
    });
    QVERIFY(ocrRes.ok);
    QCOMPARE(ocrRes.value.toArray().size(), 1);
    QCOMPARE(ocrRes.value.toArray()[0].toObject().value(QStringLiteral("text")).toString(), QStringLiteral("GPU Acceleration"));

    // 5. Call index.status
    auto statusRes = engine.call(QStringLiteral("index.status"), QJsonObject{
      {QStringLiteral("assetId"), QStringLiteral("asset_demo")}
    });
    QVERIFY(statusRes.ok);
    QCOMPARE(statusRes.value.toObject().value(QStringLiteral("isAnalyzing")).toBool(), false);
  }

  void specialistAgentAccess() {
    sf::engine::Engine engine;
    sf::index::IndexManager manager;
    QVERIFY(manager.openCatalog(QStringLiteral(":memory:")));
    sf::index::registerIndexCommands(&engine, &manager);

    sf::agent::PeerBus bus;
    auto mockLlm = std::make_shared<sf::agent::MockLlmClient>();

    sf::agent::Specialist editor(sf::agent::SpecialistKind::Editor, &engine, &bus, mockLlm);
    sf::agent::Specialist audio(sf::agent::SpecialistKind::Audio, &engine, &bus, mockLlm);
    sf::agent::Specialist colourist(sf::agent::SpecialistKind::Colourist, &engine, &bus, mockLlm);

    auto editorTools = editor.availableTools();
    auto audioTools = audio.availableTools();
    auto colouristTools = colourist.availableTools();

    auto hasTool = [](const QJsonArray& arr, const QString& name) {
      for (const auto& v : arr) {
        if (v.toObject().value(QStringLiteral("name")).toString() == name) return true;
      }
      return false;
    };

    // Shared "engine" domain commands must be visible to all 3 specialists
    QVERIFY(hasTool(editorTools, QStringLiteral("index.search")));
    QVERIFY(hasTool(editorTools, QStringLiteral("index.scenes")));
    QVERIFY(hasTool(editorTools, QStringLiteral("index.ocr")));

    QVERIFY(hasTool(audioTools, QStringLiteral("index.search")));
    QVERIFY(hasTool(audioTools, QStringLiteral("index.speech")));

    QVERIFY(hasTool(colouristTools, QStringLiteral("index.search")));
    QVERIFY(hasTool(colouristTools, QStringLiteral("index.scenes")));
  }
};

QTEST_MAIN(TstIndexCommands)
#include "tst_index_commands.moc"
