#include "index/zetaris_provider.h"
#include "editor/project.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

using namespace sf;
using namespace sf::index;

class TestZetaris : public QObject {
  Q_OBJECT

private slots:
  void testFederatedAssetSerialization() {
    FederatedAsset a;
    a.id = QStringLiteral("asset-broll-01");
    a.uri = QStringLiteral("s3://studio-bucket/raw/broll_shibuya.mp4");
    a.name = QStringLiteral("Shibuya Crosswalk");
    a.storageType = FederatedStorageType::CloudS3;
    a.description = QStringLiteral("High-tempo nighttime crosswalk with neon reflections");
    a.tags = {QStringLiteral("tokyo"), QStringLiteral("night"), QStringLiteral("neon")};
    a.transcriptSnippet = QStringLiteral("ambient crowd and traffic noise");
    a.durationMs = 12500;
    a.width = 3840;
    a.height = 2160;
    a.fps = 59.94;
    a.relevanceScore = 4.5f;
    a.fileSizeBytes = 524288000;

    QJsonObject json = a.toJson();
    FederatedAsset roundtrip = FederatedAsset::fromJson(json);

    QCOMPARE(roundtrip.id, a.id);
    QCOMPARE(roundtrip.uri, a.uri);
    QCOMPARE(roundtrip.name, a.name);
    QCOMPARE(roundtrip.storageType, FederatedStorageType::CloudS3);
    QCOMPARE(roundtrip.description, a.description);
    QCOMPARE(roundtrip.tags.size(), 3);
    QCOMPARE(roundtrip.transcriptSnippet, a.transcriptSnippet);
    QCOMPARE(roundtrip.durationMs, 12500);
    QCOMPARE(roundtrip.width, 3840);
    QCOMPARE(roundtrip.height, 2160);
    QCOMPARE(roundtrip.fileSizeBytes, 524288000);
  }

  void testStorageTypeConversions() {
    QCOMPARE(storageTypeToString(FederatedStorageType::Local), QStringLiteral("local"));
    QCOMPARE(storageTypeToString(FederatedStorageType::StudioNas), QStringLiteral("studio_nas"));
    QCOMPARE(storageTypeToString(FederatedStorageType::CloudS3), QStringLiteral("cloud_s3"));
    QCOMPARE(storageTypeToString(FederatedStorageType::CloudGcs), QStringLiteral("cloud_gcs"));
    QCOMPARE(storageTypeToString(FederatedStorageType::Unknown), QStringLiteral("unknown"));

    QCOMPARE(storageTypeFromString(QStringLiteral("local")), FederatedStorageType::Local);
    QCOMPARE(storageTypeFromString(QStringLiteral("nas")), FederatedStorageType::StudioNas);
    QCOMPARE(storageTypeFromString(QStringLiteral("smb")), FederatedStorageType::StudioNas);
    QCOMPARE(storageTypeFromString(QStringLiteral("s3")), FederatedStorageType::CloudS3);
    QCOMPARE(storageTypeFromString(QStringLiteral("gcs")), FederatedStorageType::CloudGcs);
    QCOMPARE(storageTypeFromString(QStringLiteral("invalid")), FederatedStorageType::Unknown);
  }

  void testNaturalLanguageQueryAndRanking() {
    ZetarisProvider provider;
    
    FederatedAsset a1;
    a1.id = QStringLiteral("asset-1");
    a1.name = QStringLiteral("Tokyo Crosswalk");
    a1.tags = {QStringLiteral("tokyo"), QStringLiteral("night"), QStringLiteral("neon")};
    a1.description = QStringLiteral("Pedestrians in Shibuya");

    FederatedAsset a2;
    a2.id = QStringLiteral("asset-2");
    a2.name = QStringLiteral("Bridge Aerial");
    a2.tags = {QStringLiteral("drone"), QStringLiteral("aerial"), QStringLiteral("bridge"), QStringLiteral("sunset")};
    a2.description = QStringLiteral("Drone shot over suspension bridge at sunset");

    FederatedAsset a3;
    a3.id = QStringLiteral("asset-3");
    a3.name = QStringLiteral("Host Interview");
    a3.tags = {QStringLiteral("interview"), QStringLiteral("studio")};
    a3.description = QStringLiteral("Main speaker talking about technology");

    provider.setMockAssets({a1, a2, a3});

    // Query specifically for the drone shot
    ZetarisQuery q;
    q.naturalLanguage = QStringLiteral("aerial sunset drone bridge");
    std::vector<FederatedAsset> results = provider.query(q);

    QCOMPARE(results.size(), 1);
    QCOMPARE(results[0].id, QStringLiteral("asset-2"));
    QVERIFY(results[0].relevanceScore > 5.0f);
  }

