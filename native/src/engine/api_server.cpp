#include "engine/api_server.h"

#include <QCoreApplication>
#include <algorithm>
#include <cstring>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

namespace sf::engine {

namespace {

constexpr int kMaxHeaderBytes = 16 * 1024;

bool constantTimeEqual(const QByteArray& a, const QByteArray& b) {
  unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
  const int n = static_cast<int>(std::max(a.size(), b.size()));
  for (int i = 0; i < n; ++i) diff |= static_cast<unsigned char>(i < a.size() ? a[i] : 0) ^ static_cast<unsigned char>(i < b.size() ? b[i] : 0);
  return diff == 0;
}

QByteArray statusText(int status) {
  switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return "Error";
  }
}

} // namespace

class ApiServer::Connection : public QObject {
public:
  Connection(ApiServer* server, QTcpSocket* socket) : QObject(server), srv_(server), sock_(socket) {
    socket->setParent(this);
    idle_.setSingleShot(true);
    idle_.setInterval(server->options_.idleMs);
    QObject::connect(&idle_, &QTimer::timeout, this, [this] { sock_->abort(); });
    QObject::connect(socket, &QTcpSocket::readyRead, this, [this] {
      buf_ += sock_->readAll();
      if (!sse_) idle_.start();
      process();
    });
    QObject::connect(socket, &QTcpSocket::disconnected, this, [this] { deleteLater(); });
    idle_.start();
  }
  ~Connection() override { srv_->connections_.removeAll(this); }

  bool isSse() const { return sse_; }
  bool wants(const QString& event) const { return filter_.isEmpty() || filter_.contains(event); }
  void write(const QByteArray& bytes) {
    if (sock_->state() == QAbstractSocket::ConnectedState) sock_->write(bytes);
  }
  void reject(int status, const QString& message, bool close = true, const QByteArray& extra = {}) {
    reply(status, "application/json", QJsonDocument(QJsonObject{{QStringLiteral("error"), message}}).toJson(QJsonDocument::Compact), !close, extra);
    if (close) closing_ = true;
  }

private:
  void reply(int status, const QByteArray& type, const QByteArray& body, bool keepAlive, const QByteArray& extra = {}) {
    QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + ' ' + statusText(status) + "\r\n";
    if (status != 204) head += "Content-Type: " + type + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n";
    else head += "Content-Length: 0\r\n";
    head += "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n";
    head += extra;
    head += keepAlive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
    write(head + (status == 204 ? QByteArray() : body));
    if (!keepAlive) sock_->disconnectFromHost();
  }

  bool hostAllowed(const QByteArray& host) const {
    QString h = QString::fromLatin1(host).trimmed().toLower();
    QString name = h;
    QString port;
    if (h.startsWith(QLatin1Char('['))) {
      const int e = h.indexOf(QLatin1Char(']'));
      if (e < 0) return false;
      name = h.left(e + 1);
      port = h.mid(e + 1);
      if (port.startsWith(QLatin1Char(':'))) port = port.mid(1);
    } else if (const int c = h.lastIndexOf(QLatin1Char(':')); c >= 0) {
      name = h.left(c);
      port = h.mid(c + 1);
    }
    if (name != QLatin1String("127.0.0.1") && name != QLatin1String("localhost") && name != QLatin1String("[::1]")) return false;
    return port.isEmpty() ? srv_->port() == 80 : port == QString::number(srv_->port());
  }
  bool originAllowed(const QByteArray& origin) const {
    const QString o = QString::fromLatin1(origin).trimmed().toLower();
    const QString p = QString::number(srv_->port());
    return o == QStringLiteral("http://127.0.0.1:") + p || o == QStringLiteral("http://localhost:") + p;
  }

