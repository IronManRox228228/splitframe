// ComfyUI unit tests: schemas, graph editing, validation, client, and invariants.

#include "comfy/comfy_client.h"
#include "comfy/comfy_graph.h"
#include "comfy/comfy_schema.h"
#include "comfy/comfy_validate.h"
#include "comfy_fake_server.h"

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>

using namespace sf::comfy;

class TstComfy : public QObject {
  Q_OBJECT

  SchemaSet schemas_;
  Graph txt2imgGraph_;
  QJsonObject txt2imgJson_;

private slots:
  void initTestCase() {
    const QString fixturesDir = QStringLiteral(SF_COMFY_FIXTURES);
    QFile fObj(fixturesDir + QStringLiteral("/object_info_min.json"));
    QVERIFY2(fObj.open(QIODevice::ReadOnly), qPrintable(fObj.errorString()));
    QJsonDocument docObj = QJsonDocument::fromJson(fObj.readAll());
    QVERIFY(docObj.isObject());
    QStringList warnings;
    schemas_ = SchemaSet::parse(docObj.object(), &warnings);
    QVERIFY(warnings.isEmpty());

    QFile fGraph(fixturesDir + QStringLiteral("/txt2img_api.json"));
    QVERIFY2(fGraph.open(QIODevice::ReadOnly), qPrintable(fGraph.errorString()));
    QJsonDocument docGraph = QJsonDocument::fromJson(fGraph.readAll());
    QVERIFY(docGraph.isObject());
    txt2imgJson_ = docGraph.object();

    QString err;
    auto g = Graph::fromApiJson(txt2imgJson_, &err);
    QVERIFY2(g.has_value(), qPrintable(err));
    txt2imgGraph_ = std::move(*g);
  }

  // 1. parsesSchema
  void parsesSchema() {
    QCOMPARE(schemas_.size(), 7);

    const NodeSchema* ksampler = schemas_.find(QStringLiteral("KSampler"));
    QVERIFY(ksampler != nullptr);
    const QStringList expectedOrder = {
      QStringLiteral("model"), QStringLiteral("seed"), QStringLiteral("steps"),
      QStringLiteral("cfg"), QStringLiteral("sampler_name"), QStringLiteral("scheduler"),
      QStringLiteral("positive"), QStringLiteral("negative"), QStringLiteral("latent_image"),
      QStringLiteral("denoise")
    };
    QCOMPARE(ksampler->inputs.size(), static_cast<size_t>(expectedOrder.size()));
    for (size_t i = 0; i < ksampler->inputs.size(); ++i) {
      QCOMPARE(ksampler->inputs[i].name, expectedOrder[static_cast<int>(i)]);
    }

    const InputSpec* cfg = ksampler->input(QStringLiteral("cfg"));
    QVERIFY(cfg != nullptr);
    QCOMPARE(cfg->kind, InputKind::Float);
    QVERIFY(cfg->min.has_value());
    QCOMPARE(*cfg->min, 0.0);
    QVERIFY(cfg->max.has_value());
    QCOMPARE(*cfg->max, 100.0);
    QVERIFY(cfg->step.has_value());
    QCOMPARE(*cfg->step, 0.1);

    const InputSpec* samplerName = ksampler->input(QStringLiteral("sampler_name"));
    QVERIFY(samplerName != nullptr);
    QCOMPARE(samplerName->kind, InputKind::Combo);
    QCOMPARE(samplerName->options.size(), 3);

    const NodeSchema* previewAny = schemas_.find(QStringLiteral("PreviewAny"));
    QVERIFY(previewAny != nullptr);
    const InputSpec* mode = previewAny->input(QStringLiteral("mode"));
    QVERIFY(mode != nullptr);
    QCOMPARE(mode->kind, InputKind::Combo);
    const QStringList expectedModeOpts = {QStringLiteral("a"), QStringLiteral("b")};
    QCOMPARE(mode->options, expectedModeOpts);
    QCOMPARE(mode->defaultValue.toString(), QStringLiteral("a"));

    const NodeSchema* saveImage = schemas_.find(QStringLiteral("SaveImage"));
    QVERIFY(saveImage != nullptr);
    QVERIFY(saveImage->outputNode);

    const InputSpec* source = previewAny->input(QStringLiteral("source"));
    QVERIFY(source != nullptr);
    const QStringList expectedWildcard = {QStringLiteral("*")};
    QCOMPARE(source->linkTypes, expectedWildcard);
  }

