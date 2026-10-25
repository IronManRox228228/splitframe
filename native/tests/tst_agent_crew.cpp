#include "agent/agent_harness.h"
#include "agent/llm_client.h"
#include "agent/llm_config.h"
#include "agent/peer_bus.h"
#include "agent/specialist.h"
#include "agent/trace.h"
#include "core/schema.h"
#include "core/timeline_doc.h"
#include "engine/engine.h"

#include "fake_llm_server.h"
#include "comfy_fake_server.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace sf;
using namespace sf::agent;
using namespace sf::engine;

class ScopedEnv {
public:
  void set(const char* key, const QString& val) {
    if (!saved_.contains(key)) {
      saved_[key] = qEnvironmentVariable(key);
    }
    qputenv(key, val.toUtf8());
  }
  void unset(const char* key) {
    if (!saved_.contains(key)) {
      saved_[key] = qEnvironmentVariable(key);
    }
    qunsetenv(key);
  }
  ~ScopedEnv() {
    for (auto it = saved_.cbegin(); it != saved_.cend(); ++it) {
      if (it.value().isNull()) {
        qunsetenv(it.key());
      } else {
        qputenv(it.key(), it.value().toUtf8());
      }
    }
  }
private:
  QHash<const char*, QString> saved_;
};

class TstAgentCrew : public QObject {
  Q_OBJECT

private:
  std::unique_ptr<sf::test::FakeComfyServer> fakeComfy_;
  std::unique_ptr<QTemporaryDir> userTemplatesDir_;
  std::unique_ptr<QTemporaryDir> jobsCacheDir_;
  std::unique_ptr<QTemporaryDir> testProjectDir_;
  std::unique_ptr<Engine> engine_;

  QString fixturesDir() const {
#ifdef SF_COMFY_FIXTURES
    return QStringLiteral(SF_COMFY_FIXTURES);
#else
    return QStringLiteral("fixtures/comfy");
#endif
  }

  QJsonObject ok(const QString& cmd, const QJsonObject& args = {}) {
    Result r = engine_->call(cmd, args);
    if (!r.ok) {
      QTest::qFail(qPrintable(QStringLiteral("%1 failed: %2 (%3)")
                                .arg(cmd, r.error.message, r.error.code)),
                   __FILE__, __LINE__);
    }
    return r.value.toObject();
  }

  Result tryCall(const QString& cmd, const QJsonObject& args = {}) {
    return engine_->call(cmd, args);
  }