  void testTagAndResolutionFiltering() {
    ZetarisProvider provider;

    FederatedAsset hdClip;
    hdClip.id = QStringLiteral("hd");
    hdClip.width = 1920;
    hdClip.height = 1080;
    hdClip.tags = {QStringLiteral("nature"), QStringLiteral("forest")};

    FederatedAsset fourKClip;
    fourKClip.id = QStringLiteral("4k");
    fourKClip.width = 3840;
    fourKClip.height = 2160;
    fourKClip.tags = {QStringLiteral("nature"), QStringLiteral("mountains")};

    provider.setMockAssets({hdClip, fourKClip});

    // 1. Filter by 4K resolution requirement
    ZetarisQuery qRes;
    qRes.minWidth = 3840;
    qRes.minHeight = 2160;
    auto resResults = provider.query(qRes);
    QCOMPARE(resResults.size(), 1);
    QCOMPARE(resResults[0].id, QStringLiteral("4k"));

    // 2. Filter by tag
    ZetarisQuery qTag;
    qTag.filterTags = {QStringLiteral("forest")};
    auto tagResults = provider.query(qTag);
    QCOMPARE(tagResults.size(), 1);
    QCOMPARE(tagResults[0].id, QStringLiteral("hd"));
  }

  void testCrossStorageFederation() {
    ZetarisProvider provider;

    FederatedAsset localAsset;
    localAsset.id = QStringLiteral("loc");
    localAsset.storageType = FederatedStorageType::Local;

    FederatedAsset s3Asset;
    s3Asset.id = QStringLiteral("s3");
    s3Asset.storageType = FederatedStorageType::CloudS3;

    FederatedAsset nasAsset;
    nasAsset.id = QStringLiteral("nas");
    nasAsset.storageType = FederatedStorageType::StudioNas;

    provider.setMockAssets({localAsset, s3Asset, nasAsset});

    ZetarisQuery qS3;
    qS3.storageFilter = {FederatedStorageType::CloudS3};
    auto s3Results = provider.query(qS3);
    QCOMPARE(s3Results.size(), 1);
    QCOMPARE(s3Results[0].id, QStringLiteral("s3"));

    ZetarisQuery qLocalNas;
    qLocalNas.storageFilter = {FederatedStorageType::Local, FederatedStorageType::StudioNas};
    auto localNasResults = provider.query(qLocalNas);
    QCOMPARE(localNasResults.size(), 2);
  }

  void testImportToProject() {
    ZetarisProvider provider;
    editor::Project project;
    project.newProject(QStringLiteral("Federation Test"));

    FederatedAsset asset;
    asset.id = QStringLiteral("zetaris-asset-007");
    asset.uri = QStringLiteral("s3://footage/doc_scene1.mp4");
    asset.name = QStringLiteral("Scene 1 Cut");
    asset.storageType = FederatedStorageType::CloudS3;
    asset.durationMs = 60000;
    asset.width = 3840;
    asset.height = 2160;
    asset.fps = 24.0;
    asset.fileSizeBytes = 104857600;

    QString importedId = provider.importToProject(asset, project);
    QCOMPARE(importedId, QStringLiteral("zetaris-asset-007"));

    const auto& assets = project.bundle().assets;
    QCOMPARE(assets.size(), 1);
    QCOMPARE(assets[0].id, QStringLiteral("zetaris-asset-007"));
    QCOMPARE(assets[0].path, QStringLiteral("s3://footage/doc_scene1.mp4"));
    QCOMPARE(assets[0].metadata.location.value_or(QString()), QStringLiteral("cloud_s3"));
    QCOMPARE(assets[0].width, 3840);
    QCOMPARE(assets[0].height, 2160);
  }

  void testTelemetryAndLimits() {
    ZetarisProvider provider;
    for (int i = 0; i < 50; ++i) {
      FederatedAsset a;
      a.id = QStringLiteral("asset-%1").arg(i);
      a.name = QStringLiteral("Sample");
      provider.addMockAsset(a);
    }

    ZetarisQuery q;
    q.limit = 10;
    auto res = provider.query(q);

    QCOMPARE(res.size(), 10);
    QCOMPARE(provider.telemetry().totalQueries, 1);
    QCOMPARE(provider.telemetry().totalAssetsReturned, 10);
    QVERIFY(provider.telemetry().lastLatencyMs >= 0);

    provider.resetTelemetry();
    QCOMPARE(provider.telemetry().totalQueries, 0);
    QCOMPARE(provider.telemetry().totalAssetsReturned, 0);
  }
};

QTEST_MAIN(TestZetaris)
#include "tst_zetaris.moc"