  // 2. toolSchemaShape
  void toolSchemaShape() {
    const NodeSchema* ksampler = schemas_.find(QStringLiteral("KSampler"));
    QVERIFY(ksampler != nullptr);
    const QJsonObject ts = toolSchema(*ksampler);
    QCOMPARE(ts.value(QStringLiteral("type")).toString(), QStringLiteral("object"));

    const QJsonObject props = ts.value(QStringLiteral("properties")).toObject();
    QVERIFY(props.contains(QStringLiteral("seed")));
    QVERIFY(props.contains(QStringLiteral("steps")));
    QVERIFY(props.contains(QStringLiteral("cfg")));
    QVERIFY(props.contains(QStringLiteral("sampler_name")));
    QVERIFY(props.value(QStringLiteral("sampler_name")).toObject().contains(QStringLiteral("enum")));
    QVERIFY(props.contains(QStringLiteral("scheduler")));
    QVERIFY(props.contains(QStringLiteral("denoise")));

    QVERIFY(!props.contains(QStringLiteral("model")));
    QVERIFY(!props.contains(QStringLiteral("positive")));

    const QJsonArray reqArr = ts.value(QStringLiteral("required")).toArray();
    QStringList reqList;
    for (const auto& r : reqArr) reqList.append(r.toString());
    const QStringList expectedReq = {QStringLiteral("sampler_name"), QStringLiteral("scheduler")};
    QCOMPARE(reqList, expectedReq);
  }

  // 3. roundTripApiJson
  void roundTripApiJson() {
    const QJsonObject outJson = txt2imgGraph_.toApiJson();
    QCOMPARE(outJson, txt2imgJson_);
  }

  // 4. validGraphHasNoIssues
  void validGraphHasNoIssues() {
    const auto issues = validate(txt2imgGraph_, schemas_);
    QVERIFY2(issues.empty(), qPrintable(QString::number(issues.size()) + QStringLiteral(" issues found")));
  }

  // 5. opsAndAtomicity
  void opsAndAtomicity() {
    Graph g;
    const QJsonArray batch1 = {
      QJsonObject{{QStringLiteral("op"), QStringLiteral("add")}, {QStringLiteral("classType"), QStringLiteral("CheckpointLoaderSimple")}},
      QJsonObject{{QStringLiteral("op"), QStringLiteral("add")}, {QStringLiteral("classType"), QStringLiteral("SaveImage")}},
      QJsonObject{{QStringLiteral("op"), QStringLiteral("set")}, {QStringLiteral("id"), QStringLiteral("$0")}, {QStringLiteral("input"), QStringLiteral("ckpt_name")}, {QStringLiteral("value"), QStringLiteral("sd15.safetensors")}},
      QJsonObject{{QStringLiteral("op"), QStringLiteral("connect")}, {QStringLiteral("from"), QStringLiteral("$0")}, {QStringLiteral("output"), 0}, {QStringLiteral("to"), QStringLiteral("$1")}, {QStringLiteral("input"), QStringLiteral("images")}}
    };

    OpResult r1 = applyOps(g, batch1);
    QVERIFY2(r1.ok, qPrintable(r1.error));
    g = std::move(r1.graph);

    const auto issues = validate(g, schemas_);
    QCOMPARE(issues.size(), static_cast<size_t>(1));
    QCOMPARE(issues[0].code, QStringLiteral("type_mismatch"));

    const QJsonArray batch2 = {
      QJsonObject{{QStringLiteral("op"), QStringLiteral("set")}, {QStringLiteral("id"), QStringLiteral("1")}, {QStringLiteral("input"), QStringLiteral("ckpt_name")}, {QStringLiteral("value"), QStringLiteral("sd15.safetensors")}},
      QJsonObject{{QStringLiteral("op"), QStringLiteral("connect")}, {QStringLiteral("from"), QStringLiteral("999")}, {QStringLiteral("output"), 0}, {QStringLiteral("to"), QStringLiteral("2")}, {QStringLiteral("input"), QStringLiteral("images")}}
    };

    OpResult r2 = applyOps(g, batch2);
    QVERIFY(!r2.ok);
    QVERIFY2(r2.error.contains(QStringLiteral("op 1")), qPrintable(r2.error));
    QCOMPARE(r2.graph.nodes().size(), g.nodes().size());
  }

