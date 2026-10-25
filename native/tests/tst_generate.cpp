// 15-GEN Part F: comfy.graph.saveAsTemplate, generate.fromGraph, readiness gating,
// pending/collect flow, variants undo grouping, audio/video wrapper paths, and
// Part F comprehensive integration suite. Runs against the fake ComfyUI server.

#include "comfy_fake_server.h"
#include "comfy/comfy_router_client.h"
#include "core/schema.h"
#include "engine/engine.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <vector>

using namespace sf;
using namespace sf::engine;

QString Q(const char* s) { return QString::fromLatin1(s); }

class TstGenerate : public QObject {
  Q_OBJECT

  std::unique_ptr<FakeComfyServer> fake_;
  std::unique_ptr<QTemporaryDir> userTemplatesDir_;
  std::unique_ptr<QTemporaryDir> jobsCacheDir_;
  std::unique_ptr<Engine> engine_;

private:
  // call + expect ok; returns the response value as an object.
  QJsonObject ok(const QString& cmd, const QJsonObject& args = {}) {
    Result r = engine_->call(cmd, args);
    if (!r.ok) {
      QTest::qFail(qPrintable(QStringLiteral("%1 failed: %2 (%3)")
                                  .arg(cmd, r.error.message, r.error.code)),
                   __FILE__, __LINE__);
    }
    return r.value.toObject();
  }

  // call without expectations
  Result tryCall(const QString& cmd, const QJsonObject& args = {}) {
    return engine_->call(cmd, args);
  }

  QString fixturesDir() const {
#ifdef SF_COMFY_FIXTURES
    return QStringLiteral(SF_COMFY_FIXTURES);
#else
    return QStringLiteral("fixtures/comfy");
#endif
  }

  // Load the fixture workflow graph as an open document with a known name.
  QString loadGraph(const QString& name) {
    return ok(Q("comfy.graph.load"), {{
                   Q("path"), fixturesDir() + QStringLiteral("/txt2img_api.json")
                 }, {Q("name"), name}})
        .value(Q("graphId")).toString();
  }

  int timelineItemCount() {
    const QJsonObject tl = ok(Q("timeline.get"), {{Q("mode"), Q("summary")}});
    int n = 0;
    for (const auto& tv : tl.value(Q("tracks")).toArray())
      n += tv.toObject().value(Q("items")).toArray().size();
    return n;
  }

  QJsonObject lastHistoryRecord() {
    const QJsonArray hist = ok(Q("generate.history")).value(Q("history")).toArray();
    return hist.isEmpty() ? QJsonObject() : hist.last().toObject();
  }

  // one-file requires.models object; `file` is looked for in every combo option of the schema
  QJsonObject modelReq(const QString& file) {
    return QJsonObject{{Q("models"), QJsonArray{QJsonObject{
        {{Q("folder"), Q("checkpoints")}, {Q("file"), file}}}}}};
  }

  const Asset* findAsset(const QString& path) {
    if (!engine_ || !engine_->session(QString()) || !engine_->session(QString())->project) return nullptr;
    const auto& assets = engine_->session(QString())->project->assets();
    const QString abs = QFileInfo(path).absoluteFilePath();
    for (const auto& a : assets) {
      if (a.path == path || a.path == abs || QFileInfo(a.path).absoluteFilePath() == abs) {
        return &a;
      }
    }
    return nullptr;
  }

private slots:
  void initTestCase() {
    QVERIFY(QFile::exists(fixturesDir() + QStringLiteral("/txt2img_api.json")));
    QVERIFY(QFile::exists(fixturesDir() + QStringLiteral("/object_info_min.json")));

    fake_ = std::make_unique<FakeComfyServer>();
    qputenv("SF_COMFY_URL", fake_->url().toString().toUtf8());

    userTemplatesDir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(userTemplatesDir_->isValid());
    qputenv("SF_COMFY_TEMPLATES", userTemplatesDir_->path().toUtf8());

    jobsCacheDir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(jobsCacheDir_->isValid());
    qputenv("SF_JOBS_CACHE_DIR", jobsCacheDir_->path().toUtf8());

    // Router commands point at the local fake Router API (15-GEN Part G).
    qputenv("COMFY_ROUTER_BASE_URL", fake_->url().toString().toUtf8());
  }

  void init() {
    engine_ = std::make_unique<Engine>();
    ok(Q("project.new"), {{
        Q("name"), QStringLiteral("TstGenerateProj")
      }, {Q("width"), 1920}, {Q("height"), 1080}, {Q("fps"), 30}});
    // Save the project so `project.path()` is non-empty: provenance records are written next to
    // the project file (appendProvenanceRecord bails on a pathless project).
    ok(Q("project.saveAs"), {{Q("path"), userTemplatesDir_->path() + QStringLiteral("/project.json")}});
  }

  void cleanup() {
    if (fake_) {
      fake_->setOutputKind(Q("image"));
      fake_->setOutputFilename(QString());
      fake_->setObjectInfoPath(QString());
      fake_->setObjectInfoJson(QJsonObject());
      fake_->setRouterAuthFail(false);
      fake_->setRouter5xx(false);
      qputenv("COMFY_ROUTER_BASE_URL", fake_->url().toString().toUtf8());
    }
    qunsetenv("COMFY_API_KEY");
    engine_.reset();
  }

  // =========================================================================
  // Part F: 10 Core Test Cases
  // =========================================================================

  // 1. generate.ready reports a missing node and a missing model correctly; generate.music then fails not_ready.
  void test01_readyReportsMissingNodeAndModelAndMusicFailsNotReady() {
    const QString gid = loadGraph(QStringLiteral("SrcMusicReq"));
    ok(Q("comfy.graph.saveAsTemplate"), {{
        Q("graphId"), gid}, {Q("id"), QStringLiteral("tmpl_music_unmet")},
        {Q("name"), QStringLiteral("Unmet Music Template")}, {Q("kind"), QStringLiteral("music")},
        {Q("outputKind"), QStringLiteral("audio")},
        {Q("requires"), QJsonObject{
            {Q("nodes"), QJsonArray{QStringLiteral("MissingMusicNode")}},
            {Q("models"), QJsonArray{QJsonObject{{Q("folder"), Q("checkpoints")}, {Q("file"), QStringLiteral("unmet_music.safetensors")}}}}
        }}});

    const size_t queuedBefore = fake_->queued.size();

    // Call generate.ready
    const QJsonObject r = ok(Q("generate.ready"), {{Q("templateId"), QStringLiteral("tmpl_music_unmet")}});
    QCOMPARE(r.value(Q("ok")).toBool(), false);
    const QJsonArray missingNodes = r.value(Q("missingNodes")).toArray();
    QCOMPARE(missingNodes.size(), 1);
    QCOMPARE(missingNodes.at(0).toString(), QStringLiteral("MissingMusicNode"));

    const QJsonArray missingModels = r.value(Q("missingModels")).toArray();
    QCOMPARE(missingModels.size(), 1);
    QCOMPARE(missingModels.at(0).toObject().value(Q("file")).toString(), QStringLiteral("unmet_music.safetensors"));
    QVERIFY(r.value(Q("reason")).toString().contains(QStringLiteral("MissingMusicNode")));
    QVERIFY(r.value(Q("reason")).toString().contains(QStringLiteral("unmet_music.safetensors")));

    // generate.music fails not_ready and queues nothing
    const Result rGen = tryCall(Q("generate.music"), {{
        Q("templateId"), QStringLiteral("tmpl_music_unmet")},
        {Q("prompt"), QStringLiteral("jazz piano")},
        {Q("durationSec"), 30.0}});
    QVERIFY(!rGen.ok);
    QCOMPARE(rGen.error.code, Q("not_ready"));
    QVERIFY(rGen.error.message.contains(QStringLiteral("MissingMusicNode")) ||
            rGen.error.message.contains(QStringLiteral("unmet_music.safetensors")));
    QCOMPARE(fake_->queued.size(), queuedBefore);
  }

