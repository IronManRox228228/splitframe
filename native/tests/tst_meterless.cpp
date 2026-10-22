#include "agent/meterless_client.h"
#include "agent/llm_config.h"
#include "fake_llm_server.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QTemporaryDir>

using namespace sf;
using namespace sf::agent;
using sf::test::FakeLlmServer;

namespace {

void clearProviderEnv() {
  for (const char* k : {"SF_LLM_ENDPOINT", "SF_LLM_MODEL", "SF_LLM_API_KEY", "SF_LLM_TIMEOUT_MS", "SF_LLM_MOCK_SCRIPT",
                        "MODEL_BASE_URL", "MODEL_API_KEY", "NEMOTRON_MODEL",
                        "METERLESS_BASE_URL", "METERLESS_RUNTIME_TOKEN", "METERLESS_MODEL",
                        "METERLESS_LANE_FAST", "METERLESS_LANE_DEEP", "METERLESS_LANE_JUDGE"}) {
    qunsetenv(k);
  }
  // Tests are offline by default; the trace-logging test opts back in with its own path.
  qputenv("SF_AGENT_TRACE", "off");
}

// The unconfigured state: no METERLESS_*, no Nightshift token file, nothing SF_LLM_*.
class ScopedEnv {
public:
  ScopedEnv() { clearProviderEnv(); }
  ~ScopedEnv() { clearProviderEnv(); }
};

std::vector<ChatMessage> oneUser(const QString& text) {
  return {ChatMessage{QStringLiteral("user"), text}};
}

} // namespace

class TestMeterless : public QObject {
  Q_OBJECT

private slots:
  // Part A item 2: no invented endpoint or model id. Unset config fails with a clear error
  // instead of pretending api.meterless.ai exists; the routing default is meterless/auto.
  void routedDefaults() {
    ScopedEnv env;
    MeterlessClient client;
    QVERIFY(client.baseUrl().isEmpty());
    QVERIFY(!client.configured());
    QCOMPARE(client.routingMode(), QStringLiteral("meterless/auto"));
    // Lane defaults (the scoping note): Fast = text.fast, Deep = text.deep, Judge = judge.auto.
    QCOMPARE(client.modelForLane(Lane::Fast), QStringLiteral("meterless/text.fast"));
    QCOMPARE(client.modelForLane(Lane::Deep), QStringLiteral("meterless/text.deep"));
    QCOMPARE(client.modelForLane(Lane::Judge), QStringLiteral("meterless/judge.auto"));

    QJsonObject resp = client.complete(QStringLiteral("sys"), oneUser(QStringLiteral("hi")));
    QCOMPARE(meterlessErrorCode(resp), QStringLiteral("provider_unreachable"));

    QJsonObject disc = client.discoverModels();
    QCOMPARE(disc.value(QStringLiteral("ok")).toBool(), false);
    QCOMPARE(disc.value(QStringLiteral("error")).toString(), QStringLiteral("provider_unreachable"));
    QString why;
    QCOMPARE(client.reachable(&why), false);
    QCOMPARE(why, QStringLiteral("provider_unreachable"));
  }

  // Part A item 2: METERLESS_BASE_URL / METERLESS_MODEL / METERLESS_LANE_* must be honored.
  void environmentIsRead() {
    ScopedEnv env;
    qputenv("METERLESS_BASE_URL", "http://10.0.0.7:9000");
    qputenv("METERLESS_MODEL", "meterless/text.fast");
    qputenv("METERLESS_LANE_DEEP", "meterless/text.deep");
    MeterlessClient client;
    QCOMPARE(client.baseUrl(), QStringLiteral("http://10.0.0.7:9000"));
    QCOMPARE(client.routingMode(), QStringLiteral("meterless/text.fast"));
    QCOMPARE(client.modelForLane(Lane::Fast), QStringLiteral("meterless/text.fast"));
    QCOMPARE(client.modelForLane(Lane::Deep), QStringLiteral("meterless/text.deep"));
    QCOMPARE(client.modelForLane(Lane::Judge), QStringLiteral("meterless/text.fast"));
    QVERIFY(client.configured());
  }

