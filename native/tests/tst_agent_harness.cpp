// Integration tests for AgentHarness coordinating specialists, router, and engine in M5b.

#include "agent/agent_harness.h"
#include "engine/engine.h"

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
    // Configure mock LLM to issue a peer_request to duck music
    QJsonArray toolCalls{
      QJsonObject{
        {QStringLiteral("id"), QStringLiteral("call_123")},
        {QStringLiteral("function"), QJsonObject{
          {QStringLiteral("name"), QStringLiteral("peer_request")},
          {QStringLiteral("arguments"), QStringLiteral("{\"to\":\"audio\",\"kind\":\"duck_music\",\"args\":{\"volume\":0.2},\"reason\":\"duck under voice\"}")}
        }}
      }
    };

    mockLlm_->setDefaultResponse(QJsonObject{
      {QStringLiteral("content"), QStringLiteral("I requested the Audio specialist to duck the music.")},
      {QStringLiteral("tool_calls"), toolCalls}
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
};

QTEST_MAIN(TstAgentHarness)
#include "tst_agent_harness.moc"
