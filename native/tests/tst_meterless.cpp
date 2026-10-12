#include "agent/meterless_client.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

using namespace sf;
using namespace sf::agent;

class TestMeterless : public QObject {
  Q_OBJECT

private slots:
  void testClientDefaultsAndConfiguration() {
    MeterlessClient client;
    QCOMPARE(client.endpoint(), QStringLiteral("https://api.meterless.ai/v1/chat/completions"));
    QCOMPARE(client.model(), QStringLiteral("meterless-v1"));

    client.setApiKey(QStringLiteral("test-key-123"));
    client.setEndpoint(QStringLiteral("https://custom.meterless.ai/v1/chat"));
    client.setModel(QStringLiteral("deepseek-r1"));
    client.setTimeoutMs(5000);

    QCOMPARE(client.apiKey(), QStringLiteral("test-key-123"));
    QCOMPARE(client.endpoint(), QStringLiteral("https://custom.meterless.ai/v1/chat"));
    QCOMPARE(client.model(), QStringLiteral("deepseek-r1"));
    QCOMPARE(client.timeoutMs(), 5000);
  }

  void testMockCompletionAndTelemetry() {
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