  // Part A item 9: token from METERLESS_RUNTIME_TOKEN wins; else Nightshift's proxy-token file.
  // The token itself is never logged or displayed: no getter, trace line only carries models.
  void runtimeTokenSources() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QDir(dir.path()).mkpath(QStringLiteral("ai.meterless.runtime"));
    QFile tokenFile(dir.path() + QStringLiteral("/ai.meterless.runtime/proxy-token"));
    QVERIFY(tokenFile.open(QIODevice::WriteOnly | QIODevice::Text));
    tokenFile.write("  file-token-123 \n");
    tokenFile.close();

    ScopedEnv env;
    qputenv("LOCALAPPDATA", dir.path().toUtf8());

    FakeLlmServer server;
    QVERIFY(server.listen());
    server.start();
    MeterlessClient fromFile;
    fromFile.setBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
    QJsonObject resp = fromFile.complete(QStringLiteral("sys"), oneUser(QStringLiteral("hi")));
    QVERIFY(!resp.contains(QStringLiteral("error")));
    QCOMPARE(server.header("authorization"), QByteArrayLiteral("Bearer file-token-123"));

    // The env var beats the file; both are read without ever being printed.
    qputenv("METERLESS_RUNTIME_TOKEN", "env-token-456");
    MeterlessClient fromEnv;
    fromEnv.setBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
    resp = fromEnv.complete(QStringLiteral("sys"), oneUser(QStringLiteral("hi")));
    QVERIFY(!resp.contains(QStringLiteral("error")));
    QCOMPARE(server.header("authorization"), QByteArrayLiteral("Bearer env-token-456"));