  // 6. validator cases
  void validatorCases() {
    // sampler_name="eulr" -> bad_enum
    {
      OpResult r = applyOps(txt2imgGraph_, {
        QJsonObject{{QStringLiteral("op"), QStringLiteral("set")}, {QStringLiteral("id"), QStringLiteral("3")}, {QStringLiteral("input"), QStringLiteral("sampler_name")}, {QStringLiteral("value"), QStringLiteral("eulr")}}
      });
      QVERIFY(r.ok);
      const auto issues = validate(r.graph, schemas_);
      bool found = false;
      for (const auto& is : issues) if (is.code == QStringLiteral("bad_enum")) found = true;
      QVERIFY(found);
    }

    // steps=0 -> out_of_range
    {
      OpResult r = applyOps(txt2imgGraph_, {
        QJsonObject{{QStringLiteral("op"), QStringLiteral("set")}, {QStringLiteral("id"), QStringLiteral("3")}, {QStringLiteral("input"), QStringLiteral("steps")}, {QStringLiteral("value"), 0}}
      });
      QVERIFY(r.ok);
      const auto issues = validate(r.graph, schemas_);
      bool found = false;
      for (const auto& is : issues) if (is.code == QStringLiteral("out_of_range")) found = true;
      QVERIFY(found);
    }

    // steps="ten" -> bad_literal_type
    {
      OpResult r = applyOps(txt2imgGraph_, {
        QJsonObject{{QStringLiteral("op"), QStringLiteral("set")}, {QStringLiteral("id"), QStringLiteral("3")}, {QStringLiteral("input"), QStringLiteral("steps")}, {QStringLiteral("value"), QStringLiteral("ten")}}
      });
      QVERIFY(r.ok);
      const auto issues = validate(r.graph, schemas_);
      bool found = false;
      for (const auto& is : issues) if (is.code == QStringLiteral("bad_literal_type")) found = true;
      QVERIFY(found);
    }

    // cfg=7 (integral) is OK for Float -> no issue
    {
      OpResult r = applyOps(txt2imgGraph_, {
        QJsonObject{{QStringLiteral("op"), QStringLiteral("set")}, {QStringLiteral("id"), QStringLiteral("3")}, {QStringLiteral("input"), QStringLiteral("cfg")}, {QStringLiteral("value"), 7}}
      });
      QVERIFY(r.ok);
      const auto issues = validate(r.graph, schemas_);
      QVERIFY(issues.empty());
    }

    // removing node 4 erases its links in KSampler.model, both CLIPTextEncode.clip and VAEDecode.vae -> 4 missing_required
    {
      OpResult r = applyOps(txt2imgGraph_, {
        QJsonObject{{QStringLiteral("op"), QStringLiteral("remove")}, {QStringLiteral("id"), QStringLiteral("4")}}
      });
      QVERIFY(r.ok);
      const auto issues = validate(r.graph, schemas_);
      int missingCount = 0;
      for (const auto& is : issues) {
        if (is.code == QStringLiteral("missing_required")) ++missingCount;
      }
      QCOMPARE(missingCount, 4);
    }

    // connecting with outputIndex 5 from node 4 -> bad_link
    {
      OpResult r = applyOps(txt2imgGraph_, {
        QJsonObject{{QStringLiteral("op"), QStringLiteral("connect")}, {QStringLiteral("from"), QStringLiteral("4")}, {QStringLiteral("output"), 5}, {QStringLiteral("to"), QStringLiteral("3")}, {QStringLiteral("input"), QStringLiteral("model")}}
      });
      QVERIFY(r.ok);
      const auto issues = validate(r.graph, schemas_);
      bool found = false;
      for (const auto& is : issues) if (is.code == QStringLiteral("bad_link")) found = true;
      QVERIFY(found);
    }

    // adding a node of class Nope -> unknown_class
    {
      OpResult r = applyOps(txt2imgGraph_, {
        QJsonObject{{QStringLiteral("op"), QStringLiteral("add")}, {QStringLiteral("classType"), QStringLiteral("Nope")}}
      });
      QVERIFY(r.ok);
      const auto issues = validate(r.graph, schemas_);
      bool found = false;
      for (const auto& is : issues) if (is.code == QStringLiteral("unknown_class")) found = true;
      QVERIFY(found);
    }

    // removing node 9 -> no_output_node
    {
      OpResult r = applyOps(txt2imgGraph_, {
        QJsonObject{{QStringLiteral("op"), QStringLiteral("remove")}, {QStringLiteral("id"), QStringLiteral("9")}}
      });
      QVERIFY(r.ok);
      const auto issues = validate(r.graph, schemas_);
      bool found = false;
      for (const auto& is : issues) if (is.code == QStringLiteral("no_output_node")) found = true;
      QVERIFY(found);
    }

    // cycle
    {
      const QJsonObject cycleJson = QJsonDocument::fromJson(
        "{\"1\":{\"class_type\":\"VAEDecode\",\"inputs\":{\"samples\":[\"2\",0],\"vae\":[\"2\",0]}},"
        "\"2\":{\"class_type\":\"VAEDecode\",\"inputs\":{\"samples\":[\"1\",0],\"vae\":[\"1\",0]}}}"
      ).object();
      auto cg = Graph::fromApiJson(cycleJson);
      QVERIFY(cg.has_value());
      const auto issues = validate(*cg, schemas_);
      bool found = false;
      for (const auto& is : issues) if (is.code == QStringLiteral("cycle")) found = true;
      QVERIFY(found);
    }

    // wildcard: PreviewAny.source connected from VAEDecode output 0 has no type issue
    {
      OpResult r = applyOps(txt2imgGraph_, {
        QJsonObject{{QStringLiteral("op"), QStringLiteral("add")}, {QStringLiteral("classType"), QStringLiteral("PreviewAny")}, {QStringLiteral("id"), QStringLiteral("10")}},
        QJsonObject{{QStringLiteral("op"), QStringLiteral("connect")}, {QStringLiteral("from"), QStringLiteral("8")}, {QStringLiteral("output"), 0}, {QStringLiteral("to"), QStringLiteral("10")}, {QStringLiteral("input"), QStringLiteral("source")}}
      });
      QVERIFY(r.ok);
      const auto issues = validate(r.graph, schemas_);
      for (const auto& is : issues) {
        QVERIFY(is.code != QStringLiteral("type_mismatch"));
      }
    }
  }