  void process() {
    QPointer<Connection> self(this);
    while (self && !closing_ && !sse_) {
      const int end = static_cast<int>(buf_.indexOf("\r\n\r\n"));
      if (end < 0) {
        if (buf_.size() > kMaxHeaderBytes) reject(431, QStringLiteral("Headers too large."));
        return;
      }
      if (end > kMaxHeaderBytes) return reject(431, QStringLiteral("Headers too large."));
      const QList<QByteArray> lines = buf_.left(end).split('\n');
      const QList<QByteArray> reqLine = lines.value(0).trimmed().split(' ');
      if (reqLine.size() != 3 || !reqLine[2].startsWith("HTTP/1.")) return reject(400, QStringLiteral("Malformed request line."));
      QHash<QByteArray, QByteArray> headers;
      for (int i = 1; i < lines.size(); ++i) {
        const int c = static_cast<int>(lines[i].indexOf(':'));
        if (c <= 0) return reject(400, QStringLiteral("Malformed header."));
        headers.insert(lines[i].left(c).trimmed().toLower(), lines[i].mid(c + 1).trimmed());
      }
      const QByteArray method = reqLine[0], target = reqLine[1];
      // validate before reading any body
      if (!hostAllowed(headers.value("host"))) return reject(403, QStringLiteral("Host not allowed."));
      if (headers.contains("origin") && !originAllowed(headers.value("origin"))) return reject(403, QStringLiteral("Foreign Origin rejected."));
      const QByteArray auth = headers.value("authorization");
      if (!auth.startsWith("Bearer ") || !constantTimeEqual(auth.mid(7).trimmed(), srv_->token_.toLatin1()))
        return reject(401, QStringLiteral("A valid bearer token is required."), true, "WWW-Authenticate: Bearer\r\n");
      if (headers.contains("transfer-encoding")) return reject(501, QStringLiteral("Chunked bodies are not supported; send Content-Length."));
      bool lenOk = true;
      const qint64 len = headers.contains("content-length") ? headers.value("content-length").toLongLong(&lenOk) : 0;
      if (!lenOk || len < 0) return reject(400, QStringLiteral("Bad Content-Length."));
      if (len > srv_->options_.maxBody) return reject(413, QStringLiteral("Body larger than %1 bytes.").arg(srv_->options_.maxBody));
      const QUrl url = QUrl::fromEncoded(target);
      const bool keepAlive = headers.value("connection").toLower() != "close" && reqLine[2] != "HTTP/1.0";

      if (url.path() == QLatin1String("/rpc")) {
        if (method != "POST") return reject(405, QStringLiteral("Use POST /rpc."), true, "Allow: POST\r\n");
        if (!headers.contains("content-length")) return reject(411, QStringLiteral("Content-Length required."));
        if (buf_.size() < end + 4 + len) return; // body still arriving
        const QByteArray body = buf_.mid(end + 4, static_cast<int>(len));
        buf_.remove(0, end + 4 + static_cast<int>(len));
        const QByteArray out = srv_->rpc_.handleText(body);
        if (!self) return;
        if (out.isEmpty()) reply(204, {}, {}, keepAlive);
        else reply(200, "application/json", out, keepAlive);
        if (!keepAlive) closing_ = true;
      } else if (url.path() == QLatin1String("/events")) {
        if (method != "GET") return reject(405, QStringLiteral("Use GET /events."), true, "Allow: GET\r\n");
        buf_.remove(0, end + 4 + static_cast<int>(len));
        for (const QString& e : QUrlQuery(url).queryItemValue(QStringLiteral("events")).split(QLatin1Char(','), Qt::SkipEmptyParts)) filter_.insert(e);
        write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\nConnection: keep-alive\r\nX-Accel-Buffering: no\r\n\r\n: connected\n\n");
        sse_ = true;
        idle_.stop();
      } else {
        buf_.remove(0, end + 4 + static_cast<int>(len));
        reject(404, QStringLiteral("Unknown path. Use POST /rpc or GET /events."), false);
      }
    }
  }

  ApiServer* srv_;
  QTcpSocket* sock_;
  QByteArray buf_;
  QTimer idle_;
  bool sse_ = false;
  bool closing_ = false;
  QSet<QString> filter_;
};

