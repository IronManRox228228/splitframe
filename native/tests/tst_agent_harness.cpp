// Integration tests for AgentHarness coordinating specialists, router, and engine in M5b.

#include "agent/agent_harness.h"
#include "agent/llm_config.h"
#include "core/schema.h"
#include "engine/engine.h"
#include "fake_llm_server.h"

#include <QTest>

using namespace sf;
using namespace sf::agent;
using namespace sf::engine;

class TstAgentHarness : public QObject {
  Q_OBJECT

  std::unique_ptr<Engine> engine_;
  std::shared_ptr<MockLlmClient> mockLlm_;
  std::unique_ptr<AgentHarness> harness_;

private slots:
  void initTestCase() {}

  void init() {
    engine_ = std::make_unique<Engine>();
    mockLlm_ = std::make_shared<MockLlmClient>();
    harness_ = std::make_unique<AgentHarness>(engine_.get(), mockLlm_);
    harness_->registerCommands(*engine_);

    engine_->call(QStringLiteral("project.new"), QJsonObject{
      {QStringLiteral("name"), QStringLiteral("HarnessTest")},
      {QStringLiteral("width"), 1920},
      {QStringLiteral("height"), 1080},
      {QStringLiteral("fps"), 30}
    });
  }

  void cleanup() {
    harness_.reset();
    mockLlm_.reset();
    engine_.reset();
  }

  // ---- command registration in engine ----
  void engineCommandsRegistered() {
    // Check that engine has agent.* commands
    QVERIFY(engine_->find(QStringLiteral("agent.turn")) != nullptr);
    QVERIFY(engine_->find(QStringLiteral("agent.mode.get")) != nullptr);
    QVERIFY(engine_->find(QStringLiteral("agent.mode.set")) != nullptr);
    QVERIFY(engine_->find(QStringLiteral("agent.team.thread")) != nullptr);
    QVERIFY(engine_->find(QStringLiteral("agent.specialists")) != nullptr);
    QVERIFY(engine_->find(QStringLiteral("agent.route")) != nullptr);

    // Call agent.specialists
    Result specRes = engine_->call(QStringLiteral("agent.specialists"));
    QVERIFY(specRes.ok);
    QJsonArray specs = specRes.value.toObject().value(QStringLiteral("specialists")).toArray();
    QCOMPARE(specs.size(), 3);
  }

  // ---- agent mode get/set ----
  void modeManagement() {
    Result getRes = engine_->call(QStringLiteral("agent.mode.get"));
    QVERIFY(getRes.ok);
    QCOMPARE(getRes.value.toObject().value(QStringLiteral("mode")).toString(), QStringLiteral("default"));

    Result setRes = engine_->call(QStringLiteral("agent.mode.set"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("plan")}});
    QVERIFY(setRes.ok);
    QCOMPARE(harness_->mode(), AgentMode::Plan);