  // 7. removeNodeCleansLinks
  void removeNodeCleansLinks() {
    Graph g = txt2imgGraph_;
    QCOMPARE(g.removeNode(QStringLiteral("4")), QString());
    for (const auto& [_, node] : g.nodes()) {
      for (const auto& [inName, inVal] : node.inputs) {
        if (std::holds_alternative<Link>(inVal)) {
          QVERIFY(std::get<Link>(inVal).fromNode != QStringLiteral("4"));
        }
      }
    }
  }

  // 8. nextIdIsNumericMax
  void nextIdIsNumericMax() {
    QCOMPARE(txt2imgGraph_.nextId(), QStringLiteral("10"));
    Graph empty;
    QCOMPARE(empty.nextId(), QStringLiteral("1"));
  }

  // 9. clientUnreachable
  void clientUnreachable() {
    Client c(QUrl(QStringLiteral("http://127.0.0.1:1")));
    QString err;
    auto stats = c.systemStats(&err, 500);
    QVERIFY(!stats.has_value());
    QVERIFY2(err.contains(QStringLiteral("not reachable")), qPrintable(err));
  }

  // 10. liveServer
  void liveServer() {
    const QString envUrl = qEnvironmentVariable("SF_COMFY_URL");
    if (envUrl.isEmpty()) QSKIP("no ComfyUI");
    Client c{QUrl(envUrl)};
    QString err;
    auto stats = c.systemStats(&err, 2000);
    if (!stats) QSKIP("no ComfyUI");
    auto info = c.objectInfo(&err);
    QVERIFY(info.has_value());
    QStringList warnings;
    SchemaSet set = SchemaSet::parse(*info, &warnings);
    QVERIFY(set.size() > 20);
    QVERIFY(set.find(QStringLiteral("KSampler")) != nullptr);
  }

