// ComfyUI integration tests: templates, background run, caching, and generate.shot timeline placement.

#include "comfy/comfy_client.h"
#include "comfy/comfy_graph.h"
#include "comfy_fake_server.h"
#include "core/schema.h"
#include "engine/engine.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace sf;
using namespace sf::engine;

class TstComfyRun : public QObject {
  Q_OBJECT

  std::unique_ptr<FakeComfyServer> fake_;
  std::unique_ptr<QTemporaryDir> userTemplatesDir_;
  std::unique_ptr<QTemporaryDir> jobsCacheDir_;
  std::unique_ptr<Engine> engine_;

  static QString q(const char* s) { return QString::fromLatin1(s); }

private slots:
  void initTestCase() {
    fake_ = std::make_unique<FakeComfyServer>();
    qputenv("SF_COMFY_URL", fake_->url().toString().toUtf8());

    userTemplatesDir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(userTemplatesDir_->isValid());
    qputenv("SF_COMFY_TEMPLATES", userTemplatesDir_->path().toUtf8());

    jobsCacheDir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(jobsCacheDir_->isValid());
    qputenv("SF_JOBS_CACHE_DIR", jobsCacheDir_->path().toUtf8());
  }

  void init() {
    engine_ = std::make_unique<Engine>();
  }

  void cleanup() {
    engine_.reset();
  }

  // 1. templateList: sd15_txt2img is listed with source builtin.
  void templateList() {
    Result r = engine_->call(q("comfy.template.list"), {});
    QVERIFY(r.ok);
    const QJsonArray templates = r.value.toObject().value(q("templates")).toArray();
    bool foundSd15 = false;
    for (const auto& v : templates) {
      const QJsonObject t = v.toObject();
      if (t.value(q("id")).toString() == QLatin1String("sd15_txt2img")) {
        foundSd15 = true;
        QCOMPARE(t.value(q("source")).toString(), QStringLiteral("builtin"));
        QCOMPARE(t.value(q("kind")).toString(), QStringLiteral("image"));
        QVERIFY(t.value(q("bindings")).toArray().contains(QStringLiteral("prompt")));
        QVERIFY(t.value(q("bindings")).toArray().contains(QStringLiteral("seed")));
        break;
      }
    }
    QVERIFY2(foundSd15, "Builtin template sd15_txt2img not found in comfy.template.list");
  }

  // 2. fromTemplateBinds: params prompt/seed/width set the right inputs. An unknown param is invalid.
  void fromTemplateBinds() {
    QJsonObject validParams{
      {q("prompt"), QStringLiteral("a majestic mountain")},
      {q("seed"), 424242},
      {q("width"), 768}
    };
    Result r = engine_->call(q("comfy.graph.fromTemplate"), {
      {q("templateId"), QStringLiteral("sd15_txt2img")},
      {q("params"), validParams}
    });
    QVERIFY(r.ok);
    const QString gid = r.value.toObject().value(q("graphId")).toString();
    QVERIFY(!gid.isEmpty());
    const QJsonArray applied = r.value.toObject().value(q("applied")).toArray();
    QCOMPARE(applied.size(), 3);

    // Verify graph inputs
    Result rGet = engine_->call(q("comfy.graph.get"), {{q("graphId"), gid}});
    QVERIFY(rGet.ok);
    const QJsonObject graphObj = rGet.value.toObject().value(q("graph")).toObject();
    // node 6 is prompt CLIPTextEncode
    QCOMPARE(graphObj.value(q("6")).toObject().value(q("inputs")).toObject().value(q("text")).toString(),
             QStringLiteral("a majestic mountain"));
    // node 3 is KSampler seed
    QCOMPARE(graphObj.value(q("3")).toObject().value(q("inputs")).toObject().value(q("seed")).toInt(),
             424242);
    // node 5 is EmptyLatentImage width
    QCOMPARE(graphObj.value(q("5")).toObject().value(q("inputs")).toObject().value(q("width")).toInt(),
             768);

    // Unknown param returns invalid
    QJsonObject badParams{
      {q("nonExistentParam"), QStringLiteral("fails")}
    };
    Result rBad = engine_->call(q("comfy.graph.fromTemplate"), {
      {q("templateId"), QStringLiteral("sd15_txt2img")},
      {q("params"), badParams}
    });
    QVERIFY(!rBad.ok);
    QCOMPARE(rBad.error.code, QStringLiteral("invalid_params"));
  }

