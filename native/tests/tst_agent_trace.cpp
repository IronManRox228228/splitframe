// Unit and integration tests for agent trace logging and provider error codes.

#include "agent/agent_harness.h"
#include "agent/llm_client.h"
#include "agent/peer_bus.h"
#include "agent/specialist.h"
#include "agent/trace.h"
#include "engine/engine.h"
#include "fake_llm_server.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

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
  QMap<const char*, QString> saved_;
};

class TstAgentTrace : public QObject {
  Q_OBJECT

private slots:
  void initTestCase() {}

  // ---- 1. Provider error codes from FakeLlmServer ----
  void errorCodeProviderAuth() {
    sf::test::FakeLlmServer server;
    QVERIFY(server.listen());
    server.start();

    HttpLlmClient client(server.chatUrl(), QStringLiteral("test-model"));

    server.setNextChatStatus(401);
    QJsonObject resp401 = client.complete(QStringLiteral("s"), {ChatMessage{.role = QStringLiteral("user"), .content = QStringLiteral("hi")}});
    QCOMPARE(llmErrorCode(resp401), QStringLiteral("provider_auth"));

    server.setNextChatStatus(403);
    QJsonObject resp403 = client.complete(QStringLiteral("s"), {ChatMessage{.role = QStringLiteral("user"), .content = QStringLiteral("hi")}});
    QCOMPARE(llmErrorCode(resp403), QStringLiteral("provider_auth"));
  }

  void errorCodeRateLimited() {
    sf::test::FakeLlmServer server;
    QVERIFY(server.listen());
    server.start();

    HttpLlmClient client(server.chatUrl(), QStringLiteral("test-model"));
    client.setRateLimitRetries(0);
    server.setNextChatStatus(429);
    QJsonObject resp = client.complete(QStringLiteral("s"), {ChatMessage{.role = QStringLiteral("user"), .content = QStringLiteral("hi")}});
    QCOMPARE(llmErrorCode(resp), QStringLiteral("rate_limited"));
  }