    Result getRes2 = engine_->call(QStringLiteral("agent.mode.get"));
    QCOMPARE(getRes2.value.toObject().value(QStringLiteral("mode")).toString(), QStringLiteral("plan"));
  }

  // ---- routing command ----
  void agentRouteCommand() {
    Result r = engine_->call(QStringLiteral("agent.route"), QJsonObject{
      {QStringLiteral("message"), QStringLiteral("apply a teal and orange LUT and duck music")}
    });
    QVERIFY(r.ok);
    QJsonObject routeObj = r.value.toObject();
    QVERIFY(routeObj.value(QStringLiteral("needsPlan")).toBool());
    QJsonArray intents = routeObj.value(QStringLiteral("intents")).toArray();
    QVERIFY(intents.contains(QStringLiteral("color")));
    QVERIFY(intents.contains(QStringLiteral("music")));
  }

  // ---- plan mode generates plan without mutating ----
  void planModeGeneratesPlan() {
    harness_->setMode(AgentMode::Plan);
    QJsonObject turnRes = harness_->executeTurn(QStringLiteral("cut the pauses and add background music"));
    QVERIFY(turnRes.value(QStringLiteral("planNeeded")).toBool());
    QVERIFY(turnRes.contains(QStringLiteral("plan")));
    QJsonObject plan = turnRes.value(QStringLiteral("plan")).toObject();
    QVERIFY(plan.value(QStringLiteral("steps")).toArray().size() >= 2);
  }

  // ---- question turn is non-mutating ----
  void questionTurn() {
    mockLlm_->setDefaultResponse(QJsonObject{
      {QStringLiteral("content"), QStringLiteral("The project duration is 0 frames at 30 fps.")}
    });

    QJsonObject res = harness_->executeTurn(QStringLiteral("What is the current timeline duration?"));
    QCOMPARE(res.value(QStringLiteral("mode")).toString(), QStringLiteral("default"));
    QCOMPARE(res.value(QStringLiteral("response")).toString(), QStringLiteral("The project duration is 0 frames at 30 fps."));
    QVERIFY(!res.value(QStringLiteral("planNeeded")).toBool());
  }

  // ---- mock llm tool call execution ----
  void mockLlmToolCallExecution() {
    // Configure mock LLM to issue a peer_request to duck music once, then answer in prose
    // once the tool result is fed back (the bounded loop calls the model again).
    QJsonArray toolCalls{
      QJsonObject{
        {QStringLiteral("id"), QStringLiteral("call_123")},
        {QStringLiteral("function"), QJsonObject{
          {QStringLiteral("name"), QStringLiteral("peer_request")},
          {QStringLiteral("arguments"), QStringLiteral("{\"to\":\"audio\",\"kind\":\"duck_music\",\"args\":{\"volume\":0.2},\"reason\":\"duck under voice\"}")}
        }}
      }
    };

    mockLlm_->enqueueResponse(QJsonObject{
      {QStringLiteral("content"), QString()},
      {QStringLiteral("tool_calls"), toolCalls}
    });
    mockLlm_->setDefaultResponse(QJsonObject{
      {QStringLiteral("content"), QStringLiteral("I requested the Audio specialist to duck the music.")}
    });

    QJsonObject res = harness_->executeTurn(QStringLiteral("cut pauses in dialogue"));
    QVERIFY(res.contains(QStringLiteral("toolResults")));
    QJsonArray toolResults = res.value(QStringLiteral("toolResults")).toArray();
    QCOMPARE(toolResults.size(), 1);

    // Verify peer bus logged the request
    auto log = harness_->peerBus()->teamThread();
    QCOMPARE(static_cast<int>(log.size()), 1);
    QCOMPARE(log[0].from, SpecialistKind::Editor);
    QCOMPARE(log[0].to, SpecialistKind::Audio);
    QCOMPARE(log[0].kind, QStringLiteral("duck_music"));
  }

  // ---- Ask mode holds proposed mutations until agent.plan.run / agent.plan.skip ----
  void askModeHoldsThenRun() {
    harness_->setMode(AgentMode::Ask);
    mockLlm_->setDefaultResponse(QJsonObject{{QStringLiteral("content"), QStringLiteral("Ready to apply whenever you approve.")}});
    mockLlm_->enqueueResponse(QJsonObject{
      {QStringLiteral("content"), QString()},
      {QStringLiteral("tool_calls"), QJsonArray{QJsonObject{
        {QStringLiteral("id"), QStringLiteral("call_hold")},
        {QStringLiteral("function"), QJsonObject{
          {QStringLiteral("name"), QStringLiteral("track.add")},
          {QStringLiteral("arguments"), QStringLiteral("{\"kind\":\"video\",\"name\":\"Agent Track\"}")}
        }}
      }}}
    });

    QJsonObject res = harness_->executeTurn(QStringLiteral("add a track named Agent Track"));
    QCOMPARE(res.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    QVERIFY(res.value(QStringLiteral("pendingApproval")).toBool());
    const QJsonArray proposed = res.value(QStringLiteral("proposed")).toArray();
    QCOMPARE(proposed.size(), 1);
    QCOMPARE(proposed.at(0).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("track.add"));

    // Nothing applied yet: the fresh project still has its three default tracks.
    auto trackSize = [this]() {
      Result tl = engine_->call(QStringLiteral("timeline.get"));
      return tl.value.toObject().value(QStringLiteral("tracks")).toArray().size();
    };
    QCOMPARE(trackSize(), 3);

    // agent.plan.run applies the held proposals now.
    Result run = engine_->call(QStringLiteral("agent.plan.run"));
    QVERIFY(run.ok);
    const QJsonArray applied = run.value.toObject().value(QStringLiteral("toolResults")).toArray();
    QCOMPARE(applied.size(), 1);
    QCOMPARE(applied.at(0).toObject().value(QStringLiteral("result")).toObject().value(QStringLiteral("ok")).toBool(), true);
    QCOMPARE(trackSize(), 4);
    bool found = false;
    for (const auto& tVal : engine_->call(QStringLiteral("timeline.get")).value.toObject().value(QStringLiteral("tracks")).toArray()) {
      if (tVal.toObject().value(QStringLiteral("name")).toString() == QStringLiteral("Agent Track")) found = true;
    }
    QVERIFY(found);
    QVERIFY(!harness_->hasPending());

    // Running again with nothing pending fails honestly.
    Result rerun = engine_->call(QStringLiteral("agent.plan.run"));
    QVERIFY(!rerun.ok);
    QCOMPARE(rerun.error.code, QStringLiteral("no_pending_plan"));
  }

  // ---- Ask mode: agent.plan.skip discards the held proposals ----
  void askModeSkipDiscards() {
    harness_->setMode(AgentMode::Ask);
    mockLlm_->setDefaultResponse(QJsonObject{{QStringLiteral("content"), QStringLiteral("Proposal withheld.")}});
    mockLlm_->enqueueResponse(QJsonObject{
      {QStringLiteral("content"), QString()},
      {QStringLiteral("tool_calls"), QJsonArray{QJsonObject{
        {QStringLiteral("id"), QStringLiteral("call_skip")},
        {QStringLiteral("function"), QJsonObject{
          {QStringLiteral("name"), QStringLiteral("track.add")},
          {QStringLiteral("arguments"), QStringLiteral("{\"kind\":\"video\",\"name\":\"Discard Track\"}")}
        }}
      }}}
    });

    QJsonObject res = harness_->executeTurn(QStringLiteral("add a track named Discard Track"));
    QVERIFY(res.value(QStringLiteral("pendingApproval")).toBool());
    QVERIFY(harness_->hasPending());

    Result skip = engine_->call(QStringLiteral("agent.plan.skip"));
    QVERIFY(skip.ok);
    QVERIFY(!harness_->hasPending());

    Result run = engine_->call(QStringLiteral("agent.plan.run"));
    QVERIFY(!run.ok);
    QCOMPARE(run.error.code, QStringLiteral("no_pending_plan"));

    Result tl = engine_->call(QStringLiteral("timeline.get"));
    QCOMPARE(tl.value.toObject().value(QStringLiteral("tracks")).toArray().size(), 3);
  }

  // ---- Plan mode: agent.plan.run executes the held plan for real ----
  void planModeRunExecutes() {
    harness_->setMode(AgentMode::Plan);
    mockLlm_->setDefaultResponse(QJsonObject{{QStringLiteral("content"), QStringLiteral("Plan executed.")}});

    QJsonObject planRes = harness_->executeTurn(QStringLiteral("cut the pauses and add background music"));
    QVERIFY(planRes.value(QStringLiteral("planNeeded")).toBool());
    QVERIFY(harness_->hasPending());

    QJsonObject run = harness_->runPending();
    QCOMPARE(run.value(QStringLiteral("ok")).toBool(), true);
    // The plan runs as an auto turn (execute, no further planning) and reports the model response.
    QCOMPARE(run.value(QStringLiteral("mode")).toString(), QStringLiteral("auto"));
    QCOMPARE(run.value(QStringLiteral("response")).toString(), QStringLiteral("Plan executed."));
    QVERIFY(!harness_->hasPending());
  }

  // ---- copilot lock: however a run ends, the undo group closes and the project unlocks ----
  int markerCount() {
    return engine_->call(QStringLiteral("project.info")).value.toObject().value(QStringLiteral("markers")).toInt();
  }
  bool humanCanEdit() {
    return engine_->call(QStringLiteral("marker.add"), QJsonObject{{QStringLiteral("frame"), 1}, {QStringLiteral("label"), QStringLiteral("human")}}).ok;
  }
  static QJsonObject markerCall(const QString& id, int frame) {
    return QJsonObject{{QStringLiteral("id"), id},
                       {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), QStringLiteral("marker.add")},
                                                                {QStringLiteral("arguments"), QStringLiteral("{\"frame\":%1,\"label\":\"agent\"}").arg(frame)}}}};
  }

  void providerErrorReleasesTheLock() {
    harness_->setMode(AgentMode::Auto);
    mockLlm_->setDefaultResponse(QJsonObject{{QStringLiteral("error"), QStringLiteral("provider_unreachable")}});
    const QJsonObject res = harness_->executeTurn(QStringLiteral("add a marker at frame 3"));
    QCOMPARE(res.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    QVERIFY(humanCanEdit());
    QCOMPARE(markerCount(), 1);
  }

  void aThrowingTurnReleasesTheLock() {
    class ThrowingLlm : public LlmClient {
    public:
      QJsonObject complete(const QString&, const std::vector<ChatMessage>&, const QJsonArray&, int, Lane) override {
        throw std::runtime_error("model client blew up");
      }
    };
    harness_->setLlmClient(std::make_shared<ThrowingLlm>());
    harness_->setMode(AgentMode::Auto);
    const Result r = engine_->call(QStringLiteral("agent.turn"), QJsonObject{{QStringLiteral("message"), QStringLiteral("add a marker at frame 3")}});
    QVERIFY(!r.ok); // the engine turns the exception into an error value
    QVERIFY(humanCanEdit());
  }

  // The app wraps its model client in a RecordingLlmClient; Stop must reach the client that waits on the network,
  // not only the wrapper, or a slow model keeps the run (and the lock) alive.
  void stopReachesTheClientInsideTheRecordingWrapper() {
    auto inner = std::make_shared<MockLlmClient>();
    harness_->setLlmClient(std::make_shared<RecordingLlmClient>(inner));
    QVERIFY(!inner->cancelRequested());
    harness_->requestCancel();
    QVERIFY(inner->cancelRequested());
  }

  void everySpecialistOffersEachToolNameOnce() {
    // hosted APIs (Gemini) reject a request whose tool list repeats a function name
    for (SpecialistKind kind : {SpecialistKind::Director, SpecialistKind::Editor, SpecialistKind::Colourist, SpecialistKind::Audio}) {
      Specialist* sp = harness_->specialist(kind);
      QVERIFY(sp);
      QSet<QString> seen;
      for (const QJsonValue& t : sp->availableTools()) {
        const QString name = t.toObject().value(QStringLiteral("name")).toString();
        QVERIFY2(!seen.contains(name), qPrintable(QStringLiteral("duplicate tool: %1").arg(name)));
        seen.insert(name);
      }
    }
  }

  void agentToolListsKeepFilesAndUndoGroupsOut() {
    for (SpecialistKind kind : {SpecialistKind::Director, SpecialistKind::Editor, SpecialistKind::Colourist, SpecialistKind::Audio}) {
      QSet<QString> names;
      for (const QJsonValue& t : harness_->specialist(kind)->availableTools()) names.insert(t.toObject().value(QStringLiteral("name")).toString());
      for (const char* banned : {"project.new", "project.open", "project.save", "project.saveAs", "project.close",
                                 "session.group.begin", "session.group.end", "session.group.rollback"})
        QVERIFY2(!names.contains(QString::fromLatin1(banned)), banned);
    }
    QSet<QString> director;
    for (const QJsonValue& t : harness_->specialist(SpecialistKind::Director)->availableTools()) director.insert(t.toObject().value(QStringLiteral("name")).toString());
    // the film fast path, one hand-off tool
    QVERIFY(director.contains(QStringLiteral("edit.assemble")));
    QVERIFY(director.contains(QStringLiteral("audio.add")));
    QVERIFY(director.contains(QStringLiteral("agent.assign")));
    QVERIFY(!director.contains(QStringLiteral("peer_request")));
  }

  void stopEndsAfterTheCurrentToolCallAndCommitsOneStep() {
    harness_->setMode(AgentMode::Auto);
    // one reply asks for two markers; Stop arrives once the first call has run
    mockLlm_->enqueueResponse(QJsonObject{{QStringLiteral("content"), QString()},
                                          {QStringLiteral("tool_calls"), QJsonArray{markerCall(QStringLiteral("c1"), 5), markerCall(QStringLiteral("c2"), 6)}}});
    mockLlm_->setDefaultResponse(QJsonObject{{QStringLiteral("content"), QStringLiteral("Done.")}});
    int humanRefusals = 0;
    connect(harness_.get(), &AgentHarness::agentEvent, this, [&](const QJsonObject& ev) {
      if (ev.value(QStringLiteral("kind")).toString() != QStringLiteral("result")) return;
      // mid-run the human is locked out
      const Result human = engine_->call(QStringLiteral("marker.add"), QJsonObject{{QStringLiteral("frame"), 9}, {QStringLiteral("label"), QStringLiteral("human")}});
      if (!human.ok && human.error.code == QStringLiteral("agent_busy")) ++humanRefusals;
      harness_->requestCancel();
    });
    const QJsonObject res = harness_->executeTurn(QStringLiteral("add two markers"));
    QCOMPARE(res.value(QStringLiteral("status")).toString(), QStringLiteral("cancelled"));
    QCOMPARE(humanRefusals, 1);
    QCOMPARE(markerCount(), 1); // the second call never ran; the first stays (commit, not rollback)
    // the lock released and the run is exactly one undo step
    QVERIFY(humanCanEdit());
    QCOMPARE(markerCount(), 2);
    QCOMPARE(engine_->call(QStringLiteral("history.undo")).value.toObject().value(QStringLiteral("steps")).toInt(), 1);
    QCOMPARE(markerCount(), 1);
    QCOMPARE(engine_->call(QStringLiteral("history.undo")).value.toObject().value(QStringLiteral("steps")).toInt(), 1);
    QCOMPARE(markerCount(), 0);
    // a cancelled run does not poison the next one
    const QJsonObject next = harness_->executeTurn(QStringLiteral("add a marker at frame 3"));
    QVERIFY(next.value(QStringLiteral("status")).toString() != QStringLiteral("cancelled"));
  }

  // ---- the whole agent loop over HTTP against an OpenAI-compatible fake server ----
  void agentLoopOverHttp() {
    sf::test::FakeLlmServer fake;
    QVERIFY(fake.listen());
    fake.start();
    // Round 0: the model asks for a real engine command (wire name has dots replaced by "__").
    fake.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QString()},
      {QStringLiteral("tool_calls"), QJsonArray{QJsonObject{
        {QStringLiteral("id"), QStringLiteral("c1")},
        {QStringLiteral("type"), QStringLiteral("function")},
        {QStringLiteral("function"), QJsonObject{
          {QStringLiteral("name"), QStringLiteral("track__add")},
          {QStringLiteral("arguments"), QStringLiteral("{\"kind\":\"video\",\"name\":\"HTTP Track\"}")}
        }}
      }}}
    });
    // Round 1: final answer, no more tool calls.
    fake.enqueueChat(QJsonObject{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QStringLiteral("Added the track for you.")}
    });

    auto llm = std::make_shared<HttpLlmClient>(fake.chatUrl(), QStringLiteral("fake-model"));
    llm->setTimeoutMs(5000);
    QString probeErr;
    QVERIFY2(llm->reachable(&probeErr), qPrintable(probeErr));

    AgentHarness harness(engine_.get(), llm);
    QJsonObject res = harness.executeTurn(QStringLiteral("add a track named HTTP Track"));

    QCOMPARE(fake.chatHits(), 2);
    QCOMPARE(res.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    QCOMPARE(res.value(QStringLiteral("response")).toString(), QStringLiteral("Added the track for you."));
    const QJsonArray toolResults = res.value(QStringLiteral("toolResults")).toArray();
    QCOMPARE(toolResults.size(), 1);
    QCOMPARE(toolResults.at(0).toObject().value(QStringLiteral("result")).toObject().value(QStringLiteral("ok")).toBool(), true);

    // The second POST must carry the tool result of round 0 back to the model as role:"tool".
    const QJsonArray messages = fake.lastChatRequest().value(QStringLiteral("messages")).toArray();
    bool sawToolMessage = false;
    for (const auto& mVal : messages) {
      const QJsonObject m = mVal.toObject();
      if (m.value(QStringLiteral("role")).toString() != QStringLiteral("tool")) continue;
      sawToolMessage = true;
      QCOMPARE(m.value(QStringLiteral("tool_call_id")).toString(), QStringLiteral("c1"));
      QVERIFY(m.value(QStringLiteral("content")).toString().contains(QStringLiteral("\"ok\"")));
    }
    QVERIFY(sawToolMessage);

    // The wire tool call was mapped back to the engine command and really ran.
    Result tl = engine_->call(QStringLiteral("timeline.get"));
    QVERIFY(tl.ok);
    const QJsonArray tracks = tl.value.toObject().value(QStringLiteral("tracks")).toArray();
    QCOMPARE(tracks.size(), 4);
    bool found = false;
    for (const auto& tVal : tracks) {
      if (tVal.toObject().value(QStringLiteral("name")).toString() == QStringLiteral("HTTP Track")) found = true;
    }
    QVERIFY(found);
  }

  // ---- project summary is injected into every run ----
  void projectSummaryNamesAssets() {
    Session* sess = engine_->session();
    QVERIFY(sess && sess->project);
    Asset v;
    v.id = QStringLiteral("ast_v1");
    v.originalName = QStringLiteral("beach_sunset.mp4");
    v.path = QStringLiteral("beach_sunset.mp4");
    v.kind = AssetKind::Video;
    v.durationMs = 12500;
    v.width = 1920;
    v.height = 1080;
    v.hasAudio = true;
    sess->project->addAsset(v);

    mockLlm_->clearCalls();
    harness_->chat(QStringLiteral("make a short film"));
    QVERIFY(!mockLlm_->calls().empty());
    const QString sys = mockLlm_->calls().front().systemPrompt;
    QVERIFY(sys.contains(QStringLiteral("beach_sunset.mp4")));
    QVERIFY(sys.contains(QStringLiteral("ast_v1")));
    QVERIFY(sys.contains(QStringLiteral("12.5s")));
    QVERIFY(sys.contains(QStringLiteral("1920x1080")));
    QVERIFY(sys.contains(QStringLiteral("Timeline:")));
    QVERIFY(sys.contains(QStringLiteral("Storyboard: 0 scenes")));
    QVERIFY(!sys.contains(QStringLiteral("Project has no media yet.")));
  }

  void projectSummaryCapsAssetList() {
    Session* sess = engine_->session();
    QVERIFY(sess && sess->project);
    for (int i = 0; i < 45; ++i) {
      Asset a;
      a.id = QStringLiteral("ast_%1").arg(i);
      a.originalName = QStringLiteral("clip_%1.mp4").arg(i);
      a.path = a.originalName;
      a.kind = AssetKind::Video;
      a.durationMs = 1000;
      sess->project->addAsset(a);
    }
    mockLlm_->clearCalls();
    harness_->chat(QStringLiteral("what do I have"));
    const QString sys = mockLlm_->calls().front().systemPrompt;
    QVERIFY(sys.contains(QStringLiteral("clip_39.mp4")));
    QVERIFY(sys.contains(QStringLiteral("clip_44.mp4"))); // every asset is listed, no cap
    QVERIFY(!sys.contains(QStringLiteral(" more")));
  }

  void projectSummaryEmptyProject() {
    mockLlm_->clearCalls();
    harness_->chat(QStringLiteral("make a short film"));
    QVERIFY(!mockLlm_->calls().empty());
    QVERIFY(mockLlm_->calls().front().systemPrompt.contains(QStringLiteral("Project has no media yet.")));
  }
};

QTEST_MAIN(TstAgentHarness)
#include "tst_agent_harness.moc"