    // No token at all: requests still go out (the runtime answers or rejects them).
    qunsetenv("METERLESS_RUNTIME_TOKEN");
    QFile::remove(dir.path() + QStringLiteral("/ai.meterless.runtime/proxy-token"));
    MeterlessClient none;
    none.setBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
    resp = none.complete(QStringLiteral("sys"), oneUser(QStringLiteral("hi")));
    QVERIFY(!resp.contains(QStringLiteral("error")));
    QVERIFY(server.header("authorization").isEmpty());
  }

  // Part A item 9: the runtime contract — Bearer + meterless-app always, meterless-scope on chat,
  // POST {base}/v1/chat/completions, and max_tokens sized for reasoning plus the answer.
  void runtimeContractHeaders() {
    ScopedEnv env;
    FakeLlmServer server;
    QVERIFY(server.listen());
    server.start();
    MeterlessClient client(QStringLiteral("http://127.0.0.1:%1").arg(server.port()), QStringLiteral("tok"), QStringLiteral("meterless/auto"));
    QJsonObject resp = client.complete(QStringLiteral("sys"), oneUser(QStringLiteral("hi")));
    QVERIFY(!resp.contains(QStringLiteral("error")));
    QCOMPARE(server.chatHits(), 1);
    QCOMPARE(server.header("meterless-app"), QByteArrayLiteral("splitframe"));
    QCOMPARE(server.header("meterless-scope"), QByteArrayLiteral("user"));
    QCOMPARE(server.header("authorization"), QByteArrayLiteral("Bearer tok"));
    const QJsonObject sent = server.lastChatRequest();
    QCOMPARE(sent.value(QStringLiteral("model")).toString(), QStringLiteral("meterless/auto"));
    QVERIFY(sent.value(QStringLiteral("max_tokens")).toInt() >= 4096);

    // Discovery GETs {base}/v1/models with Bearer + meterless-app, without the chat scope header.
    QJsonObject status = client.statusJson();
    QCOMPARE(status.value(QStringLiteral("provider")).toString(), QStringLiteral("meterless"));
    QCOMPARE(status.value(QStringLiteral("reachable")).toBool(), true);
    QCOMPARE(status.value(QStringLiteral("baseUrl")).toString(), QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
    QCOMPARE(status.value(QStringLiteral("models")).toArray().first().toString(), QStringLiteral("fake-model"));
    QCOMPARE(status.value(QStringLiteral("lanes")).toObject().value(QStringLiteral("fast")).toString(), QStringLiteral("meterless/auto"));
    QCOMPARE(server.modelsHits(), 1);
    // The last request was the discovery GET: meterless-app rides along, the chat scope does not.
    QCOMPARE(server.header("meterless-app"), QByteArrayLiteral("splitframe"));
    QVERIFY(server.header("meterless-scope").isEmpty());
  }

  // Part A item 4: the lane picks its configured model. SplitFrame's lane defaults (the lead's
  // scoping note) are Fast = meterless/text.fast, Deep = meterless/text.deep, Judge = meterless/judge.auto.
  void lanePicksItsModel() {
    ScopedEnv env;
    FakeLlmServer server;
    QVERIFY(server.listen());
    server.start();
    MeterlessClient client(QStringLiteral("http://127.0.0.1:%1").arg(server.port()), QStringLiteral("tok"), QStringLiteral("meterless/auto"));
    client.setModelForLane(Lane::Deep, QStringLiteral("meterless/text.deep"));
    client.setModelForLane(Lane::Judge, QStringLiteral("meterless/judge.v1"));

    client.complete(QStringLiteral("s"), oneUser(QStringLiteral("q"))); // default lane = Fast
    QCOMPARE(server.lastChatRequest().value(QStringLiteral("model")).toString(), QStringLiteral("meterless/auto"));
    client.complete(QStringLiteral("s"), oneUser(QStringLiteral("q")), QJsonArray{}, -1, Lane::Deep);
    QCOMPARE(server.lastChatRequest().value(QStringLiteral("model")).toString(), QStringLiteral("meterless/text.deep"));
    client.complete(QStringLiteral("s"), oneUser(QStringLiteral("q")), QJsonArray{}, -1, Lane::Judge);
    QCOMPARE(server.lastChatRequest().value(QStringLiteral("model")).toString(), QStringLiteral("meterless/judge.v1"));

    // Env-configured lanes land on the client too (METERLESS_LANE_* readable per item 4).
    qputenv("METERLESS_LANE_FAST", "meterless/text.local");
    MeterlessClient envLanes;
    QCOMPARE(envLanes.modelForLane(Lane::Fast), QStringLiteral("meterless/text.local"));
  }

  // Part A item 6: the four provider error codes.
  void providerErrorCodes() {
    ScopedEnv env;
    FakeLlmServer server;
    QVERIFY(server.listen());
    server.start();
    MeterlessClient client(QStringLiteral("http://127.0.0.1:%1").arg(server.port()), QStringLiteral("tok"), {});

    server.setNextChatStatus(401);
    QJsonObject resp = client.complete(QStringLiteral("s"), oneUser(QStringLiteral("q")));
    QCOMPARE(meterlessErrorCode(resp), QStringLiteral("provider_auth"));

    server.setNextChatStatus(429);
    resp = client.complete(QStringLiteral("s"), oneUser(QStringLiteral("q")));
    QCOMPARE(meterlessErrorCode(resp), QStringLiteral("rate_limited"));

    server.setNextChatStatus(404);
    resp = client.complete(QStringLiteral("s"), oneUser(QStringLiteral("q")));
    QCOMPARE(meterlessErrorCode(resp), QStringLiteral("model_not_found"));
  }

  // Part A item 9: Nightshift may substitute the model (record the response's model, never assume
  // the requested id) and reasoning never reaches the answer or the trace.
  void observedModelAndReasoningStrip() {
    ScopedEnv env;
    FakeLlmServer server;
    QVERIFY(server.listen());
    server.start();
    MeterlessClient client(QStringLiteral("http://127.0.0.1:%1").arg(server.port()), QStringLiteral("tok"), QStringLiteral("meterless/auto"));
    server.setNextChatBody(QJsonObject{
      {QStringLiteral("model"), QStringLiteral("nvidia/nemotron-ultra-substituted")},
      {QStringLiteral("choices"), QJsonArray{QJsonObject{{QStringLiteral("message"), QJsonObject{
        {QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("reasoning"), QStringLiteral("The user wants me to check the clip.")},
        {QStringLiteral("content"), QStringLiteral("```think\nThe user wants me to check the clip.\n```\nChecked: the clip is fine.")}}}}}}});

    QJsonObject resp = client.complete(QStringLiteral("s"), oneUser(QStringLiteral("q")));
    QCOMPARE(client.lastObservedModel(), QStringLiteral("nvidia/nemotron-ultra-substituted"));
    QCOMPARE(resp.value(QStringLiteral("content")).toString(), QStringLiteral("Checked: the clip is fine."));
    QVERIFY(!resp.contains(QStringLiteral("reasoning")));
    QVERIFY(!resp.contains(QStringLiteral("reasoning_content")));
  }

  // Part A item 79: agent-trace.jsonl records which model actually answered, and never the token.
  void traceLogsTheModelNotTheToken() {
    ScopedEnv env;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    qputenv("SF_AGENT_TRACE", (dir.path() + QStringLiteral("/agent-trace.jsonl")).toUtf8());
    FakeLlmServer server;
    QVERIFY(server.listen());
    server.start();
    MeterlessClient client(QStringLiteral("http://127.0.0.1:%1").arg(server.port()), QStringLiteral("secret-token-do-not-log"), {});
    server.setNextChatBody(QJsonObject{
      {QStringLiteral("model"), QStringLiteral("observed-model-7b")},
      {QStringLiteral("choices"), QJsonArray{QJsonObject{{QStringLiteral("message"), QJsonObject{{QStringLiteral("content"), QStringLiteral("ok")}}}}}}});
    client.complete(QStringLiteral("s"), oneUser(QStringLiteral("q")));

    QFile trace(dir.path() + QStringLiteral("/agent-trace.jsonl"));
    QVERIFY(trace.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString lines = QString::fromUtf8(trace.readAll());
    trace.close();
    QVERIFY(lines.contains(QStringLiteral("\"event\":\"llm_response\"")));
    QVERIFY(lines.contains(QStringLiteral("\"model\":\"observed-model-7b\"")));
    QVERIFY(!lines.contains(QStringLiteral("secret-token-do-not-log")));
  }

  void testMockCompletionAndTelemetry() {
    ScopedEnv env;
    MeterlessClient client;
    client.setMockHandler([](const QString& sysPrompt,
                             const std::vector<ChatMessage>& msgs,
                             const QJsonArray& tools) -> QJsonObject {
      Q_UNUSED(sysPrompt);
      Q_UNUSED(msgs);
      Q_UNUSED(tools);
      return QJsonObject{
        {QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("content"), QStringLiteral("Telemetry verified.")},
        {QStringLiteral("usage"), QJsonObject{
          {QStringLiteral("prompt_tokens"), 42},
          {QStringLiteral("completion_tokens"), 18}
        }}
      };
    });

    QVERIFY(client.hasMockHandler());

    std::vector<ChatMessage> messages = {
      ChatMessage{QStringLiteral("user"), QStringLiteral("Test request.")}
    };

    QJsonObject resp = client.complete(QStringLiteral("You are an assistant."), messages);
    QCOMPARE(resp.value(QStringLiteral("content")).toString(), QStringLiteral("Telemetry verified."));

    const auto& tel = client.telemetry();
    QCOMPARE(tel.totalCalls, 1);
    QCOMPARE(tel.promptTokens, 42);
    QCOMPARE(tel.completionTokens, 18);
    QCOMPARE(tel.totalTokens, 60);
    QVERIFY(tel.lastLatencyMs >= 0);

    client.resetTelemetry();
    QCOMPARE(client.telemetry().totalCalls, 0);
    QCOMPARE(client.telemetry().totalTokens, 0);
  }

  // Part A item 5: Meterless primary, one retry on the local provider for retriable errors only.
  void fallbackToLocalProvider() {
    ScopedEnv env;
    qputenv("SF_AGENT_TRACE", "off");
    FakeLlmServer meterless;
    QVERIFY(meterless.listen());
    meterless.start();
    auto primary = std::make_shared<MeterlessClient>(
      QStringLiteral("http://127.0.0.1:%1").arg(meterless.port()), QStringLiteral("tok"), QStringLiteral("meterless/auto"));

    FakeLlmServer local;
    QVERIFY(local.listen());
    local.start();
    auto fallback = std::make_shared<HttpLlmClient>(local.chatUrl(), QStringLiteral("qwythos-9b-v2"));

    FallbackLlmClient fb(primary, fallback);
    QString noticed;
    fb.setNoticeSink([&](const QString& n) { noticed = n; });

    // Healthy runtime: no fallback, the answer comes from Meterless.
    QJsonObject resp = fb.complete(QStringLiteral("s"), oneUser(QStringLiteral("q")));
    QVERIFY(!resp.contains(QStringLiteral("error")));
    QCOMPARE(fb.fallbackCount(), 0);
    QCOMPARE(meterless.chatHits(), 1);
    QCOMPARE(local.chatHits(), 0);

    // Rejected model id is never retried locally: the user must see it (item 5).
    meterless.setNextChatStatus(404);
    resp = fb.complete(QStringLiteral("s"), oneUser(QStringLiteral("q")));
    QCOMPARE(meterlessErrorCode(resp), QStringLiteral("model_not_found"));
    QCOMPARE(local.chatHits(), 0);

    // A bad token is likewise surfaced, never swapped for local answers.
    meterless.setNextChatStatus(401);
    resp = fb.complete(QStringLiteral("s"), oneUser(QStringLiteral("q")));
    QCOMPARE(meterlessErrorCode(resp), QStringLiteral("provider_auth"));
    QCOMPARE(local.chatHits(), 0);

    // Rate limited: one automatic retry on the local provider, marked and noticed.
    meterless.setNextChatStatus(429);
    resp = fb.complete(QStringLiteral("s"), oneUser(QStringLiteral("q")));
    QVERIFY(!resp.contains(QStringLiteral("error")));
    QCOMPARE(resp.value(QStringLiteral("content")).toString(), QStringLiteral("ok"));
    QCOMPARE(resp.value(QStringLiteral("service")).toString(), QStringLiteral("local"));
    QCOMPARE(resp.value(QStringLiteral("fallback_from")).toString(), QStringLiteral("meterless"));
    QVERIFY(!resp.value(QStringLiteral("fallback_notice")).toString().isEmpty());
    QCOMPARE(fb.fallbackCount(), 1);
    QCOMPARE(fb.lastFallback(), QStringLiteral("local"));
    QVERIFY(!noticed.isEmpty());
    QCOMPARE(local.chatHits(), 1);
  }

  // Part A item 5: with no explicit SF_LLM_* choice, a Meterless runtime is the default provider
  // and the stack is Meterless + local fallback; an explicit SF_LLM_* selection keeps the local one.
  void providerDefaultAndAliases() {
    QTemporaryDir emptyHome;
    QVERIFY(emptyHome.isValid());
    ScopedEnv env;
    qputenv("LOCALAPPDATA", emptyHome.path().toUtf8()); // no Nightshift token file

    // Alias keys from the setup guide onto the generic provider.
    qputenv("MODEL_BASE_URL", "http://10.1.1.5:8000");
    qputenv("NEMOTRON_MODEL", "nvidia/nemotron-3-ultra");
    qputenv("MODEL_API_KEY", "alias-key");
    LlmConfig aliased = LlmConfig::fromEnvironment();
    QCOMPARE(aliased.endpoint, QStringLiteral("http://10.1.1.5:8000"));
    QCOMPARE(aliased.model, QStringLiteral("nvidia/nemotron-3-ultra"));
    QCOMPARE(aliased.apiKey, QStringLiteral("alias-key"));
    QCOMPARE(aliased.meterless, false); // an aliased endpoint is an explicit local choice

    // SF_LLM_* wins over the aliases.
    qputenv("SF_LLM_ENDPOINT", "http://10.2.2.2:1234");
    LlmConfig explicitWin = LlmConfig::fromEnvironment();
    QCOMPARE(explicitWin.endpoint, QStringLiteral("http://10.2.2.2:1234"));
    QCOMPARE(explicitWin.apiKey, QStringLiteral("alias-key"));
    qunsetenv("SF_LLM_ENDPOINT");
    qunsetenv("MODEL_API_KEY");
    qputenv("SF_LLM_API_KEY", "generic-key");
    QCOMPARE(LlmConfig::fromEnvironment().apiKey, QStringLiteral("generic-key"));
    qunsetenv("SF_LLM_API_KEY");
    qunsetenv("MODEL_BASE_URL");
    qunsetenv("NEMOTRON_MODEL");

    // Nothing configured at all: local provider (no invented Meterless default).
    QCOMPARE(LlmConfig::fromEnvironment().meterless, false);

    // METERLESS_BASE_URL present and no explicit SF_LLM_*: Meterless is the default provider.
    qputenv("METERLESS_BASE_URL", "http://127.0.0.1:1");
    const LlmConfig meterless = LlmConfig::fromEnvironment();
    QCOMPARE(meterless.meterless, true);
    QString error;
    auto client = makeLlmClient(meterless, &error);
    auto stack = std::dynamic_pointer_cast<FallbackLlmClient>(client);
    QVERIFY(stack != nullptr);
    auto meter = std::dynamic_pointer_cast<MeterlessClient>(stack->primary());
    QVERIFY(meter != nullptr);
    QCOMPARE(meter->baseUrl(), QStringLiteral("http://127.0.0.1:1"));
    auto local = std::dynamic_pointer_cast<HttpLlmClient>(stack->fallback());
    QVERIFY(local != nullptr);
  }

  // Part A item 5 (local provider keys): a configured MODEL_API_KEY / SF_LLM_API_KEY is sent as Bearer.
  void localProviderApiKey() {
    ScopedEnv env;
    FakeLlmServer server;
    QVERIFY(server.listen());
    server.start();
    HttpLlmClient local(server.chatUrl(), QStringLiteral("qwythos-9b-v2"));
    local.setApiKey(QStringLiteral("local-key"));
    QJsonObject resp = local.complete(QStringLiteral("s"), oneUser(QStringLiteral("q")));
    QVERIFY(!resp.contains(QStringLiteral("error")));
    QCOMPARE(server.header("authorization"), QByteArrayLiteral("Bearer local-key"));
  }

  // Part A item 8: the official runtime checks only run when a real runtime token is configured;
  // otherwise the item is SKIPPED (and the report says so). Never prints the token.
  void officialNightshiftOnlyWhenTokenPresent() {
    const QString explicitToken = qEnvironmentVariable("METERLESS_RUNTIME_TOKEN").trimmed();
    const QString base = qEnvironmentVariable("METERLESS_BASE_URL").trimmed();
    if (explicitToken.isEmpty() || base.isEmpty()) {
      QSKIP("METERLESS_RUNTIME_TOKEN / METERLESS_BASE_URL not configured: official runtime checks skipped (item 8)");
    }
    MeterlessClient real;
    QJsonObject status = real.statusJson();
    QVERIFY(status.value(QStringLiteral("reachable")).toBool());
    QVERIFY(!status.value(QStringLiteral("models")).toArray().isEmpty());
    QJsonObject resp = real.complete(QStringLiteral("You are a film editor."), oneUser(QStringLiteral("Reply with the word ready.")));
    QVERIFY(!meterlessErrorCode(resp).isEmpty() || !resp.value(QStringLiteral("content")).toString().isEmpty());
  }

  void testVramNormalRouting() {
    auto localMock = std::make_shared<MockLlmClient>();
    localMock->setDefaultResponse(QJsonObject{{QStringLiteral("content"), QStringLiteral("local response")}});

    auto offloadMock = std::make_shared<MeterlessClient>();
    offloadMock->setMockHandler([](const QString&, const std::vector<ChatMessage>&, const QJsonArray&) {
      return QJsonObject{{QStringLiteral("content"), QStringLiteral("offload response")}};
    });

    VramAwareLlmClient vramClient(localMock, offloadMock);
    QVERIFY(!vramClient.isVramStrained());

    std::vector<ChatMessage> messages = {
      ChatMessage{QStringLiteral("user"), QStringLiteral("Hello")}
    };

    QJsonObject resp = vramClient.complete(QStringLiteral("Sys"), messages);
    QCOMPARE(resp.value(QStringLiteral("content")).toString(), QStringLiteral("local response"));
    QCOMPARE(vramClient.localCallCount(), 1);
    QCOMPARE(vramClient.offloadCallCount(), 0);
    QCOMPARE(vramClient.fallbackCount(), 0);
  }

  void testVramStrainedRouting() {
    auto localMock = std::make_shared<MockLlmClient>();
    localMock->setDefaultResponse(QJsonObject{{QStringLiteral("content"), QStringLiteral("local response")}});

    auto offloadMock = std::make_shared<MeterlessClient>();
    offloadMock->setMockHandler([](const QString&, const std::vector<ChatMessage>&, const QJsonArray&) {
      return QJsonObject{{QStringLiteral("content"), QStringLiteral("offload response")}};
    });

    VramAwareLlmClient vramClient(localMock, offloadMock);
    vramClient.setVramStrained(true);
    QVERIFY(vramClient.isVramStrained());

    std::vector<ChatMessage> messages = {
      ChatMessage{QStringLiteral("user"), QStringLiteral("High load edit task")}
    };

    QJsonObject resp = vramClient.complete(QStringLiteral("Sys"), messages);
    QCOMPARE(resp.value(QStringLiteral("content")).toString(), QStringLiteral("offload response"));
    QCOMPARE(vramClient.localCallCount(), 0);
    QCOMPARE(vramClient.offloadCallCount(), 1);
    QCOMPARE(vramClient.fallbackCount(), 0);
  }

  void testDynamicFallbackOnLocalFailure() {
    auto localMock = std::make_shared<MockLlmClient>();
    // Simulate CUDA OOM failure on local engine
    localMock->setDefaultResponse(QJsonObject{{QStringLiteral("error"), QStringLiteral("CUDA out of memory: VRAM allocation failed")}});

    auto offloadMock = std::make_shared<MeterlessClient>();
    offloadMock->setMockHandler([](const QString&, const std::vector<ChatMessage>&, const QJsonArray&) {
      return QJsonObject{{QStringLiteral("content"), QStringLiteral("recovered via meterless")}};
    });

    VramAwareLlmClient vramClient(localMock, offloadMock);
    vramClient.setVramStrained(false);

    std::vector<ChatMessage> messages = {
      ChatMessage{QStringLiteral("user"), QStringLiteral("Generate edit plan")}
    };

    QJsonObject resp = vramClient.complete(QStringLiteral("Sys"), messages);
    // Should gracefully fallback and return the recovered response
    QCOMPARE(resp.value(QStringLiteral("content")).toString(), QStringLiteral("recovered via meterless"));
    QCOMPARE(vramClient.localCallCount(), 1);
    QCOMPARE(vramClient.offloadCallCount(), 1);
    QCOMPARE(vramClient.fallbackCount(), 1);
  }

  void testResilienceWithNullClients() {
    // Both null
    VramAwareLlmClient emptyClient(nullptr, nullptr);
    QJsonObject resp1 = emptyClient.complete({}, {});
    QVERIFY(resp1.contains(QStringLiteral("error")));

    // Only offload present
    auto offloadMock = std::make_shared<MeterlessClient>();
    offloadMock->setMockHandler([](const QString&, const std::vector<ChatMessage>&, const QJsonArray&) {
      return QJsonObject{{QStringLiteral("content"), QStringLiteral("solo offload")}};
    });
    VramAwareLlmClient offloadOnly(nullptr, offloadMock);
    QJsonObject resp2 = offloadOnly.complete({}, {});
    QCOMPARE(resp2.value(QStringLiteral("content")).toString(), QStringLiteral("solo offload"));

    // Only local present
    auto localMock = std::make_shared<MockLlmClient>();
    localMock->setDefaultResponse(QJsonObject{{QStringLiteral("content"), QStringLiteral("solo local")}});
    VramAwareLlmClient localOnly(localMock, nullptr);
    QJsonObject resp3 = localOnly.complete({}, {});
    QCOMPARE(resp3.value(QStringLiteral("content")).toString(), QStringLiteral("solo local"));
  }

  void testRapidAlternatingLoadStress() {
    auto localMock = std::make_shared<MockLlmClient>();
    localMock->setDefaultResponse(QJsonObject{{QStringLiteral("content"), QStringLiteral("L")}});

    auto offloadMock = std::make_shared<MeterlessClient>();
    offloadMock->setMockHandler([](const QString&, const std::vector<ChatMessage>&, const QJsonArray&) {
      return QJsonObject{{QStringLiteral("content"), QStringLiteral("O")}};
    });

    VramAwareLlmClient vramClient(localMock, offloadMock);

    for (int i = 0; i < 100; ++i) {
      vramClient.setVramStrained(i % 2 == 1);
      QJsonObject r = vramClient.complete(QStringLiteral("P"), {});
      if (i % 2 == 1) {
        QCOMPARE(r.value(QStringLiteral("content")).toString(), QStringLiteral("O"));
      } else {
        QCOMPARE(r.value(QStringLiteral("content")).toString(), QStringLiteral("L"));
      }
    }

    QCOMPARE(vramClient.localCallCount(), 50);
    QCOMPARE(vramClient.offloadCallCount(), 50);
    QCOMPARE(vramClient.fallbackCount(), 0);
  }
};

QTEST_MAIN(TestMeterless)
#include "tst_meterless.moc"