  void rateLimitWaitsOutTheProvidersHintAndRetries() {
    sf::test::FakeLlmServer server;
    QVERIFY(server.listen());
    server.start();

    HttpLlmClient client(server.chatUrl(), QStringLiteral("test-model"));
    server.setNextChatStatus(429);
    server.setNextChatBody(QJsonObject{{QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), QStringLiteral("rate_limited")},
                                                                              {QStringLiteral("message"), QStringLiteral("Quota exceeded. Please retry in 0.1s.")}}}});
    QElapsedTimer t;
    t.start();
    QJsonObject resp = client.complete(QStringLiteral("s"), {ChatMessage{.role = QStringLiteral("user"), .content = QStringLiteral("hi")}});
    QVERIFY2(llmErrorCode(resp).isEmpty(), "the retry after the hint should reach the normal reply");
    QVERIFY(t.elapsed() >= 500);
  }

  void errorCodeModelNotFound() {
    sf::test::FakeLlmServer server;
    QVERIFY(server.listen());
    server.start();

    HttpLlmClient client(server.chatUrl(), QStringLiteral("test-model"));
    server.setNextChatStatus(404);
    QJsonObject resp = client.complete(QStringLiteral("s"), {ChatMessage{.role = QStringLiteral("user"), .content = QStringLiteral("hi")}});
    QCOMPARE(llmErrorCode(resp), QStringLiteral("model_not_found"));
  }

  void errorCodeProviderError() {
    sf::test::FakeLlmServer server;
    QVERIFY(server.listen());
    server.start();

    HttpLlmClient client(server.chatUrl(), QStringLiteral("test-model"));
    server.setNextChatStatus(500);
    QJsonObject resp = client.complete(QStringLiteral("s"), {ChatMessage{.role = QStringLiteral("user"), .content = QStringLiteral("hi")}});
    QCOMPARE(llmErrorCode(resp), QStringLiteral("provider_error"));

    server.setNextChatStatus(502);
    QJsonObject resp502 = client.complete(QStringLiteral("s"), {ChatMessage{.role = QStringLiteral("user"), .content = QStringLiteral("hi")}});
    QCOMPARE(llmErrorCode(resp502), QStringLiteral("provider_error"));
  }

  void errorCodeProviderUnreachable() {
    // Port 1 is not listening -> connection refused
    HttpLlmClient client(QStringLiteral("http://127.0.0.1:1/v1/chat/completions"), QStringLiteral("test-model"));
    client.setTimeoutMs(1000);
    QJsonObject resp = client.complete(QStringLiteral("s"), {ChatMessage{.role = QStringLiteral("user"), .content = QStringLiteral("hi")}});
    QCOMPARE(llmErrorCode(resp), QStringLiteral("provider_unreachable"));
  }

  void errorCodesFromBody() {
    sf::test::FakeLlmServer server;
    QVERIFY(server.listen());
    server.start();

    HttpLlmClient client(server.chatUrl(), QStringLiteral("test-model"));
    client.setRateLimitRetries(0);

    const QStringList codes{
      QStringLiteral("provider_unreachable"),
      QStringLiteral("provider_auth"),
      QStringLiteral("model_not_found"),
      QStringLiteral("rate_limited"),
      QStringLiteral("provider_error")
    };

    for (const QString& code : codes) {
      server.setNextChatBody(QJsonObject{
        {QStringLiteral("error"), QJsonObject{
          {QStringLiteral("code"), code},
          {QStringLiteral("message"), QStringLiteral("Error test message")}
        }}
      });
      QJsonObject resp = client.complete(QStringLiteral("s"), {ChatMessage{.role = QStringLiteral("user"), .content = QStringLiteral("hi")}});
      QCOMPARE(llmErrorCode(resp), code);
    }
  }

  // ---- 2. Error codes surface in agent results and trace lines ----
  void errorCodeSurfacesInAgentTurnAndTrace() {
    ScopedEnv env;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString traceFile = dir.filePath(QStringLiteral("agent-trace.jsonl"));
    env.set("SF_AGENT_TRACE", traceFile);

    sf::test::FakeLlmServer server;
    QVERIFY(server.listen());
    server.start();

    server.setNextChatStatus(401);

    Engine engine;
    auto llm = std::make_shared<HttpLlmClient>(server.chatUrl(), QStringLiteral("test-model"));
    AgentHarness harness(&engine, llm);

    QJsonObject turnRes = harness.executeTurn(QStringLiteral("add video track"));
    QCOMPARE(turnRes.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    QCOMPARE(turnRes.value(QStringLiteral("error")).toString(), QStringLiteral("provider_auth"));

    // Check trace file
    QFile f(traceFile);
    QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(f.readAll());
    f.close();

    QVERIFY(content.contains(QStringLiteral("\"action\":\"error\"")));
    QVERIFY(content.contains(QStringLiteral("\"error\":\"provider_auth\"")));
    QVERIFY(content.contains(QStringLiteral("\"status\":\"error\"")));
  }

  // ---- 3. Strip reasoning from answers before use ----
  void stripReasoningFencesAndFields() {
    // 1. XML-style <think>...</think>
    QJsonObject msg1{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QStringLiteral("<think>\nInternal analysis of video\n</think>\nCut applied at 00:05.")}
    };
    QJsonObject stripped1 = stripReasoning(msg1);
    QCOMPARE(stripped1.value(QStringLiteral("content")).toString(), QStringLiteral("Cut applied at 00:05."));

    // 2. Markdown ```think...```
    QJsonObject msg2{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QStringLiteral("```think\nDeliberating\n```\nGrade updated.")}
    };
    QJsonObject stripped2 = stripReasoning(msg2);
    QCOMPARE(stripped2.value(QStringLiteral("content")).toString(), QStringLiteral("Grade updated."));

    // 3. Unclosed <think>
    QJsonObject msg3{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QStringLiteral("<think>\nCut off mid-thought")}
    };
    QJsonObject stripped3 = stripReasoning(msg3);
    QCOMPARE(stripped3.value(QStringLiteral("content")).toString(), QString());

    // 4. Separate reasoning and reasoning_content fields
    QJsonObject msg4{
      {QStringLiteral("role"), QStringLiteral("assistant")},
      {QStringLiteral("content"), QStringLiteral("Finished.")},
      {QStringLiteral("reasoning"), QStringLiteral("Hidden chain-of-thought")},
      {QStringLiteral("reasoning_content"), QStringLiteral("DeepSeek style field")}
    };
    QJsonObject stripped4 = stripReasoning(msg4);
    QCOMPARE(stripped4.value(QStringLiteral("content")).toString(), QStringLiteral("Finished."));
    QVERIFY(!stripped4.contains(QStringLiteral("reasoning")));
    QVERIFY(!stripped4.contains(QStringLiteral("reasoning_content")));
  }

  // ---- 4. Observed model (reported by response, not requested) ----
  void observedModelReportedInTrace() {
    ScopedEnv env;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString traceFile = dir.filePath(QStringLiteral("agent-trace.jsonl"));
    env.set("SF_AGENT_TRACE", traceFile);

    sf::test::FakeLlmServer server;
    QVERIFY(server.listen());
    server.start();

    // Server substitutes the model
    server.setNextChatBody(QJsonObject{
      {QStringLiteral("model"), QStringLiteral("provider-substituted-deep-70b")},
      {QStringLiteral("choices"), QJsonArray{QJsonObject{
        {QStringLiteral("message"), QJsonObject{
          {QStringLiteral("role"), QStringLiteral("assistant")},
          {QStringLiteral("content"), QStringLiteral("All done.")}
        }}
      }}}
    });

    Engine engine;
    auto llm = std::make_shared<HttpLlmClient>(server.chatUrl(), QStringLiteral("requested-fast-model"));
    AgentHarness harness(&engine, llm);

    QJsonObject res = harness.executeTurn(QStringLiteral("What is the state?"));
    QCOMPARE(llm->lastObservedModel(), QStringLiteral("provider-substituted-deep-70b"));

    // Check trace file contains observed model
    QFile f(traceFile);
    QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(f.readAll());
    f.close();

    QVERIFY(content.contains(QStringLiteral("\"model\":\"provider-substituted-deep-70b\"")));
    QVERIFY(!content.contains(QStringLiteral("\"model\":\"requested-fast-model\"")));
  }

  // ---- 5. Trace lines appear for a handoff and a critique ----
  void traceLinesForHandoffAndCritique() {
    ScopedEnv env;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString traceFile = dir.filePath(QStringLiteral("agent-trace.jsonl"));
    env.set("SF_AGENT_TRACE", traceFile);

    PeerBus bus;

    // Register a handoff handler on Audio
    bus.registerHandler(SpecialistKind::Audio, QStringLiteral("duck_music"), [](const PeerRequest&) {
      PeerResponse res;
      res.status = QStringLiteral("done");
      res.note = QStringLiteral("ducked music to 0.2");
      return res;
    });

    // Register a critique handler on Colourist
    bus.registerHandler(SpecialistKind::Colourist, QStringLiteral("critique"), [](const PeerRequest&) {
      PeerResponse res;
      res.status = QStringLiteral("done");
      res.note = QStringLiteral("colour grading passed review");
      return res;
    });

    // 1. Dispatch handoff: Editor -> Audio
    PeerRequest handoffReq{
      .from = SpecialistKind::Editor,
      .to = SpecialistKind::Audio,
      .kind = QStringLiteral("duck_music"),
      .args = QJsonObject{{QStringLiteral("volume"), 0.2}},
      .reason = QStringLiteral("duck under dialogue")
    };
    PeerResponse handoffResp = bus.send(handoffReq);
    QCOMPARE(handoffResp.status, QStringLiteral("done"));

    // 2. Dispatch critique: Editor -> Colourist
    PeerRequest critiqueReq{
      .from = SpecialistKind::Editor,
      .to = SpecialistKind::Colourist,
      .kind = QStringLiteral("critique"),
      .args = QJsonObject{{QStringLiteral("clipId"), QStringLiteral("clip_1")}},
      .reason = QStringLiteral("verify colour balance")
    };
    PeerResponse critiqueResp = bus.send(critiqueReq);
    QCOMPARE(critiqueResp.status, QStringLiteral("done"));

    // Read trace file and assert handoff and critique lines
    QFile f(traceFile);
    QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(f.readAll());
    f.close();

    // Verify handoff
    QVERIFY(content.contains(QStringLiteral("\"action\":\"handoff\"")));
    QVERIFY(content.contains(QStringLiteral("\"agent_name\":\"editor\"")));
    QVERIFY(content.contains(QStringLiteral("\"target_agent\":\"audio\"")));
    QVERIFY(content.contains(QStringLiteral("\"tool\":\"duck_music\"")));

    // Verify critique
    QVERIFY(content.contains(QStringLiteral("\"action\":\"critique\"")));
    QVERIFY(content.contains(QStringLiteral("\"agent_name\":\"editor\"")));
    QVERIFY(content.contains(QStringLiteral("\"target_agent\":\"colourist\"")));
    QVERIFY(content.contains(QStringLiteral("\"tool\":\"critique\"")));
  }

  // ---- 6. No token string ever appears in the trace file ----
  void noTokenStringEverAppearsInTrace() {
    ScopedEnv env;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString traceFile = dir.filePath(QStringLiteral("agent-trace.jsonl"));
    env.set("SF_AGENT_TRACE", traceFile);

    const QString secretKey = QStringLiteral("super-secret-api-key-do-not-leak-998877");
    env.set("SF_LLM_API_KEY", secretKey);

    sf::test::FakeLlmServer server;
    QVERIFY(server.listen());
    server.start();

    Engine engine;
    auto llm = std::make_shared<HttpLlmClient>(server.chatUrl(), QStringLiteral("test-model"));
    llm->setApiKey(secretKey);

    AgentHarness harness(&engine, llm);
    harness.registerCommands(engine);

    // Run turn which makes network call and writes trace
    harness.executeTurn(QStringLiteral("What is the timeline duration?"));

    // Verify file exists
    QFile f(traceFile);
    QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(f.readAll());
    f.close();

    QVERIFY(!content.isEmpty());
    // Strict assertion: secret key / token NEVER appears anywhere in the trace log
    QVERIFY(!content.contains(secretKey));
    QVERIFY(!content.contains(QStringLiteral("super-secret")));
    QVERIFY(!content.contains(QStringLiteral("Bearer super-secret-api-key-do-not-leak-998877")));
  }

  // ---- 7. agent.trace command reads back last N lines ----
  void agentTraceCommand() {
    ScopedEnv env;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString traceFile = dir.filePath(QStringLiteral("agent-trace.jsonl"));
    env.set("SF_AGENT_TRACE", traceFile);

    Engine engine;
    auto mock = std::make_shared<MockLlmClient>();
    mock->setDefaultResponse(QJsonObject{{QStringLiteral("content"), QStringLiteral("Answer 1")}});

    AgentHarness harness(&engine, mock);
    harness.registerCommands(engine);

    // Execute 4 turns
    harness.executeTurn(QStringLiteral("turn 1"));
    harness.executeTurn(QStringLiteral("turn 2"));
    harness.executeTurn(QStringLiteral("turn 3"));
    harness.executeTurn(QStringLiteral("turn 4"));

    // Read back with limit = 2
    Result resLimit2 = engine.call(QStringLiteral("agent.trace"), QJsonObject{{QStringLiteral("limit"), 2}});
    QVERIFY(resLimit2.ok);
    QJsonArray lines2 = resLimit2.value.toObject().value(QStringLiteral("entries")).toArray();
    QCOMPARE(lines2.size(), 2);

    // Read back without limit
    Result resAll = engine.call(QStringLiteral("agent.trace"));
    QVERIFY(resAll.ok);
    QJsonArray linesAll = resAll.value.toObject().value(QStringLiteral("entries")).toArray();
    QVERIFY(linesAll.size() >= 4);
  }

  // ---- 8. Trace disabled via SF_AGENT_TRACE="off" ----
  void traceDisabledViaEnv() {
    ScopedEnv env;
    env.set("SF_AGENT_TRACE", QStringLiteral("off"));

    const QString path = resolveTracePath();
    QVERIFY(path.isEmpty());

    // Calling logAgentTrace does not crash and does nothing
    logAgentTrace(AgentTraceRecord{
      .action = QStringLiteral("plan"),
      .status = QStringLiteral("success")
    });

    QJsonObject traceRes = readAgentTrace();
    QVERIFY(traceRes.value(QStringLiteral("disabled")).toBool());
  }

  // ---- 9. Default trace path resolution ----
  void defaultTracePathResolution() {
    ScopedEnv env;
    env.set("SF_AGENT_TRACE", QString()); // unset

    // With a project dir
    const QString pDir = QStringLiteral("C:/test_project");
    const QString resolvedProj = resolveTracePath(pDir);
    QCOMPARE(resolvedProj, QStringLiteral("C:/test_project/agent-trace.jsonl"));

    // Without a project dir -> AppLocalDataLocation
    const QString resolvedDefault = resolveTracePath();
    QVERIFY(!resolvedDefault.isEmpty());
    QVERIFY(resolvedDefault.endsWith(QStringLiteral("agent-trace.jsonl")));
  }
};

QTEST_MAIN(TstAgentTrace)
#include "tst_agent_trace.moc"
