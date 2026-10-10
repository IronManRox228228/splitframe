// The local HTTP API: auth, loopback / Host / Origin checks, JSON-RPC errors, SSE events, concurrent clients.

#include "engine/api_server.h"
#include "engine/engine.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

using namespace sf::engine;

namespace {
QString Q(const char* s) { return QString::fromLatin1(s); }

struct Response {
  int status = 0;
  QMap<QByteArray, QByteArray> headers;
  QByteArray body;
  bool closed = false;
  QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
};

// Parses one complete response from `buf` (removing it); false when more bytes are needed.
bool takeResponse(QByteArray& buf, Response* r) {
  const int end = static_cast<int>(buf.indexOf("\r\n\r\n"));
  if (end < 0) return false;
  const QList<QByteArray> lines = buf.left(end).split('\n');
  r->status = lines.value(0).split(' ').value(1).toInt();
  for (int i = 1; i < lines.size(); ++i) {
    const int c = static_cast<int>(lines[i].indexOf(':'));
    r->headers.insert(lines[i].left(c).trimmed().toLower(), lines[i].mid(c + 1).trimmed());
  }
  const int len = r->headers.value("content-length", "0").toInt();
  if (buf.size() < end + 4 + len) return false;
  r->body = buf.mid(end + 4, len);
  buf.remove(0, end + 4 + len);
  return true;
}
} // namespace

class TstApiHttp : public QObject {
  Q_OBJECT

  QTemporaryDir dir_;
  std::unique_ptr<Engine> engine_;
  std::unique_ptr<ApiServer> server_;

  QByteArray request(const QByteArray& method, const QByteArray& path, const QByteArray& body, QList<QPair<QByteArray, QByteArray>> headers = {}, bool withAuth = true,
                     bool withHost = true) {
    QByteArray r = method + ' ' + path + " HTTP/1.1\r\n";
    if (withHost) r += "Host: 127.0.0.1:" + QByteArray::number(server_->port()) + "\r\n";
    if (withAuth) r += "Authorization: Bearer " + server_->token().toLatin1() + "\r\n";
    for (const auto& h : headers) r += h.first + ": " + h.second + "\r\n";
    if (!body.isNull() || method == "POST") r += "Content-Length: " + QByteArray::number(body.size()) + "\r\nContent-Type: application/json\r\n";
    return r + "\r\n" + body;
  }
  static QByteArray rpc(const QString& method, const QJsonObject& params = {}, int id = 1) {
    return QJsonDocument(QJsonObject{{Q("jsonrpc"), Q("2.0")}, {Q("method"), method}, {Q("params"), params}, {Q("id"), id}}).toJson(QJsonDocument::Compact);
  }
  // Sends `raw` on a fresh connection and waits for one response (spinning the event loop the server lives in).
  Response roundTrip(const QByteArray& raw, int timeoutMs = 20000) {
    QTcpSocket s;
    QByteArray buf;
    Response r;
    bool done = false;
    connect(&s, &QTcpSocket::readyRead, &s, [&] {
      buf += s.readAll();
      if (!done && takeResponse(buf, &r)) done = true;
    });
    s.connectToHost(QHostAddress::LocalHost, server_->port());
    if (!s.waitForConnected(5000)) return r;
    s.write(raw);
    QElapsedTimer t;
    t.start();
    while (!done && t.elapsed() < timeoutMs && s.state() != QAbstractSocket::UnconnectedState) QTest::qWait(5);
    if (!done && !buf.isEmpty()) done = takeResponse(buf, &r);
    r.closed = s.state() == QAbstractSocket::UnconnectedState;
    return r;
  }
  Response call(const QString& method, const QJsonObject& params = {}) { return roundTrip(request("POST", "/rpc", rpc(method, params))); }

private slots:
  void initTestCase() { QVERIFY(dir_.isValid()); }
  void init() {
    engine_ = std::make_unique<Engine>();
    engine_->call(Q("project.new"), {{Q("name"), Q("Api")}, {Q("width"), 320}, {Q("height"), 180}, {Q("fps"), 25}});
    ApiServer::Options o;
    o.tokenFile = dir_.filePath(Q("api-token"));
    o.maxBody = 4096;
    server_ = std::make_unique<ApiServer>(*engine_, o);
    QString err;
    QVERIFY2(server_->start(&err), qPrintable(err));
  }
  void cleanup() {
    server_.reset();
    engine_.reset();
  }