  // 3. runValidatesFirst: a graph with steps=0 gives invalid with issues, and the fake's queued stays empty.
  void runValidatesFirst() {
    Result rTmpl = engine_->call(q("comfy.graph.fromTemplate"), {
      {q("templateId"), QStringLiteral("sd15_txt2img")},
      {q("params"), QJsonObject{{q("steps"), 0}}}
    });
    QVERIFY(rTmpl.ok);
    const QString gid = rTmpl.value.toObject().value(q("graphId")).toString();

    const size_t queuedBefore = fake_->queued.size();

    Result rRun = engine_->call(q("comfy.run"), {{q("graphId"), gid}});
    QVERIFY(!rRun.ok);
    QCOMPARE(rRun.error.code, QStringLiteral("invalid"));
    QVERIFY(rRun.error.details.toObject().contains(q("issues")));
    const QJsonArray issues = rRun.error.details.toObject().value(q("issues")).toArray();
    QVERIFY(!issues.isEmpty());

    // Fake queued remains empty (no new prompt queued)
    QCOMPARE(fake_->queued.size(), queuedBefore);
  }

  // 4. runWaitDownloads: comfy.run {wait:true} returns 1 file that exists, with a .sfgen.json sidecar.
  void runWaitDownloads() {
    Result rTmpl = engine_->call(q("comfy.graph.fromTemplate"), {
      {q("templateId"), QStringLiteral("sd15_txt2img")},
      {q("params"), QJsonObject{{q("prompt"), QStringLiteral("test run wait")}, {q("steps"), 20}}}
    });
    QVERIFY(rTmpl.ok);
    const QString gid = rTmpl.value.toObject().value(q("graphId")).toString();

    Result rRun = engine_->call(q("comfy.run"), {{q("graphId"), gid}, {q("wait"), true}});
    QVERIFY2(rRun.ok, qPrintable(rRun.error.message));
    const QJsonObject resObj = rRun.value.toObject();
    const QJsonArray files = resObj.value(q("files")).toArray();
    QCOMPARE(files.size(), 1);

    const QString filePath = files.at(0).toString();
    QVERIFY2(QFile::exists(filePath), qPrintable(filePath));

    const QString sidecarPath = filePath + QStringLiteral(".sfgen.json");
    QVERIFY2(QFile::exists(sidecarPath), qPrintable(sidecarPath));

    QFile scFile(sidecarPath);
    QVERIFY(scFile.open(QIODevice::ReadOnly));
    QJsonDocument scDoc = QJsonDocument::fromJson(scFile.readAll());
    QVERIFY(scDoc.isObject());
    QVERIFY(!scDoc.object().value(q("promptId")).toString().isEmpty());
    QVERIFY(!scDoc.object().value(q("outputNode")).toString().isEmpty());
  }

  // 5. runCached: running the same graph again gives cached: true, and the fake receives no new prompt.
  void runCached() {
    Result rTmpl = engine_->call(q("comfy.graph.fromTemplate"), {
      {q("templateId"), QStringLiteral("sd15_txt2img")},
      {q("params"), QJsonObject{{q("prompt"), QStringLiteral("test cache hit")}, {q("steps"), 20}, {q("seed"), 999999}}}
    });
    QVERIFY(rTmpl.ok);
    const QString gid = rTmpl.value.toObject().value(q("graphId")).toString();

    Result rRun1 = engine_->call(q("comfy.run"), {{q("graphId"), gid}, {q("wait"), true}});
    QVERIFY(rRun1.ok);

    const size_t queuedCount = fake_->queued.size();

    Result rRun2 = engine_->call(q("comfy.run"), {{q("graphId"), gid}, {q("wait"), true}});
    QVERIFY(rRun2.ok);
    QCOMPARE(rRun2.value.toObject().value(q("cached")).toBool(), true);
    QCOMPARE(fake_->queued.size(), queuedCount);
  }