  // 11. clientQueueHistoryDownload
  void clientQueueHistoryDownload() {
    FakeComfyServer fake;
    Client client(fake.url());
    QueueResult qres = client.queuePrompt(txt2imgJson_);
    QVERIFY2(qres.error.isEmpty(), qPrintable(qres.error));
    QCOMPARE(qres.promptId, QStringLiteral("p1"));
    QCOMPARE(static_cast<int>(fake.queued.size()), 1);

    QString netErr;
    HistoryResult h1 = client.history(qres.promptId, &netErr, 5000);
    QVERIFY(!h1.finished);

    HistoryResult h2 = client.history(qres.promptId, &netErr, 5000);
    QVERIFY(!h2.finished);

    HistoryResult h3 = client.history(qres.promptId, &netErr, 5000);
    QVERIFY(h3.finished);
    QVERIFY(h3.success);
    QCOMPARE(static_cast<int>(h3.outputs.size()), 1);
    QCOMPARE(h3.outputs[0].filename, QStringLiteral("out_1.png"));

    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());
    const QString dest = tmpDir.filePath(QStringLiteral("test_dl.png"));
    QString dlErr;
    QVERIFY2(client.download(h3.outputs[0], dest, &dlErr), qPrintable(dlErr));
    QVERIFY(QFile::exists(dest));
    QImage img(dest);
    QCOMPARE(img.size(), QSize(64, 64));
  }

  // 12. clientError400ContainsNodeId
  void clientError400ContainsNodeId() {
    FakeComfyServer fake;
    Client client(fake.url());
    QJsonObject failGraph = txt2imgJson_;
    failGraph.insert(QStringLiteral("42"), QJsonObject{
      {QStringLiteral("class_type"), QStringLiteral("FailMe")},
      {QStringLiteral("inputs"), QJsonObject{}}
    });
    QueueResult qres = client.queuePrompt(failGraph);
    QVERIFY(!qres.error.isEmpty());
    QVERIFY2(qres.error.contains(QStringLiteral("42")), qPrintable(qres.error));
    QVERIFY(qres.nodeErrors.contains(QStringLiteral("42")));
  }

  // 13. clientUploadImage
  void clientUploadImage() {
    FakeComfyServer fake;
    Client client(fake.url());
    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());
    const QString imgPath = tmpDir.filePath(QStringLiteral("upload.png"));
    QImage img(16, 16, QImage::Format_RGB32);
    img.fill(Qt::blue);
    QVERIFY(img.save(imgPath, "PNG"));

    QString upErr;
    auto upVal = client.uploadImage(imgPath, &upErr);
    QVERIFY2(upVal.has_value(), qPrintable(upErr));
    QCOMPARE(*upVal, QStringLiteral("up.png"));
    QCOMPARE(static_cast<int>(fake.uploads), 1);
  }
};

QTEST_MAIN(TstComfy)
#include "tst_comfy.moc"
