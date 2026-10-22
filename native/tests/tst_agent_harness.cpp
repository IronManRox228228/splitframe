// Integration tests for AgentHarness coordinating specialists, router, and engine in M5b.

#include "agent/agent_harness.h"
#include "engine/engine.h"
#include "fake_llm_server.h"

#include <QTest>

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
};

QTEST_MAIN(TstAgentHarness)
#include "tst_agent_harness.moc"