  // 2. generate.speech lands an audio item on an audio track, as one undo step; undo removes it.
  void test02_generateSpeechLandsAudioOnAudioTrackUndoRemoves() {
    const QString gid = loadGraph(QStringLiteral("SprGraph2"));
    ok(Q("comfy.graph.saveAsTemplate"), {{
        Q("graphId"), gid}, {Q("id"), QStringLiteral("speech_t2")}, {Q("name"), QStringLiteral("Speech T2")},
        {Q("kind"), QStringLiteral("speech")},
        {Q("outputKind"), QStringLiteral("audio")},
        {Q("bindings"), QJsonObject{
            {Q("text"), QJsonObject{{Q("node"), Q("6")}, {Q("input"), Q("text")}}},
            {Q("seed"), QJsonObject{{Q("node"), Q("3")}, {Q("input"), Q("seed")}}}}}});

    fake_->setOutputKind(Q("audio"));
    const int countBefore = timelineItemCount();
    const QJsonObject r = ok(Q("generate.speech"), {{
        Q("templateId"), QStringLiteral("speech_t2")},
        {Q("text"), QStringLiteral("audiobook narration voice")},
        {Q("wait"), true}});

    const QJsonArray files = r.value(Q("files")).toArray();
    QCOMPARE(files.size(), 1);
    const QString wav = files.at(0).toString();
    QVERIFY(wav.endsWith(QStringLiteral(".wav"), Qt::CaseInsensitive));
    QVERIFY(QFileInfo::exists(wav));

    // verify landed on an audio track
    QCOMPARE(timelineItemCount(), countBefore + 1);
    const QJsonObject created = r.value(Q("created")).toObject();
    const QJsonArray createdItems = created.value(Q("items")).toArray();
    QCOMPARE(createdItems.size(), 1);
    const QString itemId = createdItems.at(0).toString();
    const QJsonObject itemGet = ok(Q("item.get"), {{Q("itemId"), itemId}});
    QCOMPARE(itemGet.value(Q("type")).toString(), QStringLiteral("audio"));

    // One undo step removes it
    QCOMPARE(ok(Q("history.undo")).value(Q("steps")).toInt(), 1);
    QCOMPARE(timelineItemCount(), countBefore);
  }

  // 3. generate.music respects durationSec binding injection (check the submitted graph JSON).
  void test03_generateMusicRespectsDurationSecBindingInjection() {
    const QString gid = loadGraph(QStringLiteral("MusicGraph"));
    ok(Q("comfy.graph.saveAsTemplate"), {{
        Q("graphId"), gid}, {Q("id"), QStringLiteral("music_t3")}, {Q("name"), QStringLiteral("Music T3")},
        {Q("kind"), QStringLiteral("music")},
        {Q("outputKind"), QStringLiteral("audio")},
        {Q("bindings"), QJsonObject{
            {Q("prompt"), QJsonObject{{Q("node"), Q("6")}, {Q("input"), Q("text")}}},
            {Q("durationSec"), QJsonObject{{Q("node"), Q("5")}, {Q("input"), Q("width")}}},
            {Q("seed"), QJsonObject{{Q("node"), Q("3")}, {Q("input"), Q("seed")}}}}}});

    fake_->setOutputKind(Q("audio"));
    const size_t queuedBefore = fake_->queued.size();
    const QJsonObject r = ok(Q("generate.music"), {{
        Q("templateId"), QStringLiteral("music_t3")},
        {Q("prompt"), QStringLiteral("ambient synth wave")},
        {Q("durationSec"), 42.0},
        {Q("wait"), true}});

    QCOMPARE(fake_->queued.size(), queuedBefore + 1);
    const QJsonObject submittedGraph = fake_->queued.back();
    // node "5" input "width" was bound to durationSec
    const int boundVal = submittedGraph.value(Q("5")).toObject().value(Q("inputs")).toObject().value(Q("width")).toInt();
    QCOMPARE(boundVal, 42);
  }

  // 4. generate.image placeOnTimeline:false gives a file and no timeline change.
  void test04_generateImagePlaceOnTimelineFalse() {
    fake_->setOutputKind(Q("image"));
    const int countBefore = timelineItemCount();
    const QJsonObject r = ok(Q("generate.image"), {{
        Q("templateId"), QStringLiteral("sd15_txt2img")},
        {Q("prompt"), QStringLiteral("misty forest at dawn")},
        {Q("placeOnTimeline"), false},
        {Q("wait"), true}});

    const QJsonArray files = r.value(Q("files")).toArray();
    QCOMPARE(files.size(), 1);
    const QString landed = files.at(0).toString();
    QVERIFY(QFileInfo::exists(landed));

    // No timeline items created
    QCOMPARE(r.value(Q("created")).toObject().value(Q("items")).toArray().size(), 0);
    QCOMPARE(timelineItemCount(), countBefore);
  }

  // 5. generate.thumbnail with title gives a 1280x720 PNG.
  void test05_generateThumbnailWithTitleGives1280x720Png() {
    fake_->setOutputKind(Q("image"));
    const int countBefore = timelineItemCount();
    const QJsonObject r = ok(Q("generate.thumbnail"), {{
        Q("templateId"), QStringLiteral("sd15_txt2img")},
        {Q("prompt"), QStringLiteral("cyberpunk city thumbnail")},
        {Q("title"), QStringLiteral("SplitFrame Ep 1")},
        {Q("name"), QStringLiteral("ep1_thumb")}});

    const QJsonArray files = r.value(Q("files")).toArray();
    QCOMPARE(files.size(), 1);
    const QString thumbPath = files.at(0).toString();
    QVERIFY(QFileInfo::exists(thumbPath));
    QVERIFY(thumbPath.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive));

    const QImage img(thumbPath);
    QVERIFY(!img.isNull());
    QCOMPARE(img.size(), QSize(1280, 720));