ApiServer::ApiServer(Engine& engine, Options options, QObject* parent) : QObject(parent), engine_(engine), options_(std::move(options)), server_(this), rpc_(engine) {
  connect(&server_, &QTcpServer::newConnection, this, &ApiServer::onNewConnection);
  connect(&engine_, &Engine::event, this, &ApiServer::broadcast);
  heartbeat_.setInterval(15000);
  connect(&heartbeat_, &QTimer::timeout, this, [this] {
    for (Connection* c : std::as_const(connections_))
      if (c->isSse()) c->write(": ping\n\n");
  });
}

ApiServer::~ApiServer() { stop(); }

QString ApiServer::defaultTokenFile() {
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
  return QDir(dir).filePath(QStringLiteral("api-token"));
}

QString ApiServer::generateToken() {
  QByteArray raw(32, 0);
  for (int i = 0; i < raw.size(); i += 4) {
    const quint32 v = QRandomGenerator::system()->generate();
    memcpy(raw.data() + i, &v, 4);
  }
  return QString::fromLatin1(raw.toHex());
}

bool ApiServer::start(QString* error) {
  if (running()) return true;
  auto fail = [&](const QString& m) {
    if (error) *error = m;
    return false;
  };
  token_ = options_.token.isEmpty() ? generateToken() : options_.token;
  if (options_.tokenFile.isEmpty()) options_.tokenFile = defaultTokenFile();
  if (options_.endpointFile.isEmpty()) options_.endpointFile = QDir(QFileInfo(options_.tokenFile).absolutePath()).filePath(QStringLiteral("api.json"));
  if (!QDir().mkpath(QFileInfo(options_.tokenFile).absolutePath())) return fail(QStringLiteral("Can't create %1").arg(QFileInfo(options_.tokenFile).absolutePath()));
  {
    QSaveFile f(options_.tokenFile);
    if (!f.open(QIODevice::WriteOnly)) return fail(QStringLiteral("Can't write the token file %1: %2").arg(options_.tokenFile, f.errorString()));
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(token_.toLatin1());
    if (!f.commit()) return fail(QStringLiteral("Can't write the token file %1").arg(options_.tokenFile));
    QFile::setPermissions(options_.tokenFile, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  }
  if (!server_.listen(QHostAddress::LocalHost, options_.port)) return fail(QStringLiteral("Can't listen on 127.0.0.1:%1: %2").arg(options_.port).arg(server_.errorString()));
  QSaveFile ep(options_.endpointFile);
  if (ep.open(QIODevice::WriteOnly)) {
    ep.write(QJsonDocument(QJsonObject{{QStringLiteral("host"), QStringLiteral("127.0.0.1")}, {QStringLiteral("port"), port()}, {QStringLiteral("pid"), static_cast<double>(QCoreApplication::applicationPid())},
                                       {QStringLiteral("tokenFile"), QDir::toNativeSeparators(options_.tokenFile)}})
                 .toJson());
    ep.commit();
  }
  heartbeat_.start();
  return true;
}

void ApiServer::stop() {
  heartbeat_.stop();
  server_.close();
  const QList<Connection*> conns = connections_;
  connections_.clear();
  for (Connection* c : conns) delete c;
  if (!options_.endpointFile.isEmpty()) QFile::remove(options_.endpointFile);
}

void ApiServer::onNewConnection() {
  while (QTcpSocket* s = server_.nextPendingConnection()) {
    auto* c = new Connection(this, s);
    connections_.append(c);
    if (!s->peerAddress().isLoopback()) {
      c->reject(403, QStringLiteral("Loopback clients only."));
    } else if (connections_.size() > options_.maxConnections) {
      c->reject(503, QStringLiteral("Too many connections."));
    }
  }
}

void ApiServer::broadcast(const QString& name, const QJsonObject& data, qint64 seq) {
  QByteArray frame;
  for (Connection* c : std::as_const(connections_)) {
    if (!c->isSse() || !c->wants(name)) continue;
    if (frame.isEmpty())
      frame = "id: " + QByteArray::number(seq) + "\nevent: " + name.toUtf8() + "\ndata: " + QJsonDocument(data).toJson(QJsonDocument::Compact) + "\n\n";
    c->write(frame);
  }
}

} // namespace sf::engine