  QString readTrace(const QString& dir) {
    QFile f(dir + QStringLiteral("/agent-trace.jsonl"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
  }

private slots:
  void initTestCase() {
    fakeComfy_ = std::make_unique<sf::test::FakeComfyServer>();
    fakeComfy_->setObjectInfoPath(fixturesDir() + QStringLiteral("/object_info_min.json"));
    qputenv("SF_COMFY_URL", fakeComfy_->url().toString().toUtf8());

    userTemplatesDir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(userTemplatesDir_->isValid());
    qputenv("SF_COMFY_TEMPLATES", userTemplatesDir_->path().toUtf8());

    jobsCacheDir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(jobsCacheDir_->isValid());
    qputenv("SF_JOBS_CACHE_DIR", jobsCacheDir_->path().toUtf8());

    qputenv("COMFY_ROUTER_BASE_URL", fakeComfy_->url().toString().toUtf8());
  }

  void init() {
    engine_ = std::make_unique<Engine>();
    testProjectDir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(testProjectDir_->isValid());

    ok(QStringLiteral("project.new"), {
      {QStringLiteral("name"), QStringLiteral("CrewTestProj")},
      {QStringLiteral("width"), 1920},
      {QStringLiteral("height"), 1080},
      {QStringLiteral("fps"), 30}
    });
    ok(QStringLiteral("project.saveAs"), {
      {QStringLiteral("path"), testProjectDir_->path() + QStringLiteral("/project.json")}
    });
  }

  void cleanup() {
    if (fakeComfy_) {
      fakeComfy_->setOutputKind(QStringLiteral("image"));
      fakeComfy_->setOutputFilename(QString());
      fakeComfy_->setObjectInfoPath(fixturesDir() + QStringLiteral("/object_info_min.json"));
      fakeComfy_->setObjectInfoJson(QJsonObject());
    }
    engine_.reset();
    testProjectDir_.reset();
  }

  void cleanupTestCase() {
    fakeComfy_.reset();
    userTemplatesDir_.reset();
    jobsCacheDir_.reset();
  }

  // ---- 1. Director turns a brief into a 3-scene storyboard, then toTimeline ----
  void directorBriefToStoryboardToTimeline() {
    ScopedEnv env;
    env.set("SF_AGENT_TRACE", testProjectDir_->path() + QStringLiteral("/agent-trace.jsonl"));

    sf::test::FakeLlmServer fakeLlm;
    QVERIFY(fakeLlm.listen());
    fakeLlm.start();

    // Round 0: Director calls storyboard.scene.add for 3 scenes
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QString()},
      {QStringLiteral("tool_calls"), QJsonArray{
        QJsonObject{
          {QStringLiteral("id"), QStringLiteral("c1")},
          {QStringLiteral("type"), QStringLiteral("function")},
          {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("storyboard.scene.add")},
            {QStringLiteral("arguments"), QStringLiteral("{\"id\":\"scn_1\",\"title\":\"Opening Hook\",\"intent\":\"Grab attention\",\"durationSec\":4.0}")}
          }}
        },
        QJsonObject{
          {QStringLiteral("id"), QStringLiteral("c2")},
          {QStringLiteral("type"), QStringLiteral("function")},
          {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("storyboard.scene.add")},
            {QStringLiteral("arguments"), QStringLiteral("{\"id\":\"scn_2\",\"title\":\"Product Demo\",\"intent\":\"Showcase features\",\"durationSec\":6.0}")}
          }}
        },
        QJsonObject{
          {QStringLiteral("id"), QStringLiteral("c3")},
          {QStringLiteral("type"), QStringLiteral("function")},
          {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("storyboard.scene.add")},
            {QStringLiteral("arguments"), QStringLiteral("{\"id\":\"scn_3\",\"title\":\"Call To Action\",\"intent\":\"Drive signups\",\"durationSec\":3.0}")}
          }}
        }
      }}
    });

    // Round 1: Director calls storyboard.toTimeline (the Editor reviewer has no measurements, so it approves without a model call)
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QString()},
      {QStringLiteral("tool_calls"), QJsonArray{
        QJsonObject{
          {QStringLiteral("id"), QStringLiteral("c4")},
          {QStringLiteral("type"), QStringLiteral("function")},
          {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("storyboard.toTimeline")},
            {QStringLiteral("arguments"), QStringLiteral("{}")}
          }}
        }
      }}
    });

    // Round 2: Assistant completion message
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QStringLiteral("Created a 3-scene storyboard and laid out placeholder slots and markers on the timeline.")}
    });

    auto llm = std::make_shared<HttpLlmClient>(fakeLlm.chatUrl(), QStringLiteral("test-nemotron"));
    AgentHarness harness(engine_.get(), llm);
    harness.registerCommands(*engine_);

    QJsonObject chatRes = harness.chat(QStringLiteral("Create a 3-scene video storyboard from our product brief"));
    QVERIFY(chatRes.value(QStringLiteral("ok")).toBool());
    QCOMPARE(chatRes.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));

    // Check project doc has 3 scenes
    Session* sess = engine_->session({});
    QVERIFY(sess && sess->project);
    const auto& doc = sess->project->doc();
    QCOMPARE(static_cast<int>(doc.storyboard.size()), 3);
    QCOMPARE(doc.storyboard[0].title, QStringLiteral("Opening Hook"));
    QCOMPARE(doc.storyboard[0].status, QStringLiteral("placed"));
    QCOMPARE(doc.storyboard[1].title, QStringLiteral("Product Demo"));
    QCOMPARE(doc.storyboard[2].title, QStringLiteral("Call To Action"));

    // Check timeline items and markers
    QVERIFY(!doc.tracks.empty());
    QCOMPARE(static_cast<int>(doc.items.size()), 3);
    QVERIFY(doc.markers.size() >= 3);

    // Verify trace recorded director plan on lane deep
    const QString traceContent = readTrace(testProjectDir_->path());
    QVERIFY(traceContent.contains(QStringLiteral("\"action\":\"plan\"")));
    QVERIFY(traceContent.contains(QStringLiteral("\"lane\":\"deep\"")));
    QVERIFY(traceContent.contains(QStringLiteral("\"role\":\"director\"")));
  }

  // ---- 2. Director assigns Audio, and Audio runs generate.music (fake comfy) ----
  void directorAssignsAudioGeneratesMusic() {
    ScopedEnv env;
    env.set("SF_AGENT_TRACE", testProjectDir_->path() + QStringLiteral("/agent-trace.jsonl"));

    // Save a valid music template in userTemplatesDir
    const QString gid = ok(QStringLiteral("comfy.graph.load"), {
      {QStringLiteral("path"), fixturesDir() + QStringLiteral("/txt2img_api.json")},
      {QStringLiteral("name"), QStringLiteral("BaseMusicGraph")}
    }).value(QStringLiteral("graphId")).toString();

    QJsonObject musicTmpl{
      {QStringLiteral("graphId"), gid},
      {QStringLiteral("id"), QStringLiteral("music_t3")},
      {QStringLiteral("name"), QStringLiteral("Music Gen T3")},
      {QStringLiteral("kind"), QStringLiteral("music")},
      {QStringLiteral("outputKind"), QStringLiteral("audio")},
      {QStringLiteral("bindings"), QJsonObject{
        {QStringLiteral("prompt"), QJsonObject{{QStringLiteral("node"), QStringLiteral("6")}, {QStringLiteral("input"), QStringLiteral("text")}}},
        {QStringLiteral("durationSec"), QJsonObject{{QStringLiteral("node"), QStringLiteral("5")}, {QStringLiteral("input"), QStringLiteral("width")}}},
        {QStringLiteral("seed"), QJsonObject{{QStringLiteral("node"), QStringLiteral("3")}, {QStringLiteral("input"), QStringLiteral("seed")}}}
      }}
    };
    ok(QStringLiteral("comfy.graph.saveAsTemplate"), musicTmpl);

    fakeComfy_->setOutputKind(QStringLiteral("audio"));
    fakeComfy_->setOutputFilename(QStringLiteral("synth_score.wav"));

    sf::test::FakeLlmServer fakeLlm;
    QVERIFY(fakeLlm.listen());
    fakeLlm.start();

    // 1. Director Round 0: assigns Audio via agent.assign
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QString()},
      {QStringLiteral("tool_calls"), QJsonArray{
        QJsonObject{
          {QStringLiteral("id"), QStringLiteral("d1")},
          {QStringLiteral("type"), QStringLiteral("function")},
          {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("agent.assign")},
            {QStringLiteral("arguments"), QStringLiteral("{\"to\":\"audio\",\"task\":\"Generate ambient synth soundtrack\",\"sceneId\":\"scn_1\"}")}
          }}
        }
      }}
    });

    // 2. Audio Round 0: checks readiness
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QString()},
      {QStringLiteral("tool_calls"), QJsonArray{
        QJsonObject{
          {QStringLiteral("id"), QStringLiteral("a1")},
          {QStringLiteral("type"), QStringLiteral("function")},
          {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("generate.ready")},
            {QStringLiteral("arguments"), QStringLiteral("{\"kind\":\"music\"}")}
          }}
        }
      }}
    });

    // 3. Audio Round 1: calls generate.music
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QString()},
      {QStringLiteral("tool_calls"), QJsonArray{
        QJsonObject{
          {QStringLiteral("id"), QStringLiteral("a2")},
          {QStringLiteral("type"), QStringLiteral("function")},
          {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("generate.music")},
            {QStringLiteral("arguments"), QStringLiteral("{\"templateId\":\"music_t3\",\"prompt\":\"ambient synth wave 120bpm\",\"durationSec\":30.0,\"wait\":true}")}
          }}
        }
      }}
    });

    // 4. Review by Editor on Lane Judge for Audio's music generation
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QStringLiteral("approve: audio soundtrack levels and duration verified")}
    });

    // 5. Audio Round 2: completion message
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QStringLiteral("Generated 30s ambient synth soundtrack and placed on audio track.")}
    });

    // 6. Director Round 1: completion message
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QStringLiteral("Audio specialist has generated the requested soundtrack.")}
    });

    auto llm = std::make_shared<HttpLlmClient>(fakeLlm.chatUrl(), QStringLiteral("test-nemotron"));
    AgentHarness harness(engine_.get(), llm);
    harness.registerCommands(*engine_);

    QJsonObject chatRes = harness.chat(QStringLiteral("We need energetic background music for scene 1"));
    QVERIFY(chatRes.value(QStringLiteral("ok")).toBool());



    // Verify audio generation reached fake comfy server
    QVERIFY(!fakeComfy_->queued.empty());

    // Verify handoff was recorded in trace
    const QString traceContent = readTrace(testProjectDir_->path());
    QVERIFY(traceContent.contains(QStringLiteral("\"action\":\"handoff\"")));
    QVERIFY(traceContent.contains(QStringLiteral("\"target_agent\":\"audio\"")));
    QVERIFY(traceContent.contains(QStringLiteral("\"lane\":\"fast\"")));
  }

  // ---- 3. Editor builds a graph, validate fails once, fixes with op, generation succeeds ----
  void editorBuildsGraphValidateFailsThenFixes() {
    ScopedEnv env;
    env.set("SF_AGENT_TRACE", testProjectDir_->path() + QStringLiteral("/agent-trace.jsonl"));

    // Preload base graph into comfy document state
    const QString gid = ok(QStringLiteral("comfy.graph.load"), {
      {QStringLiteral("path"), fixturesDir() + QStringLiteral("/txt2img_api.json")},
      {QStringLiteral("name"), QStringLiteral("EditGraph")}
    }).value(QStringLiteral("graphId")).toString();

    sf::test::FakeLlmServer fakeLlm;
    QVERIFY(fakeLlm.listen());
    fakeLlm.start();

    // Round 0: Editor breaks graph via op (setting steps to 0 causes out_of_range validation failure)
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QString()},
      {QStringLiteral("tool_calls"), QJsonArray{
        QJsonObject{
          {QStringLiteral("id"), QStringLiteral("e1")},
          {QStringLiteral("type"), QStringLiteral("function")},
          {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("comfy.graph.op")},
            {QStringLiteral("arguments"), QStringLiteral("{\"graphId\":\"%1\",\"ops\":[{\"op\":\"set\",\"id\":\"3\",\"input\":\"steps\",\"value\":0}]}").arg(gid)}
          }}
        }
      }}
    });

    // Round 1: Editor calls comfy.graph.validate -> returns valid: false
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QString()},
      {QStringLiteral("tool_calls"), QJsonArray{
        QJsonObject{
          {QStringLiteral("id"), QStringLiteral("e2")},
          {QStringLiteral("type"), QStringLiteral("function")},
          {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("comfy.graph.validate")},
            {QStringLiteral("arguments"), QStringLiteral("{\"graphId\":\"%1\"}").arg(gid)}
          }}
        }
      }}
    });

    // Round 2: Editor fixes graph via op (setting steps back to 20)
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QString()},
      {QStringLiteral("tool_calls"), QJsonArray{
        QJsonObject{
          {QStringLiteral("id"), QStringLiteral("e3")},
          {QStringLiteral("type"), QStringLiteral("function")},
          {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("comfy.graph.op")},
            {QStringLiteral("arguments"), QStringLiteral("{\"graphId\":\"%1\",\"ops\":[{\"op\":\"set\",\"id\":\"3\",\"input\":\"steps\",\"value\":20}]}").arg(gid)}
          }}
        }
      }}
    });

    // Round 3: Editor calls comfy.graph.validate -> returns valid: true
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QString()},
      {QStringLiteral("tool_calls"), QJsonArray{
        QJsonObject{
          {QStringLiteral("id"), QStringLiteral("e4")},
          {QStringLiteral("type"), QStringLiteral("function")},
          {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("comfy.graph.validate")},
            {QStringLiteral("arguments"), QStringLiteral("{\"graphId\":\"%1\"}").arg(gid)}
          }}
        }
      }}
    });

    // Round 4: Editor calls generate.fromGraph -> generation succeeds
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QString()},
      {QStringLiteral("tool_calls"), QJsonArray{
        QJsonObject{
          {QStringLiteral("id"), QStringLiteral("e5")},
          {QStringLiteral("type"), QStringLiteral("function")},
          {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("generate.fromGraph")},
            {QStringLiteral("arguments"), QStringLiteral("{\"graphId\":\"%1\",\"kind\":\"image\",\"prompt\":\"majestic peak at sunset\",\"wait\":true}").arg(gid)}
          }}
        }
      }}
    });

    // Round 5: Editor assistant completion message
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QStringLiteral("Workflow built, error repaired after validation, and image generated successfully.")}
    });

    auto llm = std::make_shared<HttpLlmClient>(fakeLlm.chatUrl(), QStringLiteral("test-nemotron"));
    Specialist editor(SpecialistKind::Editor, engine_.get(), nullptr, llm);
    editor.setCurrentRun(QStringLiteral("run_wf"), testProjectDir_->path());

    QJsonObject res = editor.processTurn(QStringLiteral("Build workflow, validate, and generate image"));
    QCOMPARE(res.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));

    QJsonArray toolResults = res.value(QStringLiteral("tool_results")).toArray();
    QVERIFY(toolResults.size() >= 5);

    // Validate failure was recorded in tool results
    bool sawInvalid = false;
    bool sawValid = false;
    for (const auto& trVal : toolResults) {
      QJsonObject tr = trVal.toObject();
      if (tr.value(QStringLiteral("name")).toString() == QStringLiteral("comfy.graph.validate")) {
        QJsonObject r = tr.value(QStringLiteral("result")).toObject();
        if (r.contains(QStringLiteral("result")) && r.value(QStringLiteral("result")).isObject()) {
          r = r.value(QStringLiteral("result")).toObject();
        }
        if (r.contains(QStringLiteral("valid"))) {
          if (!r.value(QStringLiteral("valid")).toBool()) sawInvalid = true;
          else sawValid = true;
        }
      }
    }
    QVERIFY(sawInvalid);
    QVERIFY(sawValid);
  }

  // ---- 4. Colourist flags a flashing generated clip on review, and Editor revises it ----
  void colouristFlagsFlashingClipReviewLoop() {
    ScopedEnv env;
    env.set("SF_AGENT_TRACE", testProjectDir_->path() + QStringLiteral("/agent-trace.jsonl"));

    // Add a video track to project
    ok(QStringLiteral("track.add"), {{QStringLiteral("kind"), QStringLiteral("video")}, {QStringLiteral("name"), QStringLiteral("V1")}});

    sf::test::FakeLlmServer fakeLlm;
    QVERIFY(fakeLlm.listen());
    fakeLlm.start();

    // Editor Round 0: Editor cuts timeline via track.add (mutating command owned by editor)
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QString()},
      {QStringLiteral("tool_calls"), QJsonArray{
        QJsonObject{
          {QStringLiteral("id"), QStringLiteral("e_cut1")},
          {QStringLiteral("type"), QStringLiteral("function")},
          {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("track.add")},
            {QStringLiteral("arguments"), QStringLiteral("{\"kind\":\"video\",\"name\":\"StrobeTrack\"}")}
          }}
        }
      }}
    });

    // Review 1 (Colourist on Lane Judge): flags flashing/strobe hazard
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QStringLiteral("revise: flash safety warning: clip introduces rapid luminance flickering at 15Hz with severe seizure risk. Please adjust transition.")}
    });

    // Editor Round 1: Editor revises by adjusting track
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QString()},
      {QStringLiteral("tool_calls"), QJsonArray{
        QJsonObject{
          {QStringLiteral("id"), QStringLiteral("e_cut2")},
          {QStringLiteral("type"), QStringLiteral("function")},
          {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("track.add")},
            {QStringLiteral("arguments"), QStringLiteral("{\"kind\":\"video\",\"name\":\"SafeTrack\"}")}
          }}
        }
      }}
    });

    // Review 2 (Colourist on Lane Judge): approves safe grade
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QStringLiteral("approve: luminance transitions are within safe broadcast standards.")}
    });

    // Editor Round 2: Assistant completion message
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QStringLiteral("Clip revised to satisfy Colourist safety review.")}
    });

    auto llm = std::make_shared<HttpLlmClient>(fakeLlm.chatUrl(), QStringLiteral("test-nemotron"));
    AgentHarness harness(engine_.get(), llm);
    harness.registerCommands(*engine_);

    Specialist* editor = harness.specialist(SpecialistKind::Editor);
    QVERIFY(editor);
    editor->setPeerReviewEnabled(true);
    editor->setCurrentRun(QStringLiteral("run_rev"), testProjectDir_->path());

    QJsonObject res = editor->processTurn(QStringLiteral("Add a clip track"));
    QCOMPARE(res.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));

    // Check trace for critique records
    const QString traceContent = readTrace(testProjectDir_->path());
    QVERIFY(traceContent.contains(QStringLiteral("\"action\":\"critique\"")));
    QVERIFY(traceContent.contains(QStringLiteral("\"lane\":\"judge\"")));
    QVERIFY(traceContent.contains(QStringLiteral("\"status\":\"needs_revision\"")));
    QVERIFY(traceContent.contains(QStringLiteral("\"status\":\"success\"")));
  }

  // ---- 5. generate.ready says not_ready, so there's no queue and the user gets a message ----
  void generateReadyNotReadyNoQueueUserMessage() {
    ScopedEnv env;
    env.set("SF_AGENT_TRACE", testProjectDir_->path() + QStringLiteral("/agent-trace.jsonl"));

    // Configure fake comfy server so it has no models or nodes for video
    const size_t queuedBefore = fakeComfy_->queued.size();

    sf::test::FakeLlmServer fakeLlm;
    QVERIFY(fakeLlm.listen());
    fakeLlm.start();

    // Round 0: Agent calls generate.ready
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QString()},
      {QStringLiteral("tool_calls"), QJsonArray{
        QJsonObject{
          {QStringLiteral("id"), QStringLiteral("r1")},
          {QStringLiteral("type"), QStringLiteral("function")},
          {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("generate.ready")},
            {QStringLiteral("arguments"), QStringLiteral("{\"kind\":\"video\"}")}
          }}
        }
      }}
    });

    // Round 1: Agent sees not_ready, does not queue generation, explains to user
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QStringLiteral("Generation is not ready: required model checkpoint flux1-schnell.safetensors is missing. Please install the model to proceed.")}
    });

    auto llm = std::make_shared<HttpLlmClient>(fakeLlm.chatUrl(), QStringLiteral("test-nemotron"));
    AgentHarness harness(engine_.get(), llm);
    harness.registerCommands(*engine_);

    QJsonObject res = harness.chat(QStringLiteral("Generate a dynamic intro video clip"));
    QVERIFY(res.value(QStringLiteral("ok")).toBool());

    // Verify no jobs were queued on ComfyUI server
    QCOMPARE(fakeComfy_->queued.size(), queuedBefore);

    // Verify user received informative message
    const QString responseText = res.value(QStringLiteral("response")).toString();
    QVERIFY(responseText.contains(QStringLiteral("missing"), Qt::CaseInsensitive) ||
            responseText.contains(QStringLiteral("not ready"), Qt::CaseInsensitive));
  }

  // ---- 6. Every mutation is undoable as one step labelled with the agent name ----
  void mutationsUndoableAsOneStepPerAgent() {
    ScopedEnv env;
    env.set("SF_AGENT_TRACE", testProjectDir_->path() + QStringLiteral("/agent-trace.jsonl"));

    sf::test::FakeLlmServer fakeLlm;
    QVERIFY(fakeLlm.listen());
    fakeLlm.start();

    // Director adds scenes and creates placeholder timeline slots in one chat turn
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QString()},
      {QStringLiteral("tool_calls"), QJsonArray{
        QJsonObject{
          {QStringLiteral("id"), QStringLiteral("m1")},
          {QStringLiteral("type"), QStringLiteral("function")},
          {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("storyboard.scene.add")},
            {QStringLiteral("arguments"), QStringLiteral("{\"id\":\"s_u1\",\"title\":\"Scene Alpha\",\"durationSec\":5.0}")}
          }}
        },
        QJsonObject{
          {QStringLiteral("id"), QStringLiteral("m2")},
          {QStringLiteral("type"), QStringLiteral("function")},
          {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("storyboard.scene.add")},
            {QStringLiteral("arguments"), QStringLiteral("{\"id\":\"s_u2\",\"title\":\"Scene Beta\",\"durationSec\":5.0}")}
          }}
        }
      }}
    });

    // Review by Editor on Lane Judge for scene additions
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QStringLiteral("approve: scene additions approved")}
    });

    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QString()},
      {QStringLiteral("tool_calls"), QJsonArray{
        QJsonObject{
          {QStringLiteral("id"), QStringLiteral("m3")},
          {QStringLiteral("type"), QStringLiteral("function")},
          {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("storyboard.toTimeline")},
            {QStringLiteral("arguments"), QStringLiteral("{}")}
          }}
        }
      }}
    });

    // Review by Editor on Lane Judge for timeline layout
    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QStringLiteral("approve: timeline placement approved")}
    });

    fakeLlm.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QStringLiteral("Storyboard built and placed.")}
    });

    auto llm = std::make_shared<HttpLlmClient>(fakeLlm.chatUrl(), QStringLiteral("test-nemotron"));
    AgentHarness harness(engine_.get(), llm);
    harness.registerCommands(*engine_);

    Session* sess = engine_->session({});
    QVERIFY(sess && sess->project);

    const int initialStoryboardCount = static_cast<int>(sess->project->doc().storyboard.size());

    const QString userPrompt = QStringLiteral("Plan and place 2 scenes");
    QJsonObject res = harness.chat(userPrompt);
    QVERIFY(res.value(QStringLiteral("ok")).toBool());

    // Mutations took effect
    QCOMPARE(static_cast<int>(sess->project->doc().storyboard.size()), initialStoryboardCount + 2);
    QVERIFY(sess->project->canUndo());
    QVERIFY(sess->project->undoLabel().contains(QStringLiteral("Director")));

    // A SINGLE undo rolls back the entire agent turn!
    Result undoRes = engine_->call(QStringLiteral("history.undo"));
    QVERIFY(undoRes.ok);

    // All storyboard scenes and placed timeline slots are completely undone
    QCOMPARE(static_cast<int>(sess->project->doc().storyboard.size()), initialStoryboardCount);
  }

  // ---- 7. Lane and model selection check (SF_LLM_MODEL_FAST, DEEP, JUDGE) and trace ----
  void laneModelSelectionAndTraceLogging() {
    ScopedEnv env;
    env.set("SF_LLM_MODEL", "model-base-general");
    env.set("SF_LLM_MODEL_FAST", "nemotron-mini-4b");
    env.set("SF_LLM_MODEL_DEEP", "nemotron-deep-70b");
    env.set("SF_LLM_MODEL_JUDGE", "nemotron-judge-8b");
    env.set("SF_AGENT_TRACE", testProjectDir_->path() + QStringLiteral("/agent-trace.jsonl"));

    LlmConfig cfg = LlmConfig::fromEnvironment();
    QCOMPARE(cfg.model, QStringLiteral("model-base-general"));
    QCOMPARE(cfg.modelFast, QStringLiteral("nemotron-mini-4b"));
    QCOMPARE(cfg.modelDeep, QStringLiteral("nemotron-deep-70b"));
    QCOMPARE(cfg.modelJudge, QStringLiteral("nemotron-judge-8b"));

    sf::test::FakeLlmServer fakeLlm;
    QVERIFY(fakeLlm.listen());
    fakeLlm.start();

    fakeLlm.enqueueChat(QJsonObject{{QStringLiteral("content"), QStringLiteral("fast answer")}});
    fakeLlm.enqueueChat(QJsonObject{{QStringLiteral("content"), QStringLiteral("deep answer")}});
    fakeLlm.enqueueChat(QJsonObject{{QStringLiteral("content"), QStringLiteral("judge answer")}});

    HttpLlmClient client(fakeLlm.chatUrl(), cfg.model);
    client.setTimeoutMs(5000);
    client.setLaneModels(cfg.modelFast, cfg.modelDeep, cfg.modelJudge);

    // Call Lane Fast
    client.complete(QStringLiteral("prompt"), {}, {}, -1, Lane::Fast);
    QCOMPARE(fakeLlm.lastChatRequest().value(QStringLiteral("model")).toString(), QStringLiteral("nemotron-mini-4b"));

    // Call Lane Deep
    client.complete(QStringLiteral("prompt"), {}, {}, -1, Lane::Deep);
    QCOMPARE(fakeLlm.lastChatRequest().value(QStringLiteral("model")).toString(), QStringLiteral("nemotron-deep-70b"));

    // Call Lane Judge
    client.complete(QStringLiteral("prompt"), {}, {}, -1, Lane::Judge);
    QCOMPARE(fakeLlm.lastChatRequest().value(QStringLiteral("model")).toString(), QStringLiteral("nemotron-judge-8b"));

    // Fallback test: unset specific model and ensure it falls back to base model
    env.unset("SF_LLM_MODEL_FAST");
    LlmConfig cfg2 = LlmConfig::fromEnvironment();
    QCOMPARE(cfg2.modelFast, QStringLiteral("model-base-general"));
  }

  // ---- 18-FIX Item 2: peer review must fail honestly - no LLM or error -> "error", never "approve" ----
  void reviewFailsHonestlyWhenLlmMissing() {
    ScopedEnv env;
    env.set("SF_AGENT_TRACE", testProjectDir_->path() + QStringLiteral("/agent-trace.jsonl"));

    // Colourist specialist with NO llm client
    Specialist colourist(SpecialistKind::Colourist, engine_.get(), nullptr, nullptr);
    colourist.setCurrentRun(QStringLiteral("run_review_missing_llm"), testProjectDir_->path());

    PeerRequest req{
        QStringLiteral("req1"),
        SpecialistKind::Editor,
        SpecialistKind::Colourist,
        QStringLiteral("review"),
        {{QStringLiteral("session"), QString()}},
        {},
        QStringLiteral("Check for flash on the graded clip"),
        true
    };

    PeerResponse res = colourist.handlePeerRequest(req, QString());

    QCOMPARE(res.status, QStringLiteral("error"));
    QVERIFY(res.note.contains(QStringLiteral("no llm client"), Qt::CaseInsensitive));
    QCOMPARE(res.outputs.value(QStringLiteral("error")).toString(), QStringLiteral("no_llm_client"));

    // Trace records the error review (not an approval)
    const QString traceContent = readTrace(testProjectDir_->path());
    QVERIFY(traceContent.contains(QStringLiteral("\"action\":\"critique\"")));
    QVERIFY(traceContent.contains(QStringLiteral("\"status\":\"error\"")));
    QVERIFY(traceContent.contains(QStringLiteral("\"error\":\"no_llm_client\"")));
  }

  // ---- 18-FIX Item 3: structured verdict - "approve, no flash risk" parses as approve, not revise ----
  void reviewVerdictApproveNoFlashRiskParsesAsApprove() {
    ScopedEnv env;
    env.set("SF_AGENT_TRACE", testProjectDir_->path() + QStringLiteral("/agent-trace.jsonl"));

    sf::test::FakeLlmServer fakeLlm;
    QVERIFY(fakeLlm.listen());
    fakeLlm.start();

    // Review returns non-JSON text starting with "approve" - must parse as approve
    fakeLlm.enqueueChat(QJsonObject{
        {QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("content"), QStringLiteral("approve, no flash risk")}
    });

    auto llm = std::make_shared<HttpLlmClient>(fakeLlm.chatUrl(), QStringLiteral("test-nemotron"));
    Specialist colourist(SpecialistKind::Colourist, engine_.get(), nullptr, llm);
    colourist.setCurrentRun(QStringLiteral("run_review_approve"), testProjectDir_->path());

    Session* sess = engine_->session({});
    QVERIFY(sess && sess->project);
    PeerRequest req{
        QStringLiteral("req2"),
        SpecialistKind::Editor,
        SpecialistKind::Colourist,
        QStringLiteral("review"),
        {{QStringLiteral("session"), sess->id}},
        {},
        QStringLiteral("Check the grade"),
        true
    };

    PeerResponse res = colourist.handlePeerRequest(req, sess->id);

    QCOMPARE(res.status, QStringLiteral("done"));
    QCOMPARE(res.outputs.value(QStringLiteral("verdict")).toString(), QStringLiteral("approve"));
    QVERIFY(res.note.contains(QStringLiteral("no flash risk"), Qt::CaseInsensitive));
  }

  // ---- 18-FIX Item 5: agent.assign target is strict - invalid name returns error, never falls back to Editor ----
  void agentAssignInvalidTargetReturnsError() {
    ScopedEnv env;
    env.set("SF_AGENT_TRACE", testProjectDir_->path() + QStringLiteral("/agent-trace.jsonl"));

    auto llm = std::make_shared<HttpLlmClient>(QStringLiteral("http://127.0.0.1:1"), QStringLiteral("test"));
    AgentHarness harness(engine_.get(), llm);
    harness.registerCommands(*engine_);

    const Result r = tryCall(QStringLiteral("agent.assign"), {{
        QStringLiteral("to"), QStringLiteral("audoi")},
        {QStringLiteral("task"), QStringLiteral("Do something")}});

    QVERIFY(!r.ok);
    QCOMPARE(r.error.code, QStringLiteral("invalid_params"));
    QVERIFY(r.error.message.contains(QStringLiteral("audoi"), Qt::CaseInsensitive));
    QVERIFY(r.error.message.contains(QStringLiteral("director"), Qt::CaseInsensitive));
    QVERIFY(r.error.message.contains(QStringLiteral("editor"), Qt::CaseInsensitive));
    QVERIFY(r.error.message.contains(QStringLiteral("colourist"), Qt::CaseInsensitive));
    QVERIFY(r.error.message.contains(QStringLiteral("audio"), Qt::CaseInsensitive));
  }

  // ---- 18-FIX Item 6: assigned task produces exactly one undo step labelled with the agent name ----
  void assignedTaskOneUndoStepLabelledWithAgentName() {
    ScopedEnv env;
    env.set("SF_AGENT_TRACE", testProjectDir_->path() + QStringLiteral("/agent-trace.jsonl"));

    // Save a music template
    const QString gid = ok(QStringLiteral("comfy.graph.load"), {{
        QStringLiteral("path"), fixturesDir() + QStringLiteral("/txt2img_api.json")},
        {QStringLiteral("name"), QStringLiteral("BaseMusicGraph")}}).value(QStringLiteral("graphId")).toString();

    QJsonObject musicTmpl{
        {QStringLiteral("graphId"), gid},
        {QStringLiteral("id"), QStringLiteral("music_t3")},
        {QStringLiteral("name"), QStringLiteral("Music Gen T3")},
        {QStringLiteral("kind"), QStringLiteral("music")},
        {QStringLiteral("outputKind"), QStringLiteral("audio")},
        {QStringLiteral("bindings"), QJsonObject{
            {QStringLiteral("prompt"), QJsonObject{{QStringLiteral("node"), QStringLiteral("6")}, {QStringLiteral("input"), QStringLiteral("text")}}},
            {QStringLiteral("durationSec"), QJsonObject{{QStringLiteral("node"), QStringLiteral("5")}, {QStringLiteral("input"), QStringLiteral("width")}}},
            {QStringLiteral("seed"), QJsonObject{{QStringLiteral("node"), QStringLiteral("3")}, {QStringLiteral("input"), QStringLiteral("seed")}}}
        }}
    };
    ok(QStringLiteral("comfy.graph.saveAsTemplate"), musicTmpl);

    fakeComfy_->setOutputKind(QStringLiteral("audio"));
    fakeComfy_->setOutputFilename(QStringLiteral("synth_score.wav"));

    sf::test::FakeLlmServer fakeLlm;
    QVERIFY(fakeLlm.listen());
    fakeLlm.start();

    // agent.assign -> Audio -> generate.music (scripted via FakeLlmServer)
    fakeLlm.enqueueChat(QJsonObject{
        {QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("content"), QString()},
        {QStringLiteral("tool_calls"), QJsonArray{
            QJsonObject{
                {QStringLiteral("id"), QStringLiteral("a1")},
                {QStringLiteral("type"), QStringLiteral("function")},
                {QStringLiteral("function"), QJsonObject{
                    {QStringLiteral("name"), QStringLiteral("generate.ready")},
                    {QStringLiteral("arguments"), QStringLiteral("{\"kind\":\"music\"}")}
                }}
            }
        }}
    });
    fakeLlm.enqueueChat(QJsonObject{
        {QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("content"), QString()},
        {QStringLiteral("tool_calls"), QJsonArray{
            QJsonObject{
                {QStringLiteral("id"), QStringLiteral("a2")},
                {QStringLiteral("type"), QStringLiteral("function")},
                {QStringLiteral("function"), QJsonObject{
                    {QStringLiteral("name"), QStringLiteral("generate.music")},
                    {QStringLiteral("arguments"), QStringLiteral("{\"templateId\":\"music_t3\",\"prompt\":\"ambient synth\",\"durationSec\":30.0,\"wait\":true}")}
                }}
            }
        }}
    });
    fakeLlm.enqueueChat(QJsonObject{
        {QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("content"), QStringLiteral("approve: music looks good")}
    });
    fakeLlm.enqueueChat(QJsonObject{
        {QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("content"), QStringLiteral("Music generated.")}
    });

    auto llm = std::make_shared<HttpLlmClient>(fakeLlm.chatUrl(), QStringLiteral("test-nemotron"));

    AgentHarness harness(engine_.get(), llm);
    harness.registerCommands(*engine_);

    // Call agent.assign directly via the engine (not through harness.chat),
    // so the undo group is labelled with the assigned agent's name.
    Session* sess = engine_->session({});
    QVERIFY(sess && sess->project);
    const QString sessionId = sess->id;

    QJsonObject assignRes = ok(QStringLiteral("agent.assign"), {{
        QStringLiteral("to"), QStringLiteral("audio")},
        {QStringLiteral("task"), QStringLiteral("Generate music")},
        {QStringLiteral("session"), sessionId}});

    // Exactly one undo step, labelled with the agent name
    QVERIFY(sess->project->canUndo());
    QCOMPARE(sess->project->undoLabel(), QStringLiteral("Audio: Generate music"));

    // One undo removes the agent's work
    QCOMPARE(ok(QStringLiteral("history.undo")).value(QStringLiteral("steps")).toInt(), 1);
  }
};

QTEST_MAIN(TstAgentCrew)
#include "tst_agent_crew.moc"