    // Never touches timeline
    QCOMPARE(timelineItemCount(), countBefore);
  }

  // 6. Provenance: the line in provenance.jsonl matches the item metadata; workflow hash matches sha256 of the submitted graph.
  void test06_provenanceJsonlMatchesAssetMetadataAndHash() {
    fake_->setOutputKind(Q("image"));
    const QJsonObject r = ok(Q("generate.image"), {{
        Q("templateId"), QStringLiteral("sd15_txt2img")},
        {Q("prompt"), QStringLiteral("provenance audit test")},
        {Q("wait"), true}});

    const QJsonArray files = r.value(Q("files")).toArray();
    QCOMPARE(files.size(), 1);
    const QString landedFile = files.at(0).toString();

    // Check media pool asset metadata provenance
    const Asset* as = findAsset(landedFile);
    QVERIFY(as != nullptr);
    QVERIFY(as->metadata.provenance.has_value());
    const QJsonObject assetProv = as->metadata.provenance.value();

    // Read provenance.jsonl
    const QString projPath = engine_->session(QString())->project->path();
    QFile pf(QFileInfo(projPath).dir().filePath(QStringLiteral("generated/provenance.jsonl")));
    QVERIFY(pf.open(QIODevice::ReadOnly | QIODevice::Text));
    QJsonObject lastLineObj;
    while (!pf.atEnd()) {
      const QByteArray line = pf.readLine().trimmed();
      if (!line.isEmpty()) {
        lastLineObj = QJsonDocument::fromJson(line).object();
      }
    }
    pf.close();
    QVERIFY(!lastLineObj.isEmpty());

    // Matches line in provenance.jsonl
    QCOMPARE(lastLineObj.value(Q("outputFile")).toString(), assetProv.value(Q("outputFile")).toString());
    QCOMPARE(lastLineObj.value(Q("seed")).toVariant().toLongLong(), assetProv.value(Q("seed")).toVariant().toLongLong());
    QCOMPARE(lastLineObj.value(Q("workflowHash")).toString(), assetProv.value(Q("workflowHash")).toString());

    // workflow hash matches sha256 of the submitted graph
    const QJsonObject submitted = fake_->queued.back();
    const QString expectedHash = QString::fromUtf8(
        QCryptographicHash::hash(QJsonDocument(submitted).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    QCOMPARE(assetProv.value(Q("workflowHash")).toString(), expectedHash);
  }

  // 7. variants:3 gives 3 seeds and 3 assets.
  void test07_variantsThreeSeedsThreeAssets() {
    fake_->setOutputKind(Q("image"));
    const int countBefore = timelineItemCount();
    const size_t assetsBefore = engine_->session(QString())->project->assets().size();

    const QJsonObject r = ok(Q("generate.image"), {{
        Q("templateId"), QStringLiteral("sd15_txt2img")},
        {Q("prompt"), QStringLiteral("three trees in a meadow")},
        {Q("seed"), 700},
        {Q("variants"), 3},
        {Q("wait"), true}});

    const QJsonArray files = r.value(Q("files")).toArray();
    QCOMPARE(files.size(), 3);
    QCOMPARE(engine_->session(QString())->project->assets().size(), assetsBefore + 3);
    QCOMPARE(timelineItemCount(), countBefore + 3);

    // Read history to verify the 3 seeds
    const QJsonArray hist = ok(Q("generate.history")).value(Q("history")).toArray();
    QVERIFY(hist.size() >= 3);
    const QJsonObject h1 = hist.at(hist.size() - 3).toObject();
    const QJsonObject h2 = hist.at(hist.size() - 2).toObject();
    const QJsonObject h3 = hist.at(hist.size() - 1).toObject();

    QCOMPARE(h1.value(Q("seed")).toVariant().toLongLong(), 700LL);
    QCOMPARE(h2.value(Q("seed")).toVariant().toLongLong(), 701LL);
    QCOMPARE(h3.value(Q("seed")).toVariant().toLongLong(), 702LL);

    // One undo step removes all 3 variants from timeline
    QCOMPARE(ok(Q("history.undo")).value(Q("steps")).toInt(), 1);
    QCOMPARE(timelineItemCount(), countBefore);
  }

  // 8. fromTemplate, then one comfy.graph.op set, then saveAsTemplate, then generate.fromGraph all work.
  //    An invalid graph fails at the validate step and queues nothing.
  void test08_fromTemplateEditSaveAndRunPipeline() {
    const size_t queuedBefore = fake_->queued.size();

    // 1. fromTemplate with only { templateId } (no params)
    const QJsonObject rTmpl = ok(Q("comfy.graph.fromTemplate"), {{Q("templateId"), QStringLiteral("sd15_txt2img")}});
    const QString gid = rTmpl.value(Q("graphId")).toString();
    QVERIFY(!gid.isEmpty());

    // 2. comfy.graph.op set on node 6 (positive prompt)
    const QJsonArray ops = {
      QJsonObject{{QStringLiteral("op"), QStringLiteral("set")},
                  {QStringLiteral("id"), QStringLiteral("6")},
                  {QStringLiteral("input"), QStringLiteral("text")},
                  {QStringLiteral("value"), QStringLiteral("emerald mountains under aurora")}}
    };
    const QJsonObject rOp = ok(Q("comfy.graph.op"), {{Q("graphId"), gid}, {Q("ops"), ops}});
    QCOMPARE(rOp.value(Q("graphId")).toString(), gid);

    // 3. saveAsTemplate
    ok(Q("comfy.graph.saveAsTemplate"), {{
        Q("graphId"), gid}, {Q("id"), QStringLiteral("aurora_tmpl")},
        {Q("name"), QStringLiteral("Aurora Template")}, {Q("kind"), Q("image")},
        {Q("outputNode"), QStringLiteral("9")}});

    // Empty graph save fails
    const QString emptyGid = ok(Q("comfy.graph.new")).value(Q("graphId")).toString();
    const Result rEmpty = tryCall(Q("comfy.graph.saveAsTemplate"), {{
        Q("graphId"), emptyGid}, {Q("id"), QStringLiteral("empty_tmpl")},
        {Q("name"), QStringLiteral("Empty")}, {Q("kind"), Q("image")}});
    QVERIFY(!rEmpty.ok);
    QCOMPARE(rEmpty.error.code, Q("invalid_params"));

    // 4. generate.fromGraph directly from edited graph gid
    const QJsonObject rGen = ok(Q("generate.fromGraph"), {{
        Q("graphId"), gid}, {Q("kind"), Q("image")}, {Q("wait"), true}});
    QCOMPARE(rGen.value(Q("files")).toArray().size(), 1);
    QCOMPARE(fake_->queued.size(), queuedBefore + 1);

    // Provenance: templateId is null, graphId and graphName recorded
    const QJsonObject rec = lastHistoryRecord();
    QVERIFY(rec.value(Q("templateId")).isNull());
    QCOMPARE(rec.value(Q("graphId")).toString(), gid);
    QCOMPARE(rec.value(Q("outputKind")).toString(), QStringLiteral("image"));

    // 5. Invalid graph fails at validation step and queues nothing
    const QJsonArray breakOps = {
      QJsonObject{{QStringLiteral("op"), QStringLiteral("disconnect")},
                  {QStringLiteral("to"), QStringLiteral("9")},
                  {QStringLiteral("input"), QStringLiteral("images")}}
    };
    ok(Q("comfy.graph.op"), {{Q("graphId"), gid}, {Q("ops"), breakOps}});

    const size_t queuedBeforeInvalid = fake_->queued.size();
    const Result rInvalid = tryCall(Q("generate.fromGraph"), {{
        Q("graphId"), gid}, {Q("kind"), Q("image")}, {Q("wait"), true}});
    QVERIFY(!rInvalid.ok);
    QCOMPARE(rInvalid.error.code, Q("invalid"));
    QCOMPARE(fake_->queued.size(), queuedBeforeInvalid);
  }

  // 9. A filename with ../ from the server is sanitised.
  void test09_filenameWithDotDotSanitised() {
    fake_->setOutputKind(Q("image"));
    fake_->setOutputFilename(QStringLiteral("../../secret_dump.png"));

    const QJsonObject r = ok(Q("generate.image"), {{
        Q("templateId"), QStringLiteral("sd15_txt2img")},
        {Q("prompt"), QStringLiteral("safe file path test")},
        {Q("wait"), true}});

    const QJsonArray files = r.value(Q("files")).toArray();
    QCOMPARE(files.size(), 1);
    const QString downloaded = files.at(0).toString();
    QVERIFY(QFileInfo::exists(downloaded));

    // Filename does not escape cache dir and has no '..'
    QVERIFY(!downloaded.contains(QStringLiteral("..")));
    const QString fn = QFileInfo(downloaded).fileName();
    QVERIFY(!fn.contains(QStringLiteral("/")));
    QVERIFY(!fn.contains(QStringLiteral("\\")));
    QVERIFY(fn.endsWith(QStringLiteral("secret_dump.png")));
  }

  // 10. Old template JSON (v1) still loads, and generate.shot still works.
  void test10_oldTemplateV1LoadsAndGenerateShotWorks() {
    fake_->setOutputKind(Q("image"));

    // Verify v1 template (sd15_txt2img has no outputKind or requires) loads in template.list
    bool foundV1 = false;
    for (const auto& tv : ok(Q("comfy.template.list")).value(Q("templates")).toArray()) {
      const QJsonObject t = tv.toObject();
      if (t.value(Q("id")).toString() == QLatin1String("sd15_txt2img")) {
        foundV1 = true;
        // outputKind defaults to image derived from kind
        QCOMPARE(t.value(Q("outputKind")).toString(), QStringLiteral("image"));
        break;
      }
    }
    QVERIFY2(foundV1, "v1 template sd15_txt2img missing from comfy.template.list");

    const int countBefore = timelineItemCount();
    const QJsonObject rShot = ok(Q("generate.shot"), {{
        Q("templateId"), QStringLiteral("sd15_txt2img")},
        {Q("prompt"), QStringLiteral("classic v1 shot")},
        {Q("wait"), true}});

    const QJsonArray files = rShot.value(Q("files")).toArray();
    QCOMPARE(files.size(), 1);
    QVERIFY(QFileInfo::exists(files.at(0).toString()));
    QCOMPARE(timelineItemCount(), countBefore + 1);
  }

  // =========================================================================
  // Additional Supporting Test Cases
  // =========================================================================

  // generate.fromGraph: happy path
  void fromGraphHappyPath() {
    const QString gid = loadGraph(QStringLiteral("GusGraph"));
    const size_t queuedBefore = fake_->queued.size();

    const QJsonObject r = ok(Q("generate.fromGraph"), {{
        Q("graphId"), gid}, {Q("kind"), Q("image")},
        {Q("prompt"), QStringLiteral("a calm lake")}, {Q("wait"), true}});
    QCOMPARE(r.value(Q("pending")).toBool(), false);
    QCOMPARE(r.value(Q("files")).toArray().size(), 1);
    const QString landed = r.value(Q("files")).toArray().at(0).toString();
    QVERIFY(QFileInfo::exists(landed));
    QCOMPARE(r.value(Q("created")).toObject().value(Q("items")).toArray().size(), 1);
    QCOMPARE(fake_->queued.size(), queuedBefore + 1);

    // provenance: templateId is the document name; outputKind/seed/workflowHash recorded
    const QJsonObject rec = lastHistoryRecord();
    QVERIFY(rec.value(Q("templateId")).isNull());
    QCOMPARE(rec.value(Q("graphId")).toString(), gid);
    QCOMPARE(rec.value(Q("graphName")).toString(), QStringLiteral("GusGraph"));
    QCOMPARE(rec.value(Q("outputKind")).toString(), QStringLiteral("image"));
    QVERIFY(rec.contains(Q("seed")));
    QVERIFY(!rec.value(Q("workflowHash")).toString().isEmpty());

    // one undo group removes the landed item
    QCOMPARE(timelineItemCount(), 1);
    QCOMPARE(ok(Q("history.undo")).value(Q("steps")).toInt(), 1);
    QCOMPARE(timelineItemCount(), 0);
  }

  // comfy.graph.saveAsTemplate round-trip
  void saveAsTemplateRoundTrip() {
    const QString gid = loadGraph(QStringLiteral("SrcDoc"));
    const QJsonObject saved = ok(Q("comfy.graph.saveAsTemplate"), {{
        Q("graphId"), gid}, {Q("id"), QStringLiteral("gusr_img")},
        {Q("name"), QStringLiteral("GUSR Image")}, {Q("kind"), Q("image")},
        {Q("description"), QStringLiteral("roundtrip")}});

    QCOMPARE(saved.value(Q("id")).toString(), QStringLiteral("gusr_img"));
    QCOMPARE(saved.value(Q("outputKind")).toString(), QStringLiteral("image"));
    const QString outPath = saved.value(Q("path")).toString();
    QVERIFY(outPath.startsWith(userTemplatesDir_->path()));
    QVERIFY(QFile::exists(outPath));

    // the file holds the v2 schema keys
    QFile f(outPath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QJsonParseError pe;
    const QJsonObject doc = QJsonDocument::fromJson(f.readAll(), &pe).object();
    QCOMPARE(pe.error, QJsonParseError::NoError);
    QCOMPARE(doc.value(Q("id")).toString(), QStringLiteral("gusr_img"));
    QCOMPARE(doc.value(Q("outputKind")).toString(), QStringLiteral("image"));
    QCOMPARE(doc.value(Q("needsUserModel")).toBool(), false);
    QVERIFY(doc.value(Q("requires")).isObject());
    QCOMPARE(doc.value(Q("outputNode")).toString(), QStringLiteral("9")); // last node default
    f.close();

    // template.list sees it as a user template
    bool found = false;
    for (const auto& tv : ok(Q("comfy.template.list")).value(Q("templates")).toArray()) {
      const QJsonObject t = tv.toObject();
      if (t.value(Q("id")).toString() == QLatin1String("gusr_img")) {
        found = true;
        QCOMPARE(t.value(Q("source")).toString(), QStringLiteral("user"));
        QCOMPARE(t.value(Q("outputKind")).toString(), QStringLiteral("image"));
        break;
      }
    }
    QVERIFY2(found, "gusr_img missing from comfy.template.list");

    // bindings pass through: fromTemplate on the saved id binds prompt/seed/width
    const QString fgid = ok(Q("comfy.graph.fromTemplate"), {{
                               Q("templateId"), QStringLiteral("gusr_img")},
        {Q("params"), QJsonObject{{Q("prompt"), QStringLiteral("a red fox")},
            {Q("seed"), 7}, {Q("width"), 640}}}})
        .value(Q("graphId")).toString();
    const QJsonObject graph = ok(Q("comfy.graph.get"), {{Q("graphId"), fgid}})
                                   .value(Q("graph")).toObject();
    QCOMPARE(graph.value(Q("6")).toObject().value(Q("inputs")).toObject().value(Q("text")).toString(),
             QStringLiteral("a red fox"));
    QCOMPARE(graph.value(Q("3")).toObject().value(Q("inputs")).toObject().value(Q("seed")).toInt(), 7);
    QCOMPARE(graph.value(Q("5")).toObject().value(Q("inputs")).toObject().value(Q("width")).toInt(), 640);
  }

  // saveAsTemplate validation: id regex, kind, outputKind, bindings, outputNode, requires.
  void saveAsTemplateValidation() {
    const QString gid = loadGraph(QStringLiteral("SrcDoc2"));

    QJsonArray arr{Q("a"), 2};
    struct Bad {
      QString why;
      QJsonObject args;
    };
    const std::vector<Bad> bads = {
      {Q("no graphId"),
       {{Q("id"), QStringLiteral("z1")}, {Q("name"), Q("N")}, {Q("kind"), Q("image")}}},
      {Q("bad id"),
       {{Q("graphId"), gid}, {Q("id"), QStringLiteral("bad id!")}, {Q("name"), Q("N")}, {Q("kind"), Q("image")}}},
      {Q("bad kind"),
       {{Q("graphId"), gid}, {Q("id"), QStringLiteral("z2")}, {Q("name"), Q("N")}, {Q("kind"), Q("sfx")}}},
      {Q("bad outputKind"),
       {{Q("graphId"), gid}, {Q("id"), QStringLiteral("z3")}, {Q("name"), Q("N")}, {Q("kind"), Q("image")},
        {Q("outputKind"), Q("sfx")}}},
      {Q("binding name not allowed"),
       {{Q("graphId"), gid}, {Q("id"), QStringLiteral("z4")}, {Q("name"), Q("N")}, {Q("kind"), Q("image")},
        {Q("bindings"), QJsonObject{{Q("wrong"), QJsonObject{{Q("node"), Q("6")}, {Q("input"), Q("text")}}}}}}},
      {Q("binding not an object"),
       {{Q("graphId"), gid}, {Q("id"), QStringLiteral("z5")}, {Q("name"), Q("N")}, {Q("kind"), Q("image")},
        {Q("bindings"), QJsonObject{{Q("prompt"), Q("text")}}}}},
      {Q("binding node missing"),
       {{Q("graphId"), gid}, {Q("id"), QStringLiteral("z6")}, {Q("name"), Q("N")}, {Q("kind"), Q("image")},
        {Q("bindings"), QJsonObject{{Q("prompt"), QJsonObject{{Q("node"), Q("555")}, {Q("input"), Q("text")}}}}}}},
      {Q("binding input missing"),
       {{Q("graphId"), gid}, {Q("id"), QStringLiteral("z7")}, {Q("name"), Q("N")}, {Q("kind"), Q("image")},
        {Q("bindings"), QJsonObject{{Q("prompt"), QJsonObject{{Q("node"), Q("6")}, {Q("input"), Q("nope")}}}}}}},
      {Q("binding value wrong shape"),
       {{Q("graphId"), gid}, {Q("id"), QStringLiteral("z7b")}, {Q("name"), Q("N")}, {Q("kind"), Q("image")},
        {Q("bindings"), QJsonObject{{Q("seed"), QJsonObject{{Q("node"), Q("3")}}}}}}},
      {Q("outputNode missing"),
       {{Q("graphId"), gid}, {Q("id"), QStringLiteral("z8")}, {Q("name"), Q("N")}, {Q("kind"), Q("image")},
        {Q("outputNode"), Q("999")}}},
      {Q("requires.nodes bad shape"),
       {{Q("graphId"), gid}, {Q("id"), QStringLiteral("z9")}, {Q("name"), Q("N")}, {Q("kind"), Q("image")},
        {Q("requires"), QJsonObject{{Q("nodes"), arr}}}}},
      {Q("requires.models bad shape"),
       {{Q("graphId"), gid}, {Q("id"), QStringLiteral("z10")}, {Q("name"), Q("N")}, {Q("kind"), Q("image")},
        {Q("requires"), QJsonObject{{Q("models"), QJsonArray{Q("file")}}}}}},
    };
    for (const Bad& b : bads) {
      const Result r = tryCall(Q("comfy.graph.saveAsTemplate"), b.args);
      QVERIFY2(!r.ok, qPrintable(b.why));
      QCOMPARE(r.error.code, Q("invalid_params"));
    }

    const Result r404 = tryCall(Q("comfy.graph.saveAsTemplate"), {{
        Q("graphId"), QStringLiteral("g99999")}, {Q("id"), QStringLiteral("z11")},
        {Q("name"), Q("N")}, {Q("kind"), Q("image")}});
    QVERIFY(!r404.ok);
    QCOMPARE(r404.error.code, Q("not_found"));

    ok(Q("comfy.graph.saveAsTemplate"), {{
        Q("graphId"), gid}, {Q("id"), QStringLiteral("zvalid")}, {Q("name"), QStringLiteral("Z")},
        {Q("kind"), Q("image")}, {Q("outputNode"), Q("8")}});
  }

  // requires/needsUserModel round trip through saveAsTemplate and generate.ready
  void saveAsTemplateRequires() {
    const QString gid = loadGraph(QStringLiteral("SrcDocReq"));
    ok(Q("comfy.graph.saveAsTemplate"), {{
        Q("graphId"), gid}, {Q("id"), QStringLiteral("greq")}, {Q("name"), QStringLiteral("GREQ")},
        {Q("kind"), Q("image")},
        {Q("requires"), QJsonObject{
            {{Q("nodes"), QJsonArray{Q("KSampler")}}, {Q("models"), QJsonArray{QJsonObject{{Q("folder"), Q("checkpoints")}, {Q("file"), QStringLiteral("sd15.safetensors")}}}}}}},
        {Q("needsUserModel"), true}});

    const QJsonObject row = ok(Q("generate.ready"), {{Q("templateId"), QStringLiteral("greq")}});
    QCOMPARE(row.value(Q("ok")).toBool(), true);
    QVERIFY(row.value(Q("missingNodes")).toArray().isEmpty());
    QVERIFY(row.value(Q("missingModels")).toArray().isEmpty());
    QCOMPARE(row.value(Q("kind")).toString(), QStringLiteral("image"));

    const QString gid2 = loadGraph(QStringLiteral("SrcDocReq2"));
    ok(Q("comfy.graph.saveAsTemplate"), {{
        Q("graphId"), gid2}, {Q("id"), QStringLiteral("greq2")}, {Q("name"), QStringLiteral("GREQ2")},
        {Q("kind"), Q("image")},
        {Q("requires"), QJsonObject{{Q("models"), QJsonArray{QJsonObject{{Q("folder"), Q("checkpoints")}, {Q("file"), QStringLiteral("unmet.safetensors")}}}}}}});
    const QJsonObject row2 = ok(Q("generate.ready"), {{Q("templateId"), QStringLiteral("greq2")}});
    QCOMPARE(row2.value(Q("ok")).toBool(), false);
    QCOMPARE(row2.value(Q("missingModels")).toArray().size(), 1);
    QCOMPARE(row2.value(Q("missingModels")).toArray().at(0).toObject().value(Q("file")).toString(),
             QStringLiteral("unmet.safetensors"));
    QVERIFY(row2.value(Q("reason")).toString().contains(QStringLiteral("unmet.safetensors")));
  }

  // generate.fromGraph readiness gate
  void fromGraphReadyGate() {
    const QString gid = loadGraph(QStringLiteral("GusGate"));
    const size_t queuedBefore = fake_->queued.size();

    const Result r = tryCall(Q("generate.fromGraph"), {{
        Q("graphId"), gid}, {Q("kind"), Q("image")}, {Q("requires"), modelReq(QStringLiteral("not_installed.safetensors"))}});
    QVERIFY(!r.ok);
    QCOMPARE(r.error.code, Q("not_ready"));
    QVERIFY(r.error.message.contains(QStringLiteral("not_installed.safetensors")));
    QCOMPARE(fake_->queued.size(), queuedBefore);
  }

  // generate.fromGraph argument errors
  void fromGraphArgErrors() {
    const Result rNoArgs = tryCall(Q("generate.fromGraph"), {{Q("kind"), Q("image")}});
    QVERIFY(!rNoArgs.ok);
    QCOMPARE(rNoArgs.error.code, Q("invalid_params"));

    const Result rBadKind = tryCall(Q("generate.fromGraph"),
                                    {{Q("graphId"), QStringLiteral("g1")}, {Q("kind"), Q("sfx")}});
    QVERIFY(!rBadKind.ok);
    QCOMPARE(rBadKind.error.code, Q("invalid_params"));

    const Result r404 = tryCall(Q("generate.fromGraph"),
                                {{Q("graphId"), QStringLiteral("g99999")}, {Q("kind"), Q("image")}});
    QVERIFY(!r404.ok);
    QCOMPARE(r404.error.code, Q("not_found"));

    const Result rT = tryCall(Q("generate.image"),
                              {{Q("templateId"), QStringLiteral("nope_tmpl")}, {Q("prompt"), Q("x")}});
    QVERIFY(!rT.ok);
    QCOMPARE(rT.error.code, Q("not_found"));
  }

  // wait=false returns pending, and generate.collect lands files once done
  void fromGraphPendingCollect() {
    const QString gid = loadGraph(QStringLiteral("GusPending"));
    const QJsonObject r = ok(Q("generate.fromGraph"), {{
        Q("graphId"), gid}, {Q("kind"), Q("image")}, {Q("prompt"), Q("v")}, {Q("wait"), false}});
    QCOMPARE(r.value(Q("pending")).toBool(), true);
    QCOMPARE(r.value(Q("files")).toArray().size(), 0);
    const QString jobId = r.value(Q("jobId")).toString();
    QVERIFY(!jobId.isEmpty());

    QJsonObject fin;
    for (int i = 0; i < 200 && !fin.contains(Q("jobId")); ++i) {
      const Result rr = tryCall(Q("generate.collect"), {{Q("jobId"), jobId}});
      if (rr.ok) fin = rr.value.toObject();
      else QTest::qWait(50);
    }
    QVERIFY2(fin.contains(Q("jobId")), "generate.collect never succeeded");
    QCOMPARE(fin.value(Q("files")).toArray().size(), 1);
    QCOMPARE(fin.value(Q("created")).toObject().value(Q("items")).toArray().size(), 1);
    QCOMPARE(timelineItemCount(), 1);
  }

  // =========================================================================
  // 18-FIX-review.md Part 1 regression tests
  // =========================================================================

  // Item 1: Undo group leak - variants with wait:false must roll back the group on early return.
  // The RAII GroupGuard around runGenerate's variants loop must always close (rollback) the group,
  // even when a variant returns pending before landing.
  void variantsFailWaitFalseRollsBackGroup() {
    const QString gid = loadGraph(QStringLiteral("GusGraph"));
    ok(Q("comfy.graph.saveAsTemplate"), {{
        Q("graphId"), gid}, {Q("id"), QStringLiteral("variants_fail_tmpl")},
        {Q("name"), QStringLiteral("Variants Fail")}, {Q("kind"), Q("image")}});

    Session* sess = engine_->session(QString());
    const int depthBefore = sess->project->groupDepth();

    // variants:2 with wait:false -> first variant returns pending immediately,
    // the RAII group guard must roll back the group.
    const Result r = tryCall(Q("generate.image"), {{
        Q("templateId"), QStringLiteral("variants_fail_tmpl")},
        {Q("prompt"), QStringLiteral("rollback test")},
        {Q("variants"), 2},
        {Q("wait"), false}});

    QCOMPARE(r.value.toObject().value(Q("pending")).toBool(), true);
    QCOMPARE(sess->project->groupDepth(), depthBefore);
    QCOMPARE(sess->project->groupDepth(), 0);
  }

  // Item 4: generate.fromGraph - text/durationSec/bpm are accepted by the schema but, with no
  // binding, must fail with unbound_param and never be silently dropped. Nothing is queued.
  void fromGraphTextDurationSecBpmUnboundIsError() {
    const QString gid = loadGraph(QStringLiteral("GusGraph"));
    ok(Q("comfy.graph.saveAsTemplate"), {{
        Q("graphId"), gid}, {Q("id"), QStringLiteral("unbound_tmpl")},
        {Q("name"), QStringLiteral("Unbound Template")}, {Q("kind"), Q("image")}});

    const size_t queuedBefore = fake_->queued.size();

    const Result rText = tryCall(Q("generate.fromGraph"), {{
        Q("graphId"), gid}, {Q("kind"), Q("image")},
        {Q("text"), QStringLiteral("should not bind")}});
    QVERIFY(!rText.ok);
    QCOMPARE(rText.error.code, Q("unbound_param"));

    const Result rDur = tryCall(Q("generate.fromGraph"), {{
        Q("graphId"), gid}, {Q("kind"), Q("image")},
        {Q("durationSec"), 42.0}});
    QVERIFY(!rDur.ok);
    QCOMPARE(rDur.error.code, Q("unbound_param"));

    const Result rBpm = tryCall(Q("generate.fromGraph"), {{
        Q("graphId"), gid}, {Q("kind"), Q("image")},
        {Q("bpm"), 120.0}});
    QVERIFY(!rBpm.ok);
    QCOMPARE(rBpm.error.code, Q("unbound_param"));

    QCOMPARE(fake_->queued.size(), queuedBefore);
  }

  // Item 4: generate.fromGraph - prompt and negative are bound by following KSampler
  // positive/negative links, not by sorted node id. In txt2img_api.json node 6 is the positive
  // CLIPTextEncode and node 7 is the negative; the positive prompt must land on node 6.
  void fromGraphPromptNegativeBoundViaKSamplerLinks() {
    const QString gid = loadGraph(QStringLiteral("GusGraph"));
    ok(Q("comfy.graph.saveAsTemplate"), {{
        Q("graphId"), gid}, {Q("id"), QStringLiteral("links_tmpl")},
        {Q("name"), QStringLiteral("Links Template")}, {Q("kind"), Q("image")}});

    fake_->setOutputKind(Q("image"));

    const QJsonObject r = ok(Q("generate.fromGraph"), {{
        Q("graphId"), gid}, {Q("kind"), Q("image")},
        {Q("prompt"), QStringLiteral("positive prompt text")},
        {Q("negative"), QStringLiteral("negative prompt text")},
        {Q("wait"), true}});

    QCOMPARE(r.value(Q("pending")).toBool(), false);
    QCOMPARE(r.value(Q("files")).toArray().size(), 1);

    // The submitted graph must have prompt bound to node 6 (positive via KSampler link)
    // and negative bound to node 7 (negative via KSampler link).
    const QJsonObject submitted = fake_->queued.back();
    const QJsonObject node6 = submitted.value("6").toObject().value("inputs").toObject();
    const QJsonObject node7 = submitted.value("7").toObject().value("inputs").toObject();
    QCOMPARE(node6.value("text").toString(), QStringLiteral("positive prompt text"));
    QCOMPARE(node7.value("text").toString(), QStringLiteral("negative prompt text"));
  }

  // video wrapper guards
  void videoWrapperGuards() {
    const Result rKind = tryCall(Q("generate.video"),
                                 {{Q("templateId"), QStringLiteral("sd15_txt2img")}, {Q("prompt"), Q("x")}, {Q("wait"), true}});
    QVERIFY(!rKind.ok);
    QCOMPARE(rKind.error.code, Q("invalid_params"));
    QVERIFY(rKind.error.message.contains(QStringLiteral("produces image")));

    const QString gid = loadGraph(QStringLiteral("GusVid"));
    ok(Q("comfy.graph.saveAsTemplate"), {{
        Q("graphId"), gid}, {Q("id"), QStringLiteral("vt")}, {Q("name"), QStringLiteral("VT")},
        {Q("kind"), Q("video")}});
    fake_->setOutputFilename(QStringLiteral("out_1.txt"));
    const size_t queuedBefore = fake_->queued.size();
    const Result rExt = tryCall(Q("generate.video"),
                                 {{Q("templateId"), QStringLiteral("vt")}, {Q("prompt"), Q("clip")}, {Q("wait"), true}});
    QVERIFY(!rExt.ok);
    QVERIFY(rExt.error.message.contains(QStringLiteral("unexpected extension")));
    QCOMPARE(fake_->queued.size(), queuedBefore + 1);
  }

  // builtin image_txt2img is listed and ready
  void builtinImageTemplate() {
    bool found = false;
    for (const auto& tv : ok(Q("comfy.template.list")).value(Q("templates")).toArray()) {
      const QJsonObject t = tv.toObject();
      if (t.value(Q("id")).toString() == QLatin1String("image_txt2img")) {
        found = true;
        QCOMPARE(t.value(Q("kind")).toString(), QStringLiteral("image"));
        QCOMPARE(t.value(Q("outputKind")).toString(), QStringLiteral("image"));
        QCOMPARE(t.value(Q("source")).toString(), QStringLiteral("builtin"));
        QCOMPARE(t.value(Q("needsUserModel")).toBool(), true);
        QVERIFY(t.value(Q("bindings")).toArray().contains(Q("prompt")));
        QVERIFY(t.value(Q("requires")).toObject().value(Q("models")).toArray().size() >= 1);
        break;
      }
    }
    QVERIFY2(found, "builtin image_txt2img missing from comfy.template.list");

    const QJsonObject row = ok(Q("generate.ready"), {{Q("templateId"), QStringLiteral("image_txt2img")}});
    QCOMPARE(row.value(Q("ok")).toBool(), true);
    QVERIFY(row.value(Q("missingNodes")).toArray().isEmpty());
    QVERIFY(row.value(Q("missingModels")).toArray().isEmpty());

    const Result r = tryCall(Q("generate.ready"), {{Q("templateId"), QStringLiteral("nope_x")}});
    QVERIFY(!r.ok);
    QCOMPARE(r.error.code, Q("not_found"));
  }

  // =========================================================================
  // Part G: Comfy Router Provider Tests
  // =========================================================================

  // Key handling
  void routerKeyConfig() {
    qunsetenv("COMFY_API_KEY");
    const QJsonObject info0 = ok(Q("comfy.router.info"));
    QCOMPARE(info0.value(Q("keyConfigured")).toBool(), false);
    const Result rNoKey = tryCall(Q("generate.router.models"), {});
    QVERIFY(!rNoKey.ok);
    QCOMPARE(rNoKey.error.code, Q("not_configured"));

    // generate.ready router mode without key
    const Result rReadyNoKey = tryCall(Q("generate.ready"), {{Q("provider"), QStringLiteral("router")}, {Q("model"), QStringLiteral("test/one")}});
    QVERIFY(!rReadyNoKey.ok);
    QCOMPARE(rReadyNoKey.error.code, Q("not_configured"));

    qputenv("COMFY_API_KEY", "test-key-g");
    const QJsonObject info = ok(Q("comfy.router.info"));
    QCOMPARE(info.value(Q("keyConfigured")).toBool(), true);
    const QJsonObject cat = ok(Q("generate.router.models"), {{Q("limit"), 20}});
    QCOMPARE(cat.value(Q("models")).toArray().size(), 2);
    QCOMPARE(cat.value(Q("hasMore")).toBool(), false);
    QCOMPARE(fake_->routerApiKey(), QStringLiteral("test-key-g"));

    // generate.ready router mode with key and known model
    const QJsonObject readyRouter = ok(Q("generate.ready"), {{Q("provider"), QStringLiteral("router")}, {Q("model"), QStringLiteral("test/one")}});
    QCOMPARE(readyRouter.value(Q("ok")).toBool(), true);

    const Result rBadId = tryCall(Q("generate.router.model"), {{Q("modelId"), QStringLiteral("noslash")}});
    QVERIFY(!rBadId.ok);
    QCOMPARE(rBadId.error.code, Q("invalid_params"));

    const QJsonObject one = ok(Q("generate.router.model"), {{Q("modelId"), QStringLiteral("test/one")}});
    QCOMPARE(one.value(Q("id")).toString(), QStringLiteral("test/one"));
  }

  // Schema with ETag
  void routerSchemaEtag() {
    qputenv("COMFY_API_KEY", "test-key-g");
    const QJsonObject s1 = ok(Q("generate.router.schema"), {{Q("modelId"), QStringLiteral("test/s1")}});
    QCOMPARE(s1.value(Q("notModified")).toBool(), false);
    QCOMPARE(s1.value(Q("etag")).toString(), QStringLiteral("\"v1\""));
    QVERIFY(s1.value(Q("schema")).toObject().contains(Q("openapi")));

    const QJsonObject s2 = ok(Q("generate.router.schema"), {{Q("modelId"), QStringLiteral("test/s1")},
        {Q("etag"), QStringLiteral("\"v1\"")}});
    QCOMPARE(s2.value(Q("notModified")).toBool(), true);
  }

  // Submit -> poll -> collect -> cancel
  void routerSubmitCollect() {
    qputenv("COMFY_API_KEY", "test-key-g");
    const QJsonObject sub = ok(Q("generate.router.submit"), {{Q("modelId"), QStringLiteral("test/one")},
        {Q("body"), QJsonObject{{Q("mode"), QStringLiteral("1")}}}});
    QCOMPARE(sub.value(Q("requestId")).toString(), QStringLiteral("req-1"));
    QCOMPARE(sub.value(Q("status")).toString(), QStringLiteral("IN_QUEUE"));
    QVERIFY(!sub.value(Q("idempotencyKey")).toString().isEmpty());

    QString st;
    for (int i = 0; i < 5 && st != QLatin1String("COMPLETED"); ++i) {
      const QJsonObject s = ok(Q("generate.router.status"), {{Q("modelId"), QStringLiteral("test/one")},
          {Q("requestId"), QStringLiteral("req-1")}});
      st = s.value(Q("status")).toString();
      if (st != QLatin1String("COMPLETED")) QTest::qWait(10);
    }
    QCOMPARE(st, QStringLiteral("COMPLETED"));

    const QJsonObject col = ok(Q("generate.router.collect"), {{Q("modelId"), QStringLiteral("test/one")},
        {Q("requestId"), QStringLiteral("req-1")}});
    QCOMPARE(col.value(Q("output")).toObject().value(Q("result")).toObject().value(Q("sample")).toString(),
             QStringLiteral("https://example.invalid/sample.png"));

    const QJsonObject can = ok(Q("generate.router.cancel"), {{Q("modelId"), QStringLiteral("test/one")},
        {Q("requestId"), QStringLiteral("req-1")}});
    QCOMPARE(can.value(Q("cancelled")).toBool(), true);
  }

  // Error mapping and binary-output saveAs
  void routerErrorsAndBinary() {
    qputenv("COMFY_API_KEY", "test-key-g");

    Result r = tryCall(Q("generate.router.submit"), {{Q("modelId"), QStringLiteral("test/insufficient")},
        {Q("body"), QJsonObject{{Q("mode"), QStringLiteral("1")}}}});
    QVERIFY(!r.ok);
    QCOMPARE(r.error.code, Q("insufficient_credits"));

    r = tryCall(Q("generate.router.submit"), {{Q("modelId"), QStringLiteral("test/missing")},
        {Q("body"), QJsonObject{{Q("mode"), QStringLiteral("1")}}}});
    QVERIFY(!r.ok);
    QCOMPARE(r.error.code, Q("model_not_found"));

    const QJsonObject stFail = ok(Q("generate.router.status"), {{Q("modelId"), QStringLiteral("test/one")},
        {Q("requestId"), QStringLiteral("req-fail")}});
    QCOMPARE(stFail.value(Q("status")).toString(), QStringLiteral("COMPLETED"));
    QCOMPARE(stFail.value(Q("errorType")).toString(), QStringLiteral("content_policy_violation"));
    const Result rCollect = tryCall(Q("generate.router.collect"), {{Q("modelId"), QStringLiteral("test/one")},
        {Q("requestId"), QStringLiteral("req-fail")}});
    QVERIFY(!rCollect.ok);
    QCOMPARE(rCollect.error.code, Q("router_error"));

    const Result rNoSave = tryCall(Q("generate.router.collect"), {{Q("modelId"), QStringLiteral("test/one")},
        {Q("requestId"), QStringLiteral("req-bin")}});
    QVERIFY(!rNoSave.ok);
    QCOMPARE(rNoSave.error.code, Q("invalid_params"));

    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString binPath = tmp.filePath(QStringLiteral("out.png"));
    const QJsonObject bin = ok(Q("generate.router.collect"), {{Q("modelId"), QStringLiteral("test/one")},
        {Q("requestId"), QStringLiteral("req-bin")}, {Q("saveAs"), binPath}});
    QCOMPARE(bin.value(Q("file")).toString(), binPath);
    QVERIFY(QFile(binPath).size() > 100);
  }

  // Direct RouterClient and provider coverage:
  // success, HTTP auth error (401), HTTP 5xx (500), transport failure, and missing-key path.
  void routerClientCoverage() {
    using namespace sf::comfy;

    // 1. Missing-key path (clear error, no crash)
    {
      qunsetenv("COMFY_API_KEY");
      RouterClient clientNoKey(fake_->url(), QString());
      QCOMPARE(clientNoKey.apiKey(), QString());

      // Check every client method with no key: none must crash, all must report transport failure
      RouterReply r1 = clientNoKey.listModels(10, QString());
      QCOMPARE(r1.status, 0);
      QVERIFY(r1.transportFailed());
      QVERIFY(r1.error.contains(QStringLiteral("no API key")));

      RouterReply r2 = clientNoKey.getModel(QStringLiteral("test/one"));
      QCOMPARE(r2.status, 0);
      QVERIFY(r2.transportFailed());
      QVERIFY(r2.error.contains(QStringLiteral("no API key")));

      RouterReply r3 = clientNoKey.getSchema(QStringLiteral("test/one"), QString());
      QCOMPARE(r3.status, 0);
      QVERIFY(r3.transportFailed());
      QVERIFY(r3.error.contains(QStringLiteral("no API key")));

      RouterReply r4 = clientNoKey.submitRequest(QStringLiteral("test/one"), QJsonObject{});
      QCOMPARE(r4.status, 0);
      QVERIFY(r4.transportFailed());
      QVERIFY(r4.error.contains(QStringLiteral("no API key")));

      RouterReply r5 = clientNoKey.requestStatus(QStringLiteral("test/one"), QStringLiteral("req-1"));
      QCOMPARE(r5.status, 0);
      QVERIFY(r5.transportFailed());
      QVERIFY(r5.error.contains(QStringLiteral("no API key")));

      RouterReply r6 = clientNoKey.collectRequest(QStringLiteral("test/one"), QStringLiteral("req-1"));
      QCOMPARE(r6.status, 0);
      QVERIFY(r6.transportFailed());
      QVERIFY(r6.error.contains(QStringLiteral("no API key")));

      RouterReply r7 = clientNoKey.cancelRequest(QStringLiteral("test/one"), QStringLiteral("req-1"));
      QCOMPARE(r7.status, 0);
      QVERIFY(r7.transportFailed());
      QVERIFY(r7.error.contains(QStringLiteral("no API key")));

      // Engine commands with missing key return not_configured
      Result cmdModelsNoKey = tryCall(Q("generate.router.models"), {});
      QVERIFY(!cmdModelsNoKey.ok);
      QCOMPARE(cmdModelsNoKey.error.code, Q("not_configured"));

      Result cmdReadyNoKey = tryCall(Q("generate.ready"), {{Q("provider"), QStringLiteral("router")}, {Q("model"), QStringLiteral("test/one")}});
      QVERIFY(!cmdReadyNoKey.ok);
      QCOMPARE(cmdReadyNoKey.error.code, Q("not_configured"));
    }

    // 2. Success path for RouterClient
    {
      const QString validKey = QStringLiteral("valid-test-key");
      RouterClient client(fake_->url(), validKey);
      QCOMPARE(client.apiKey(), validKey);
      QCOMPARE(client.base(), fake_->url());

      // listModels
      RouterReply repList = client.listModels(10, QString());
      QCOMPARE(repList.status, 200);
      QVERIFY(repList.ok());
      QVERIFY(!repList.transportFailed());
      QVERIFY(repList.body.contains(QStringLiteral("data")));
      QCOMPARE(fake_->routerApiKey(), validKey);

      // getModel
      RouterReply repModel = client.getModel(QStringLiteral("test/one"));
      QCOMPARE(repModel.status, 200);
      QVERIFY(repModel.ok());
      QCOMPARE(repModel.body.value(QStringLiteral("id")).toString(), QStringLiteral("test/one"));

      // getSchema
      RouterReply repSchema = client.getSchema(QStringLiteral("test/s1"), QString());
      QCOMPARE(repSchema.status, 200);
      QVERIFY(repSchema.ok());
      QCOMPARE(repSchema.etag, QStringLiteral("\"v1\""));

      // submitRequest
      RouterReply repSub = client.submitRequest(QStringLiteral("test/one"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("1")}});
      QCOMPARE(repSub.status, 201);
      QVERIFY(repSub.ok());
      const QString reqId = repSub.body.value(QStringLiteral("request_id")).toString();
      QCOMPARE(reqId, QStringLiteral("req-1"));

      // requestStatus (poll until completed)
      RouterReply repStatus;
      for (int i = 0; i < 5; ++i) {
        repStatus = client.requestStatus(QStringLiteral("test/one"), reqId);
        QCOMPARE(repStatus.status, 200);
        if (repStatus.body.value(QStringLiteral("status")).toString() == QLatin1String("COMPLETED")) break;
        QTest::qWait(10);
      }
      QCOMPARE(repStatus.body.value(QStringLiteral("status")).toString(), QStringLiteral("COMPLETED"));

      // collectRequest
      RouterReply repCollect = client.collectRequest(QStringLiteral("test/one"), reqId);
      QCOMPARE(repCollect.status, 200);
      QVERIFY(repCollect.ok());
      const QString sampleUrlStr = repCollect.body.value(QStringLiteral("result")).toObject().value(QStringLiteral("sample")).toString();
      QVERIFY(!sampleUrlStr.isEmpty());

      // cancelRequest
      RouterReply repCancel = client.cancelRequest(QStringLiteral("test/one"), reqId);
      QCOMPARE(repCancel.status, 202);
      QVERIFY(repCancel.ok());

      // downloadUrl success
      QTemporaryDir tmp;
      QVERIFY(tmp.isValid());
      const QString sampleDlPath = tmp.filePath(QStringLiteral("downloaded_sample.png"));
      QString dlErr;
      const QUrl sampleUrl = QUrl(QStringLiteral("http://127.0.0.1:%1/sample.png").arg(fake_->port()));
      bool dlOk = RouterClient::downloadUrl(sampleUrl, sampleDlPath, &dlErr);
      QVERIFY2(dlOk, qPrintable(dlErr));
      QVERIFY(QFile::exists(sampleDlPath));
      QVERIFY(QFile(sampleDlPath).size() > 50);

      // Provider enum value verification
      QCOMPARE(static_cast<int>(GenProvider::LocalComfy), 0);
      QCOMPARE(static_cast<int>(GenProvider::ComfyRouter), 1);
    }

    // 3. HTTP Auth Error (401)
    {
      // RouterClient with invalid key
      RouterClient clientBadKey(fake_->url(), QStringLiteral("bad-key"));
      RouterReply repAuth = clientBadKey.listModels(10, QString());
      QCOMPARE(repAuth.status, 401);
      QCOMPARE(repAuth.errorType, QStringLiteral("unauthorized"));
      QVERIFY(!repAuth.ok());
      QVERIFY(!repAuth.transportFailed());
      QVERIFY(repAuth.error.isEmpty());

      // fake_->setRouterAuthFail(true)
      fake_->setRouterAuthFail(true);
      RouterClient clientReject(fake_->url(), QStringLiteral("valid-looking-key"));
      RouterReply repReject = clientReject.getModel(QStringLiteral("test/one"));
      QCOMPARE(repReject.status, 401);
      QCOMPARE(repReject.errorType, QStringLiteral("unauthorized"));
      fake_->setRouterAuthFail(false);

      // Engine command layer with bad key
      qputenv("COMFY_API_KEY", "bad-key");
      Result rCmdModels = tryCall(Q("generate.router.models"), {});
      QVERIFY(!rCmdModels.ok);
      QCOMPARE(rCmdModels.error.code, Q("auth_error"));
      QCOMPARE(rCmdModels.error.details.toObject().value(Q("status")).toInt(), 401);

      Result rCmdReady = tryCall(Q("generate.ready"), {{Q("provider"), QStringLiteral("router")}, {Q("model"), QStringLiteral("test/one")}});
      QVERIFY(!rCmdReady.ok);
      QCOMPARE(rCmdReady.error.code, Q("auth_error"));
      QCOMPARE(rCmdReady.error.details.toObject().value(Q("status")).toInt(), 401);
    }

    // 4. HTTP 5xx (500)
    {
      const QString validKey = QStringLiteral("valid-test-key");
      RouterClient client(fake_->url(), validKey);

      // Model test/500 returns HTTP 500
      RouterReply rep500 = client.getModel(QStringLiteral("test/500"));
      QCOMPARE(rep500.status, 500);
      QCOMPARE(rep500.errorType, QStringLiteral("internal_server_error"));
      QVERIFY(!rep500.ok());
      QVERIFY(!rep500.transportFailed());

      // fake_->setRouter5xx(true)
      fake_->setRouter5xx(true);
      RouterReply repForce500 = client.listModels(10, QString());
      QCOMPARE(repForce500.status, 500);
      QCOMPARE(repForce500.errorType, QStringLiteral("internal_server_error"));
      fake_->setRouter5xx(false);

      // Engine command layer with 500
      qputenv("COMFY_API_KEY", validKey.toUtf8());
      Result rCmd500 = tryCall(Q("generate.router.model"), {{Q("modelId"), QStringLiteral("test/500")}});
      QVERIFY(!rCmd500.ok);
      QCOMPARE(rCmd500.error.code, Q("router_error"));
      QCOMPARE(rCmd500.error.details.toObject().value(Q("status")).toInt(), 500);
      QCOMPARE(rCmd500.error.details.toObject().value(Q("errorType")).toString(), QStringLiteral("internal_server_error"));
    }

    // 5. Transport failure (connection refused / unreachable)
    {
      const QUrl unreachUrl(QStringLiteral("http://127.0.0.1:1"));
      RouterClient clientUnreach(unreachUrl, QStringLiteral("any-key"));

      RouterReply repUnreach = clientUnreach.listModels(10, QString(), 1000);
      QCOMPARE(repUnreach.status, 0);
      QVERIFY(repUnreach.transportFailed());
      QVERIFY(!repUnreach.error.isEmpty());
      QVERIFY(!repUnreach.ok());

      // downloadUrl transport failure
      QTemporaryDir tmp;
      QVERIFY(tmp.isValid());
      QString dlErr;
      bool dlOk = RouterClient::downloadUrl(QUrl(QStringLiteral("http://127.0.0.1:1/nonexistent.png")),
                                           tmp.filePath(QStringLiteral("bad.png")), &dlErr, 1000);
      QVERIFY(!dlOk);
      QVERIFY(!dlErr.isEmpty());

      // Engine command layer with unreachable base URL
      qputenv("COMFY_ROUTER_BASE_URL", "http://127.0.0.1:1");
      qputenv("COMFY_API_KEY", "any-key");
      Result rCmdUnreach = tryCall(Q("generate.router.models"), {});
      QVERIFY(!rCmdUnreach.ok);
      QCOMPARE(rCmdUnreach.error.code, Q("router_unreachable"));

      Result rCmdReadyUnreach = tryCall(Q("generate.ready"), {{Q("provider"), QStringLiteral("router")}, {Q("model"), QStringLiteral("test/one")}});
      QVERIFY(!rCmdReadyUnreach.ok);
      QCOMPARE(rCmdReadyUnreach.error.code, Q("router_unreachable"));

      // Restore base URL to fake server
      qputenv("COMFY_ROUTER_BASE_URL", fake_->url().toString().toUtf8());
    }

    // 6. Security rule: API key is NEVER leaked in logs, traces, or error strings
    {
      const QString secretKey = QStringLiteral("secret-token-do-not-leak-998877");
      qputenv("COMFY_API_KEY", secretKey.toUtf8());

      Result r = tryCall(Q("generate.router.model"), {{Q("modelId"), QStringLiteral("test/500")}});
      QVERIFY(!r.ok);
      QVERIFY(!r.error.message.contains(secretKey));
      QVERIFY(!QJsonDocument(r.error.details.toObject()).toJson().contains(secretKey.toUtf8()));

      const QJsonObject info = ok(Q("comfy.router.info"));
      QCOMPARE(info.value(Q("keyConfigured")).toBool(), true);
      QVERIFY(!QJsonDocument(info).toJson().contains(secretKey.toUtf8()));

      qunsetenv("COMFY_API_KEY");
    }
  }
};

#include "tst_generate.moc"

QTEST_MAIN(TstGenerate)
