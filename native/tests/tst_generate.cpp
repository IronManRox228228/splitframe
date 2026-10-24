// 15-GEN Part F: comfy.graph.saveAsTemplate, generate.fromGraph, readiness gating,
// pending/collect flow, variants undo grouping, audio/video wrapper paths. Runs against the
// fake ComfyUI server.

#include "comfy_fake_server.h"
#include "engine/engine.h"

#include <QFile>
#include <QFileInfo>
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
  // Uses QTest::qFail, not QVERIFY2: the assert macros expand to a void `return`,
  // which is illegal inside this non-void helper (C2561).
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
    }
    qunsetenv("COMFY_API_KEY");
    engine_.reset();
  }

  // 1. generate.fromGraph: a loaded graph runs to completion, lands exactly one item under one
  //    undo step, and stamps a provenance record with the document name as templateId.
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
    QCOMPARE(rec.value(Q("templateId")).toString(), QStringLiteral("GusGraph"));
    QCOMPARE(rec.value(Q("outputKind")).toString(), QStringLiteral("image"));
    QVERIFY(rec.contains(Q("seed")));
    QVERIFY(!rec.value(Q("workflowHash")).toString().isEmpty());

    // one undo group removes the landed item
    QCOMPARE(timelineItemCount(), 1);
    QCOMPARE(ok(Q("history.undo")).value(Q("steps")).toInt(), 1);
    QCOMPARE(timelineItemCount(), 0);
  }

  // 2. comfy.graph.saveAsTemplate round-trip: the saved user template shows up in template.list
  //    with v2 keys and comfy.graph.fromTemplate binds through it.
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

  // 3. saveAsTemplate validation: id regex, kind, outputKind, bindings, outputNode, requires.
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

    // unknown graph doc is not_found, not invalid
    const Result r404 = tryCall(Q("comfy.graph.saveAsTemplate"), {{
        Q("graphId"), QStringLiteral("g99999")}, {Q("id"), QStringLiteral("z11")},
        {Q("name"), Q("N")}, {Q("kind"), Q("image")}});
    QVERIFY(!r404.ok);
    QCOMPARE(r404.error.code, Q("not_found"));

    // an explicit valid outputNode is accepted
    ok(Q("comfy.graph.saveAsTemplate"), {{
        Q("graphId"), gid}, {Q("id"), QStringLiteral("zvalid")}, {Q("name"), QStringLiteral("Z")},
        {Q("kind"), Q("image")}, {Q("outputNode"), Q("8")}});
  }

  // 4. requires/needsUserModel round trip through saveAsTemplate and comfy.ready: an installed
  //    placeholder model is ready, an unmet one is reported per template.
  void saveAsTemplateRequires() {
    const QString gid = loadGraph(QStringLiteral("SrcDocReq"));
    ok(Q("comfy.graph.saveAsTemplate"), {{
        Q("graphId"), gid}, {Q("id"), QStringLiteral("greq")}, {Q("name"), QStringLiteral("GREQ")},
        {Q("kind"), Q("image")},
        {Q("requires"), QJsonObject{
            {{Q("nodes"), QJsonArray{Q("KSampler")}}, {Q("models"), QJsonArray{QJsonObject{{Q("folder"), Q("checkpoints")}, {Q("file"), QStringLiteral("sd15.safetensors")}}}}}}},
        {Q("needsUserModel"), true}});

    // equipped install: sd15.safetensors is offered by the fixture combo options
    const QJsonObject row = ok(Q("comfy.ready"), {{Q("templateId"), QStringLiteral("greq")}});
    QCOMPARE(row.value(Q("ok")).toBool(), true);
    QVERIFY(row.value(Q("missingNodes")).toArray().isEmpty());
    QVERIFY(row.value(Q("missingModels")).toArray().isEmpty());
    QCOMPARE(row.value(Q("kind")).toString(), QStringLiteral("image"));

    // an unmet model file is reported in missingModels with reason
    const QString gid2 = loadGraph(QStringLiteral("SrcDocReq2"));
    ok(Q("comfy.graph.saveAsTemplate"), {{
        Q("graphId"), gid2}, {Q("id"), QStringLiteral("greq2")}, {Q("name"), QStringLiteral("GREQ2")},
        {Q("kind"), Q("image")},
        {Q("requires"), QJsonObject{{Q("models"), QJsonArray{QJsonObject{{Q("folder"), Q("checkpoints")}, {Q("file"), QStringLiteral("unmet.safetensors")}}}}}}});
    const QJsonObject row2 = ok(Q("comfy.ready"), {{Q("templateId"), QStringLiteral("greq2")}});
    QCOMPARE(row2.value(Q("ok")).toBool(), false);
    QCOMPARE(row2.value(Q("missingModels")).toArray().size(), 1);
    QCOMPARE(row2.value(Q("missingModels")).toArray().at(0).toObject().value(Q("file")).toString(),
             QStringLiteral("unmet.safetensors"));
    QVERIFY(row2.value(Q("reason")).toString().contains(QStringLiteral("unmet.safetensors")));
  }

  // 5. generate.fromGraph readiness gate: an unmet requires.model list fails not_ready before
  //    anything is queued.
  void fromGraphReadyGate() {
    const QString gid = loadGraph(QStringLiteral("GusGate"));
    const size_t queuedBefore = fake_->queued.size();

    const Result r = tryCall(Q("generate.fromGraph"), {{
        Q("graphId"), gid}, {Q("kind"), Q("image")}, {Q("requires"), modelReq(QStringLiteral("not_installed.safetensors"))}});
    QVERIFY(!r.ok);
    QCOMPARE(r.error.code, Q("not_ready"));
    QVERIFY(r.error.message.contains(QStringLiteral("not_installed.safetensors")));
    QCOMPARE(fake_->queued.size(), queuedBefore); // nothing was queued
  }

  // 6. generate.fromGraph argument errors and unknown graph.
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

    // template path stays intact: unknown template id is not_found
    const Result rT = tryCall(Q("generate.image"),
                              {{Q("templateId"), QStringLiteral("nope_tmpl")}, {Q("prompt"), Q("x")}});
    QVERIFY(!rT.ok);
    QCOMPARE(rT.error.code, Q("not_found"));
  }

  // 7. wait=false returns pending, and generate.collect lands the files once the job is done.
  void fromGraphPendingCollect() {
    const QString gid = loadGraph(QStringLiteral("GusPending"));
    const QJsonObject r = ok(Q("generate.fromGraph"), {{
        Q("graphId"), gid}, {Q("kind"), Q("image")}, {Q("prompt"), Q("v")}, {Q("wait"), false}});
    QCOMPARE(r.value(Q("pending")).toBool(), true);
    QCOMPARE(r.value(Q("files")).toArray().size(), 0);
    const QString jobId = r.value(Q("jobId")).toString();
    QVERIFY(!jobId.isEmpty());

    // poll generate.collect until the job state is Done
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

  // 8. variants=2 lands two items in ONE undo step.
  void variantsOneUndoGroup() {
    const QString gid = loadGraph(QStringLiteral("GusVar"));
    const QJsonObject r = ok(Q("generate.fromGraph"), {{
        Q("graphId"), gid}, {Q("kind"), Q("image")}, {Q("prompt"), Q("mountain")},
        {Q("variants"), 2}, {Q("wait"), true}});
    QCOMPARE(r.value(Q("files")).toArray().size(), 2);
    QCOMPARE(r.value(Q("created")).toObject().value(Q("items")).toArray().size(), 2);
    QCOMPARE(timelineItemCount(), 2);

    // both items came from one undo step
    QCOMPARE(ok(Q("history.undo")).value(Q("steps")).toInt(), 1);
    QCOMPARE(timelineItemCount(), 0);
  }

  // 9. speech kind: audio landing through a user template with real WAV output.
  void speechAudioPath() {
    const QString gid = loadGraph(QStringLiteral("SprGraph"));
    ok(Q("comfy.graph.saveAsTemplate"), {{
        Q("graphId"), gid}, {Q("id"), QStringLiteral("spr")}, {Q("name"), QStringLiteral("SprT")},
        {Q("kind"), QStringLiteral("speech")},
        {Q("bindings"), QJsonObject{
            {{Q("text"), QJsonObject{{Q("node"), Q("6")}, {Q("input"), Q("text")}}},
             {Q("seed"), QJsonObject{{Q("node"), Q("3")}, {Q("input"), Q("seed")}}}}}}});

    fake_->setOutputKind(Q("audio"));
    const QJsonObject r = ok(Q("generate.speech"), {{
        Q("templateId"), QStringLiteral("spr")}, {Q("text"), QStringLiteral("hello world")},
        {Q("wait"), true}});
    const QJsonArray files = r.value(Q("files")).toArray();
    QCOMPARE(files.size(), 1);
    const QString wav = files.at(0).toString();
    QVERIFY(wav.endsWith(QStringLiteral(".wav"), Qt::CaseInsensitive));
    QVERIFY(QFileInfo::exists(wav));
    QVERIFY(QFile(wav).size() > 100); // real RIFF payload, not a stub
    QCOMPARE(r.value(Q("created")).toObject().value(Q("items")).toArray().size(), 1);

    const QJsonObject rec = lastHistoryRecord();
    QCOMPARE(rec.value(Q("templateId")).toString(), QStringLiteral("spr"));
    QCOMPARE(rec.value(Q("outputKind")).toString(), QStringLiteral("audio"));
  }

  // 10. video wrapper guards: kind mismatch and unexpected output extension never land.
  void videoWrapperGuards() {
    // sd15_txt2img produces images: generate.video refuses it
    const Result rKind = tryCall(Q("generate.video"),
                                 {{Q("templateId"), QStringLiteral("sd15_txt2img")}, {Q("prompt"), Q("x")}, {Q("wait"), true}});
    QVERIFY(!rKind.ok);
    QCOMPARE(rKind.error.code, Q("invalid_params"));
    QVERIFY(rKind.error.message.contains(QStringLiteral("produces image")));

    // a video-kind user template with a bogus output extension fails the extension guard
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
    QCOMPARE(fake_->queued.size(), queuedBefore + 1); // the job ran, landing refused
  }

  // 11. builtin image_txt2img is listed with v2 fields and reports ready on the fixture install.
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

    // the fixture install offers sd15.safetensors -> ready
    const QJsonObject row = ok(Q("comfy.ready"), {{Q("templateId"), QStringLiteral("image_txt2img")}});
    QCOMPARE(row.value(Q("ok")).toBool(), true);
    QVERIFY(row.value(Q("missingNodes")).toArray().isEmpty());
    QVERIFY(row.value(Q("missingModels")).toArray().isEmpty());

    // unknown template id is not_found
    const Result r = tryCall(Q("comfy.ready"), {{Q("templateId"), QStringLiteral("nope_x")}});
    QVERIFY(!r.ok);
    QCOMPARE(r.error.code, Q("not_found"));
  }

  // ---- 15-GEN Part G: Comfy Router provider (against the local fake Router API) ----

  // 12. Key handling: no key -> not_configured (before any HTTP), key reaches the wire, invalid
  //     modelId shapes are rejected locally.
  void routerKeyConfig() {
    qunsetenv("COMFY_API_KEY");
    const QJsonObject info0 = ok(Q("comfy.router.info"));
    QCOMPARE(info0.value(Q("keyConfigured")).toBool(), false);
    const Result rNoKey = tryCall(Q("generate.router.models"), {});
    QVERIFY(!rNoKey.ok);
    QCOMPARE(rNoKey.error.code, Q("not_configured"));

    qputenv("COMFY_API_KEY", "test-key-g");
    const QJsonObject info = ok(Q("comfy.router.info"));
    QCOMPARE(info.value(Q("keyConfigured")).toBool(), true);
    const QJsonObject cat = ok(Q("generate.router.models"), {{Q("limit"), 20}});
    QCOMPARE(cat.value(Q("models")).toArray().size(), 2);
    QCOMPARE(cat.value(Q("hasMore")).toBool(), false);
    QCOMPARE(fake_->routerApiKey(), QStringLiteral("test-key-g"));

    const Result rBadId = tryCall(Q("generate.router.model"), {{Q("modelId"), QStringLiteral("noslash")}});
    QVERIFY(!rBadId.ok);
    QCOMPARE(rBadId.error.code, Q("invalid_params"));

    const QJsonObject one = ok(Q("generate.router.model"), {{Q("modelId"), QStringLiteral("test/one")}});
    QCOMPARE(one.value(Q("id")).toString(), QStringLiteral("test/one"));
  }

  // 13. Schema with ETag: first call carries the doc + etag, the second (If-None-Match) says notModified.
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

  // 14. Submit -> poll (IN_PROGRESS then COMPLETED) -> collect the native output -> cancel.
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

  // 15. Error mapping (insufficient_credits / model_not_found / failed-run collect) and the
  //     binary-output saveAs path.
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

    // a failed run: status COMPLETED carries errorType; the result read answers the error response
    const QJsonObject stFail = ok(Q("generate.router.status"), {{Q("modelId"), QStringLiteral("test/one")},
        {Q("requestId"), QStringLiteral("req-fail")}});
    QCOMPARE(stFail.value(Q("status")).toString(), QStringLiteral("COMPLETED"));
    QCOMPARE(stFail.value(Q("errorType")).toString(), QStringLiteral("content_policy_violation"));
    const Result rCollect = tryCall(Q("generate.router.collect"), {{Q("modelId"), QStringLiteral("test/one")},
        {Q("requestId"), QStringLiteral("req-fail")}});
    QVERIFY(!rCollect.ok);
    QCOMPARE(rCollect.error.code, Q("router_error"));

    // binary output requires saveAs
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
};

#include "tst_generate.moc"

QTEST_MAIN(TstGenerate)
