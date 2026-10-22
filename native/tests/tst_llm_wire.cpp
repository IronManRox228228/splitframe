// The HTTP LLM client must speak the OpenAI-compatible wire format real servers (llama-server) accept:
// tools wrapped as {type:"function", function:{...}} with names free of dots, and tool-call names mapped
// back to the engine's dotted command names on the way in.
#include "agent/llm_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

using namespace sf::agent;

class TstLlmWire : public QObject {
  Q_OBJECT

private slots:
  void toolsAndNamesRoundTrip() {
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QByteArray requestBody;
    const QByteArray reply = QJsonDocument(QJsonObject{{QStringLiteral("choices"), QJsonArray{QJsonObject{{QStringLiteral("message"), QJsonObject{
        {QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("content"), QString()},
        {QStringLiteral("tool_calls"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("c1")},
                                                             {QStringLiteral("type"), QStringLiteral("function")},
                                                             {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), QStringLiteral("item__split")},
                                                                                                      {QStringLiteral("arguments"), QStringLiteral("{}")}}}}}}}}}}}})
                                 .toJson(QJsonDocument::Compact);
    connect(&server, &QTcpServer::newConnection, this, [&] {
      QTcpSocket* s = server.nextPendingConnection();
      connect(s, &QTcpSocket::readyRead, s, [&, s] {
        requestBody += s->readAll();
        const int headerEnd = requestBody.indexOf("\r\n\r\n");
        if (headerEnd < 0) return;
        const int lenAt = requestBody.toLower().indexOf("content-length:");
        const int len = requestBody.mid(lenAt + 15, requestBody.indexOf("\r\n", lenAt) - lenAt - 15).trimmed().toInt();
        if (requestBody.size() - headerEnd - 4 < len) return;
        requestBody = requestBody.mid(headerEnd + 4);
        s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(reply.size()) + "\r\nConnection: close\r\n\r\n" + reply);
        s->disconnectFromHost();
      });
    });

    HttpLlmClient client(QStringLiteral("http://127.0.0.1:%1/v1/chat/completions").arg(server.serverPort()), QStringLiteral("m"));
    const QJsonArray tools{QJsonObject{{QStringLiteral("name"), QStringLiteral("item.split")},
                                       {QStringLiteral("description"), QStringLiteral("Split an item")},
                                       {QStringLiteral("domain"), QStringLiteral("editor")},
                                       {QStringLiteral("parameters"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}}}};
    const ChatMessage earlier{.role = QStringLiteral("assistant"), .content = QString(),
                              .toolCalls = QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("c0")},
                                                                  {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), QStringLiteral("item.trim")},
                                                                                                           {QStringLiteral("arguments"), QStringLiteral("{}")}}}}}};
    const QJsonObject msg = client.complete(QStringLiteral("sys"), {earlier}, tools, -1);

    const QJsonObject req = QJsonDocument::fromJson(requestBody).object();
    const QJsonObject tool = req.value(QStringLiteral("tools")).toArray().at(0).toObject();
    QCOMPARE(tool.value(QStringLiteral("type")).toString(), QStringLiteral("function"));
    QCOMPARE(tool.value(QStringLiteral("function")).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("item__split"));
    QVERIFY(!tool.contains(QStringLiteral("domain")));
    const QJsonObject sent = req.value(QStringLiteral("messages")).toArray().at(1).toObject();
    QCOMPARE(sent.value(QStringLiteral("tool_calls")).toArray().at(0).toObject().value(QStringLiteral("function")).toObject().value(QStringLiteral("name")).toString(),
             QStringLiteral("item__trim"));
    QCOMPARE(msg.value(QStringLiteral("tool_calls")).toArray().at(0).toObject().value(QStringLiteral("function")).toObject().value(QStringLiteral("name")).toString(),
             QStringLiteral("item.split"));
  }

  // ---- provider-aware reachability probe (guardAgentTurn uses it before agent.turn) ----
  void probeEndpoint() {
    // empty URL and "mock" are always reachable (offline backends)
    QVERIFY(probeHttpEndpoint(QString(), 300));
    QVERIFY(probeHttpEndpoint(QStringLiteral("mock"), 300));

    // a real server's TCP port answers
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QVERIFY(probeHttpEndpoint(QStringLiteral("http://127.0.0.1:%1/v1/chat/completions").arg(server.serverPort()), 1000));

    // a closed port fails with a message naming the endpoint
    QString err;
    const bool reachable = probeHttpEndpoint(QStringLiteral("http://127.0.0.1:1/v1/chat/completions"), 500, &err);
    QVERIFY(!reachable);
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains(QStringLiteral("127.0.0.1")));
  }
};

QTEST_GUILESS_MAIN(TstLlmWire)
#include "tst_llm_wire.moc"