  // 6. generateShotPlacesImage: on a new project, generate.shot {templateId:"sd15_txt2img", prompt:"red square"}
  // creates 1 image item of 90 frames at 30 fps. A single undo removes it.
  void generateShotPlacesImage() {
    Result rProj = engine_->call(q("project.new"), {
      {q("name"), QStringLiteral("ComfyShotProject")},
      {q("width"), 1920},
      {q("height"), 1080},
      {q("fps"), 30}
    });
    QVERIFY(rProj.ok);

    Result rShot = engine_->call(q("generate.shot"), {
      {q("templateId"), QStringLiteral("sd15_txt2img")},
      {q("prompt"), QStringLiteral("red square")},
      {q("wait"), true}
    });
    QVERIFY2(rShot.ok, qPrintable(rShot.error.message));
    const QJsonObject shotVal = rShot.value.toObject();
    QVERIFY(shotVal.contains(q("seed")));
    QVERIFY(shotVal.contains(q("files")));
    QVERIFY(shotVal.contains(q("created")));

    // Verify 1 image item of 90 frames at 30 fps on the timeline
    Result rGet = engine_->call(q("timeline.get"), {{q("mode"), QStringLiteral("summary")}});
    QVERIFY(rGet.ok);
    const QJsonArray tracks = rGet.value.toObject().value(q("tracks")).toArray();
    int itemCount = 0;
    for (const auto& t : tracks) {
      const QJsonArray items = t.toObject().value(q("items")).toArray();
      itemCount += items.size();
      for (const auto& itVal : items) {
        const QJsonObject itObj = itVal.toObject();
        QCOMPARE(itObj.value(q("type")).toString(), QStringLiteral("image"));
        const int dur = itObj.contains(q("dur")) ? itObj.value(q("dur")).toInt() : itObj.value(q("durationFrames")).toInt();
        QCOMPARE(dur, 90);
      }
    }
    QCOMPARE(itemCount, 1);

    // A single undo removes it
    Result rUndo = engine_->call(q("undo"), {});
    QVERIFY(rUndo.ok);

    Result rGetAfter = engine_->call(q("timeline.get"), {{q("mode"), QStringLiteral("summary")}});
    QVERIFY(rGetAfter.ok);
    const QJsonArray tracksAfter = rGetAfter.value.toObject().value(q("tracks")).toArray();
    int itemCountAfter = 0;
    for (const auto& t : tracksAfter) {
      itemCountAfter += t.toObject().value(q("items")).toArray().size();
    }
    QCOMPARE(itemCountAfter, 0);
  }

  // 7. importTemplateChecksBindings: a binding to node "99" is invalid; a valid one is listed with source user.
  void importTemplateChecksBindings() {
    const QString fixturesDir = QStringLiteral(SF_COMFY_FIXTURES);
    const QString apiJsonPath = fixturesDir + QStringLiteral("/txt2img_api.json");

    // Invalid binding to node "99"
    Result rBad = engine_->call(q("comfy.template.import"), {
      {q("id"), QStringLiteral("bad_tmpl")},
      {q("name"), QStringLiteral("Bad Template")},
      {q("kind"), QStringLiteral("image")},
      {q("outputNode"), QStringLiteral("9")},
      {q("path"), apiJsonPath},
      {q("bindings"), QJsonObject{
        {q("prompt"), QJsonObject{{q("node"), QStringLiteral("99")}, {q("input"), QStringLiteral("text")}}}
      }}
    });
    QVERIFY(!rBad.ok);
    QCOMPARE(rBad.error.code, QStringLiteral("invalid_params"));
    QVERIFY(rBad.error.message.contains(QStringLiteral("99")));

    // Invalid id with path traversal "../x"
    Result rBadId = engine_->call(q("comfy.template.import"), {
      {q("id"), QStringLiteral("../x")},
      {q("name"), QStringLiteral("Path Traversal Template")},
      {q("kind"), QStringLiteral("image")},
      {q("outputNode"), QStringLiteral("9")},
      {q("path"), apiJsonPath},
      {q("bindings"), QJsonObject{
        {q("prompt"), QJsonObject{{q("node"), QStringLiteral("6")}, {q("input"), QStringLiteral("text")}}}
      }}
    });
    QVERIFY(!rBadId.ok);
    QCOMPARE(rBadId.error.code, QStringLiteral("invalid_params"));

    // Valid binding
    Result rGood = engine_->call(q("comfy.template.import"), {
      {q("id"), QStringLiteral("user_txt2img")},
      {q("name"), QStringLiteral("User SD Template")},
      {q("kind"), QStringLiteral("image")},
      {q("outputNode"), QStringLiteral("9")},
      {q("path"), apiJsonPath},
      {q("bindings"), QJsonObject{
        {q("prompt"), QJsonObject{{q("node"), QStringLiteral("6")}, {q("input"), QStringLiteral("text")}}}
      }}
    });
    QVERIFY2(rGood.ok, qPrintable(rGood.error.message));
    QCOMPARE(rGood.value.toObject().value(q("id")).toString(), QStringLiteral("user_txt2img"));

    // Check list has source "user"
    Result rList = engine_->call(q("comfy.template.list"), {});
    QVERIFY(rList.ok);
    const QJsonArray templates = rList.value.toObject().value(q("templates")).toArray();
    bool foundUser = false;
    for (const auto& v : templates) {
      const QJsonObject t = v.toObject();
      if (t.value(q("id")).toString() == QLatin1String("user_txt2img")) {
        foundUser = true;
        QCOMPARE(t.value(q("source")).toString(), QStringLiteral("user"));
        break;
      }
    }
    QVERIFY2(foundUser, "Imported template user_txt2img not listed with source user");
  }