  void tokenFileAndLoopbackBinding() {
    QFile f(dir_.filePath(Q("api-token")));
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromLatin1(f.readAll()), server_->token());
    QCOMPARE(server_->token().size(), 64);
    QVERIFY(server_->port() != 0);
    // the default token location is the per-user SplitFrame config folder, never the Electron app's
    const QString def = ApiServer::defaultTokenFile();
    QVERIFY(!def.contains(Q("Cutboard"), Qt::CaseInsensitive));
    QVERIFY(QFileInfo::exists(dir_.filePath(Q("api.json")))); // endpoint file
    // a second server on the same port can't bind: it is a real listener on 127.0.0.1 only
    QTcpSocket probe;
    probe.connectToHost(QHostAddress::LocalHost, server_->port());
    QVERIFY(probe.waitForConnected(3000));
    // tokens differ per start
    ApiServer other(*engine_, {.tokenFile = dir_.filePath(Q("other-token"))});
    QVERIFY(other.start());
    QVERIFY(other.token() != server_->token());
  }

  void authIsRequired() {
    Response r = roundTrip(request("POST", "/rpc", rpc(Q("engine.ping")), {}, false));
    QCOMPARE(r.status, 401);
    QVERIFY(r.headers.contains("www-authenticate"));
    QVERIFY(!r.body.contains(server_->token().toLatin1()));
    r = roundTrip(request("POST", "/rpc", rpc(Q("engine.ping")), {{"Authorization", "Bearer 0000"}}, false));
    QCOMPARE(r.status, 401);
    r = roundTrip(request("POST", "/rpc", rpc(Q("engine.ping")), {{"Authorization", "Basic abc"}}, false));
    QCOMPARE(r.status, 401);
    r = roundTrip(request("GET", "/events", {}, {}, false));
    QCOMPARE(r.status, 401);
    r = call(Q("engine.ping"));
    QCOMPARE(r.status, 200);
    QCOMPARE(r.json().value(Q("result")).toObject().value(Q("engine")).toString(), Q("splitframe"));
    QCOMPARE(r.headers.value("content-type"), QByteArray("application/json"));
    QVERIFY(!r.headers.contains("access-control-allow-origin")); // never any CORS
  }

  void hostAndOriginAreChecked() {
    QCOMPARE(roundTrip(request("POST", "/rpc", rpc(Q("engine.ping")), {{"Host", "evil.example.com"}}, true, false)).status, 403); // DNS rebinding
    QCOMPARE(roundTrip(request("POST", "/rpc", rpc(Q("engine.ping")), {{"Host", "127.0.0.1:1"}}, true, false)).status, 403);
    QCOMPARE(roundTrip(request("POST", "/rpc", rpc(Q("engine.ping")), {{"Origin", "http://evil.example.com"}})).status, 403);
    QCOMPARE(roundTrip(request("POST", "/rpc", rpc(Q("engine.ping")), {{"Origin", "null"}})).status, 403);
    QCOMPARE(roundTrip(request("POST", "/rpc", rpc(Q("engine.ping")), {{"Origin", "http://127.0.0.1:9"}})).status, 403);
    // own origin is fine, and so is a localhost Host
    QCOMPARE(roundTrip(request("POST", "/rpc", rpc(Q("engine.ping")), {{"Origin", "http://127.0.0.1:" + QByteArray::number(server_->port())}})).status, 200);
    QCOMPARE(roundTrip(request("POST", "/rpc", rpc(Q("engine.ping")), {{"Host", "localhost:" + QByteArray::number(server_->port())}}, true, false)).status, 200);
    // origin check also gates the event stream
    QCOMPARE(roundTrip(request("GET", "/events", {}, {{"Origin", "http://evil.example.com"}})).status, 403);
  }

  void httpHygiene() {
    QCOMPARE(roundTrip(request("GET", "/rpc", {})).status, 405);
    QCOMPARE(roundTrip(request("POST", "/nope", "{}")).status, 404);
    QCOMPARE(roundTrip(request("POST", "/rpc", rpc(Q("engine.ping")), {{"Transfer-Encoding", "chunked"}})).status, 501);
    QCOMPARE(roundTrip(request("POST", "/rpc", QByteArray(5000, 'x'))).status, 413);
    QCOMPARE(roundTrip("garbage\r\n\r\n").status, 400);
    const Response r = roundTrip(request("POST", "/rpc", rpc(Q("engine.ping")), {{"Connection", "close"}}));
    QCOMPARE(r.status, 200);
    QVERIFY(r.closed);
  }

  void keepAliveServesSeveralRequests() {
    QTcpSocket s;
    QByteArray buf;
    s.connectToHost(QHostAddress::LocalHost, server_->port());
    QVERIFY(s.waitForConnected(3000));
    for (int i = 1; i <= 3; ++i) {
      s.write(request("POST", "/rpc", rpc(Q("engine.ping"), {}, i)));
      Response r;
      QElapsedTimer t;
      t.start();
      bool got = false;
      while (!got && t.elapsed() < 10000) {
        QTest::qWait(5);
        buf += s.readAll();
        got = takeResponse(buf, &r);
      }
      QVERIFY(got);
      QCOMPARE(r.json().value(Q("id")).toInt(), i);
    }
  }

  void jsonRpcErrors() {
    // parse error
    Response r = roundTrip(request("POST", "/rpc", "{not json"));
    QCOMPARE(r.status, 200);
    QCOMPARE(r.json().value(Q("error")).toObject().value(Q("code")).toInt(), -32700);
    // invalid request
    r = roundTrip(request("POST", "/rpc", R"({"jsonrpc":"1.0","method":"engine.ping","id":1})"));
    QCOMPARE(r.json().value(Q("error")).toObject().value(Q("code")).toInt(), -32600);
    r = roundTrip(request("POST", "/rpc", R"({"jsonrpc":"2.0","id":1})"));
    QCOMPARE(r.json().value(Q("error")).toObject().value(Q("code")).toInt(), -32600);
    // method not found
    r = call(Q("no.such.method"));
    QCOMPARE(r.json().value(Q("error")).toObject().value(Q("code")).toInt(), -32601);
    QCOMPARE(r.json().value(Q("id")).toInt(), 1);
    // invalid params: schema violation, and positional params
    r = call(Q("clip.add"), {{Q("assetId"), 5}});
    QCOMPARE(r.json().value(Q("error")).toObject().value(Q("code")).toInt(), -32602);
    r = roundTrip(request("POST", "/rpc", R"({"jsonrpc":"2.0","method":"engine.ping","params":[1],"id":3})"));
    QCOMPARE(r.json().value(Q("error")).toObject().value(Q("code")).toInt(), -32602);
    // an engine error carries its own code in data
    r = call(Q("item.get"), {{Q("itemId"), Q("itm_nope")}});
    const QJsonObject err = r.json().value(Q("error")).toObject();
    QCOMPARE(err.value(Q("code")).toInt(), -32000);
    QCOMPARE(err.value(Q("data")).toObject().value(Q("code")).toString(), Q("not_found"));
    // notification: no response body
    r = roundTrip(request("POST", "/rpc", R"({"jsonrpc":"2.0","method":"engine.ping"})"));
    QCOMPARE(r.status, 204);
    QVERIFY(r.body.isEmpty());
    // batch: answers for the requests only
    r = roundTrip(request("POST", "/rpc", R"([{"jsonrpc":"2.0","method":"engine.ping","id":"a"},{"jsonrpc":"2.0","method":"engine.ping"},{"jsonrpc":"2.0","method":"nope","id":"b"}])"));
    const QJsonArray batch = QJsonDocument::fromJson(r.body).array();
    QCOMPARE(batch.size(), 2);
    QCOMPARE(batch.at(0).toObject().value(Q("id")).toString(), Q("a"));
    QVERIFY(batch.at(1).toObject().contains(Q("error")));
    // string ids round-trip
    r = roundTrip(request("POST", "/rpc", R"({"jsonrpc":"2.0","method":"engine.ping","id":"req-7"})"));
    QCOMPARE(r.json().value(Q("id")).toString(), Q("req-7"));
  }

  void editOverRpcMatchesEngine() {
    Response r = call(Q("marker.add"), {{Q("frame"), 7}, {Q("label"), Q("via http")}, {Q("origin"), Q("agent:test")}});
    QVERIFY(r.json().contains(Q("result")));
    r = call(Q("timeline.get"));
    QCOMPARE(r.json().value(Q("result")).toObject().value(Q("markers")).toArray().size(), 1);
    QCOMPARE(engine_->session()->project->doc().markers.size(), size_t(1));
  }

  void sseDeliversEvents() {
    QTcpSocket sse;
    QByteArray buf;
    sse.connectToHost(QHostAddress::LocalHost, server_->port());
    QVERIFY(sse.waitForConnected(3000));
    sse.write(request("GET", "/events?events=ops.applied", {}));
    auto waitFor = [&](const QByteArray& needle, int ms = 10000) {
      QElapsedTimer t;
      t.start();
      while (!buf.contains(needle) && t.elapsed() < ms) {
        QTest::qWait(5);
        buf += sse.readAll();
      }
      return buf.contains(needle);
    };
    QVERIFY2(waitFor(": connected"), buf.constData());
    QVERIFY(buf.startsWith("HTTP/1.1 200"));
    QVERIFY(buf.contains("text/event-stream"));
    // a filtered stream: doc.changed is not delivered, ops.applied is
    const Response r = call(Q("ops.apply"), {{Q("ops"), QJsonArray{QJsonObject{{Q("type"), Q("project.rename")}, {Q("name"), Q("Renamed")}}}}, {Q("origin"), Q("agent:sse")}, {Q("label"), Q("rename")}});
    QVERIFY(r.json().contains(Q("result")));
    QVERIFY2(waitFor("event: ops.applied\n"), buf.constData());
    QVERIFY(waitFor("\n\n"));
    QVERIFY(!buf.contains("event: doc.changed"));
    const int dataAt = static_cast<int>(buf.indexOf("data: "));
    const QJsonObject data = QJsonDocument::fromJson(buf.mid(dataAt + 6, buf.indexOf('\n', dataAt) - dataAt - 6)).object();
    QCOMPARE(data.value(Q("actor")).toString(), Q("agent:sse"));
    QCOMPARE(data.value(Q("label")).toString(), Q("rename"));
    QCOMPARE(data.value(Q("count")).toInt(), 1);
    QVERIFY(buf.contains("id: "));
    // an unfiltered stream also sees undo as doc.changed
    QTcpSocket all;
    QByteArray buf2;
    all.connectToHost(QHostAddress::LocalHost, server_->port());
    QVERIFY(all.waitForConnected(3000));
    all.write(request("GET", "/events", {}));
    QElapsedTimer t;
    t.start();
    while (!buf2.contains(": connected") && t.elapsed() < 10000) {
      QTest::qWait(5);
      buf2 += all.readAll();
    }
    call(Q("history.undo"));
    t.restart();
    while (!buf2.contains("event: doc.changed") && t.elapsed() < 10000) {
      QTest::qWait(5);
      buf2 += all.readAll();
    }
    QVERIFY2(buf2.contains("event: doc.changed"), buf2.constData());
  }

  void concurrentClients() {
    constexpr int kClients = 12;
    struct Client {
      std::unique_ptr<QTcpSocket> sock = std::make_unique<QTcpSocket>();
      QByteArray buf;
      Response resp;
      bool done = false;
    };
    std::vector<Client> clients(kClients);
    for (int i = 0; i < kClients; ++i) {
      Client& c = clients[static_cast<size_t>(i)];
      c.sock->connectToHost(QHostAddress::LocalHost, server_->port());
      QVERIFY(c.sock->waitForConnected(3000));
    }
    // all requests are in flight before any response is read
    for (int i = 0; i < kClients; ++i) {
      const QString cmd = i % 2 ? Q("project.info") : Q("marker.add");
      const QJsonObject args = i % 2 ? QJsonObject{} : QJsonObject{{Q("frame"), i}, {Q("label"), QStringLiteral("c%1").arg(i)}};
      clients[static_cast<size_t>(i)].sock->write(request("POST", "/rpc", rpc(cmd, args, 100 + i)));
    }
    QElapsedTimer t;
    t.start();
    int pending = kClients;
    while (pending > 0 && t.elapsed() < 20000) {
      QTest::qWait(5);
      for (Client& c : clients) {
        if (c.done) continue;
        c.buf += c.sock->readAll();
        if (takeResponse(c.buf, &c.resp)) {
          c.done = true;
          --pending;
        }
      }
    }
    QCOMPARE(pending, 0);
    for (int i = 0; i < kClients; ++i) {
      const Response& r = clients[static_cast<size_t>(i)].resp;
      QCOMPARE(r.status, 200);
      QCOMPARE(r.json().value(Q("id")).toInt(), 100 + i);
      QVERIFY(r.json().contains(Q("result")));
    }
    QCOMPARE(engine_->session()->project->doc().markers.size(), size_t(kClients / 2));
  }

  void stopClosesEverything() {
    const quint16 port = server_->port();
    server_->stop();
    QVERIFY(!server_->running());
    QVERIFY(!QFileInfo::exists(dir_.filePath(Q("api.json"))));
    QTcpSocket s;
    s.connectToHost(QHostAddress::LocalHost, port);
    QVERIFY(!s.waitForConnected(500));
  }
};

QTEST_MAIN(TstApiHttp)
#include "tst_api_http.moc"