  // 8. seedDeterministic: the same prompt twice gives the same seed in the output.
  void seedDeterministic() {
    Result rProj = engine_->call(q("project.new"), {
      {q("name"), QStringLiteral("SeedProj")},
      {q("width"), 1920},
      {q("height"), 1080},
      {q("fps"), 30}
    });
    QVERIFY(rProj.ok);

    Result r1 = engine_->call(q("generate.shot"), {
      {q("templateId"), QStringLiteral("sd15_txt2img")},
      {q("prompt"), QStringLiteral("deterministic prompt")},
      {q("wait"), false}
    });
    QVERIFY(r1.ok);

    Result r2 = engine_->call(q("generate.shot"), {
      {q("templateId"), QStringLiteral("sd15_txt2img")},
      {q("prompt"), QStringLiteral("deterministic prompt")},
      {q("wait"), false}
    });
    QVERIFY(r2.ok);

    const qint64 seed1 = r1.value.toObject().value(q("seed")).toVariant().toLongLong();
    const qint64 seed2 = r2.value.toObject().value(q("seed")).toVariant().toLongLong();
    QCOMPARE(seed1, seed2);

    Result r3 = engine_->call(q("generate.shot"), {
      {q("templateId"), QStringLiteral("sd15_txt2img")},
      {q("prompt"), QStringLiteral("different prompt")},
      {q("wait"), false}
    });
    QVERIFY(r3.ok);
    const qint64 seed3 = r3.value.toObject().value(q("seed")).toVariant().toLongLong();
    QVERIFY(seed1 != seed3);
  }

  // 9. reshootNeedsSidecar: an imported non-generated asset gives invalid.
  void reshootNeedsSidecar() {
    Result rProj = engine_->call(q("project.new"), {
      {q("name"), QStringLiteral("NeedsSidecarProj")},
      {q("width"), 1920},
      {q("height"), 1080},
      {q("fps"), 30}
    });
    QVERIFY(rProj.ok);

    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString imgPath = tmp.filePath(QStringLiteral("normal.png"));
    QImage dummy(64, 64, QImage::Format_RGB32);
    dummy.fill(Qt::blue);
    QVERIFY(dummy.save(imgPath, "PNG"));

    Result rImport = engine_->call(q("media.import"), {{q("paths"), QJsonArray{imgPath}}, {q("wait"), true}});
    QVERIFY(rImport.ok);
    const QString assetId = rImport.value.toObject().value(q("added")).toArray().first().toString();

    Result rClip = engine_->call(q("clip.add"), {{q("assetId"), assetId}});
    QVERIFY(rClip.ok);
    const QString itemId = rClip.value.toObject().value(q("created")).toObject().value(q("items")).toArray().first().toString();

    Result rReshoot = engine_->call(q("reshoot.variants"), {{q("itemId"), itemId}});
    QVERIFY(!rReshoot.ok);
    QCOMPARE(rReshoot.error.code, QStringLiteral("invalid_params"));
    QVERIFY(rReshoot.error.message.contains(QStringLiteral("no .sfgen.json")));
  }

  // 10. reshootVariantsSeeds: seeds are original+1, +2, +3, sheet PNG exists, fake server received expected seeds.
  void reshootVariantsSeeds() {
    Result rProj = engine_->call(q("project.new"), {
      {q("name"), QStringLiteral("ReshootSeedsProj")},
      {q("width"), 1920},
      {q("height"), 1080},
      {q("fps"), 30}
    });
    QVERIFY(rProj.ok);

    Result rShot = engine_->call(q("generate.shot"), {
      {q("templateId"), QStringLiteral("sd15_txt2img")},
      {q("prompt"), QStringLiteral("a blue lake")},
      {q("wait"), true}
    });
    QVERIFY2(rShot.ok, qPrintable(rShot.error.message));
    const qint64 origSeed = rShot.value.toObject().value(q("seed")).toVariant().toLongLong();
    const QString itemId = rShot.value.toObject().value(q("created")).toObject().value(q("items")).toArray().first().toString();

    const size_t queuedBefore = fake_->queued.size();

    Result rVariants = engine_->call(q("reshoot.variants"), {
      {q("itemId"), itemId},
      {q("count"), 3}
    });
    QVERIFY2(rVariants.ok, qPrintable(rVariants.error.message));
    const QJsonObject vObj = rVariants.value.toObject();

    const QString sheetPath = vObj.value(q("sheetPath")).toString();
    QVERIFY(!sheetPath.isEmpty());
    QVERIFY(QFile::exists(sheetPath));

    const QJsonArray variants = vObj.value(q("variants")).toArray();
    QCOMPARE(variants.size(), 3);
    const QStringList expectedLetters{QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")};
    for (int i = 0; i < 3; ++i) {
      const QJsonObject v = variants.at(i).toObject();
      QCOMPARE(v.value(q("letter")).toString(), expectedLetters[i]);
      QCOMPARE(v.value(q("seed")).toVariant().toLongLong(), origSeed + 1 + i);
    }

    QCOMPARE(fake_->queued.size(), queuedBefore + 3);
    for (int i = 0; i < 3; ++i) {
      const QJsonObject promptGraph = fake_->queued[queuedBefore + i];
      const qint64 ksamplerSeed = promptGraph.value(q("3")).toObject().value(q("inputs")).toObject().value(q("seed")).toVariant().toLongLong();
      QCOMPARE(ksamplerSeed, origSeed + 1 + i);
    }
  }

  // 11. reshootPromptEdits: prompts end with edits, seeds unchanged.
  void reshootPromptEdits() {
    Result rProj = engine_->call(q("project.new"), {
      {q("name"), QStringLiteral("PromptEditsProj")},
      {q("width"), 1920},
      {q("height"), 1080},
      {q("fps"), 30}
    });
    QVERIFY(rProj.ok);

    Result rShot = engine_->call(q("generate.shot"), {
      {q("templateId"), QStringLiteral("sd15_txt2img")},
      {q("prompt"), QStringLiteral("a castle")},
      {q("wait"), true}
    });
    QVERIFY(rShot.ok);
    const qint64 origSeed = rShot.value.toObject().value(q("seed")).toVariant().toLongLong();
    const QString itemId = rShot.value.toObject().value(q("created")).toObject().value(q("items")).toArray().first().toString();

    const size_t queuedBefore = fake_->queued.size();

    Result rVariants = engine_->call(q("reshoot.variants"), {
      {q("itemId"), itemId},
      {q("count"), 2},
      {q("vary"), QStringLiteral("prompt")},
      {q("promptEdits"), QJsonArray{QStringLiteral("at dusk"), QStringLiteral("wide shot")}}
    });
    QVERIFY2(rVariants.ok, qPrintable(rVariants.error.message));
    const QJsonArray variants = rVariants.value.toObject().value(q("variants")).toArray();
    QCOMPARE(variants.size(), 2);

    QCOMPARE(variants.at(0).toObject().value(q("prompt")).toString(), QStringLiteral("a castle, at dusk"));
    QCOMPARE(variants.at(0).toObject().value(q("seed")).toVariant().toLongLong(), origSeed);
    QCOMPARE(variants.at(1).toObject().value(q("prompt")).toString(), QStringLiteral("a castle, wide shot"));
    QCOMPARE(variants.at(1).toObject().value(q("seed")).toVariant().toLongLong(), origSeed);

    QCOMPARE(fake_->queued.size(), queuedBefore + 2);
    QCOMPARE(fake_->queued[queuedBefore].value(q("6")).toObject().value(q("inputs")).toObject().value(q("text")).toString(),
             QStringLiteral("a castle, at dusk"));
    QCOMPARE(fake_->queued[queuedBefore + 1].value(q("6")).toObject().value(q("inputs")).toObject().value(q("text")).toString(),
             QStringLiteral("a castle, wide shot"));
  }

  // 12. reshootPickSwaps: pick B swaps asset, keeps startFrame and duration, one undo restores original asset id.
  void reshootPickSwaps() {
    Result rProj = engine_->call(q("project.new"), {
      {q("name"), QStringLiteral("PickSwapsProj")},
      {q("width"), 1920},
      {q("height"), 1080},
      {q("fps"), 30}
    });
    QVERIFY(rProj.ok);

    Result rShot = engine_->call(q("generate.shot"), {
      {q("templateId"), QStringLiteral("sd15_txt2img")},
      {q("prompt"), QStringLiteral("a green tree")},
      {q("wait"), true}
    });
    QVERIFY(rShot.ok);
    const QString itemId = rShot.value.toObject().value(q("created")).toObject().value(q("items")).toArray().first().toString();

    Result rGetBefore = engine_->call(q("item.get"), {{q("itemId"), itemId}});
    QVERIFY(rGetBefore.ok);
    const QString origAssetId = rGetBefore.value.toObject().value(q("assetId")).toString();
    const int origStartFrame = rGetBefore.value.toObject().value(q("startFrame")).toInt();
    const int origDur = rGetBefore.value.toObject().value(q("durationFrames")).toInt();
    QVERIFY(!origAssetId.isEmpty());

    Result rVariants = engine_->call(q("reshoot.variants"), {
      {q("itemId"), itemId},
      {q("count"), 3}
    });
    QVERIFY(rVariants.ok);

    Result rPick = engine_->call(q("reshoot.pick"), {
      {q("itemId"), itemId},
      {q("letter"), QStringLiteral("B")}
    });
    QVERIFY2(rPick.ok, qPrintable(rPick.error.message));
    const QString newAssetId = rPick.value.toObject().value(q("assetId")).toString();
    QVERIFY(!newAssetId.isEmpty());
    QVERIFY(newAssetId != origAssetId);

    Result rGetMid = engine_->call(q("item.get"), {{q("itemId"), itemId}});
    QVERIFY(rGetMid.ok);
    QCOMPARE(rGetMid.value.toObject().value(q("assetId")).toString(), newAssetId);
    QCOMPARE(rGetMid.value.toObject().value(q("startFrame")).toInt(), origStartFrame);
    QCOMPARE(rGetMid.value.toObject().value(q("durationFrames")).toInt(), origDur);

    Result rUndo = engine_->call(q("history.undo"), {});
    QVERIFY(rUndo.ok);

    Result rGetAfter = engine_->call(q("item.get"), {{q("itemId"), itemId}});
    QVERIFY(rGetAfter.ok);
    QCOMPARE(rGetAfter.value.toObject().value(q("assetId")).toString(), origAssetId);
  }

  // 13. pickWithoutVariants: gives invalid.
  void pickWithoutVariants() {
    Result rProj = engine_->call(q("project.new"), {
      {q("name"), QStringLiteral("PickNoVariantsProj")},
      {q("width"), 1920},
      {q("height"), 1080},
      {q("fps"), 30}
    });
    QVERIFY(rProj.ok);

    Result rPick = engine_->call(q("reshoot.pick"), {
      {q("itemId"), QStringLiteral("nonexistent_item")},
      {q("letter"), QStringLiteral("A")}
    });
    QVERIFY(!rPick.ok);
    QCOMPARE(rPick.error.code, QStringLiteral("invalid_params"));
    QVERIFY(rPick.error.message.contains(QStringLiteral("Run reshoot.variants first")));
  }

  // 14. sanitizeEvilFilename: server output filename "../../evil.png" is sanitized to basename inside outDir.
  void sanitizeEvilFilename() {
    QTemporaryDir tempOut;
    QVERIFY(tempOut.isValid());

    fake_->setOutputFilename(QStringLiteral("../../evil.png"));

    const QString fixturesDir = QStringLiteral(SF_COMFY_FIXTURES);
    const QString apiJsonPath = fixturesDir + QStringLiteral("/txt2img_api.json");

    Result rLoad = engine_->call(q("comfy.graph.load"), {{q("path"), apiJsonPath}});
    QVERIFY(rLoad.ok);
    const QString gid = rLoad.value.toObject().value(q("graphId")).toString();

    Result rRun = engine_->call(q("comfy.run"), {
      {q("graphId"), gid},
      {q("outputDir"), tempOut.path()},
      {q("wait"), true}
    });
    QVERIFY2(rRun.ok, qPrintable(rRun.error.message));

    const QJsonArray files = rRun.value.toObject().value(q("files")).toArray();
    QVERIFY(!files.isEmpty());
    const QString savedPath = files.at(0).toString();
    QVERIFY(!savedPath.contains(QStringLiteral("..")));
    QVERIFY(savedPath.endsWith(QStringLiteral("evil.png")));
    const QFileInfo fi(savedPath);
    QCOMPARE(fi.dir().canonicalPath(), QDir(tempOut.path()).canonicalPath());

    fake_->setOutputFilename(QString());
  }
};

QTEST_MAIN(TstComfyRun)
#include "tst_comfy_run.moc"
