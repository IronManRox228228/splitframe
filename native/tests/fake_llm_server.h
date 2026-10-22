#pragma once

// OpenAI-compatible fake for tests: GET /v1/models and POST /v1/chat/completions
// with a queue of scripted replies (including tool_calls). Never talks to a real model.

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

#include <functional>
#include <vector>

namespace sf::test {

class FakeLlmServer : public QObject {
  Q_OBJECT
public:
  explicit FakeLlmServer(QObject* parent = nullptr) : QObject(parent) {}

  bool listen() { return server_.listen(QHostAddress::LocalHost); }
  quint16 port() const { return server_.serverPort(); }
  QString chatUrl() const { return QStringLiteral("http://127.0.0.1:%1/v1/chat/completions").arg(port()); }
  QString modelsUrl() const { return QStringLiteral("http://127.0.0.1:%1/v1/models").arg(port()); }

  void enqueueChat(const QJsonObject& message) { chatQueue_.push_back(message); }
  const QJsonObject& lastChatRequest() const { return lastChat_; }
  int chatHits() const { return chatHits_; }
  int modelsHits() const { return modelsHits_; }

  // Part A tests: respond to the NEXT chat call with an explicit HTTP status and body
  // (provider_auth = 401, rate_limited = 429, model_not_found = 404...).
  void setNextChatStatus(int status) { nextChatStatus_ = status; }
  void setNextChatBody(const QJsonObject& body) { nextChatBody_ = body; }
  // Raw request headers of the last chat call (lower-cased names), e.g. for the runtime contract.
  QByteArray header(const char* name) const { return lastHeaders_.value(QString::fromLatin1(name).toLower()); }

  void start() {
    QObject::connect(&server_, &QTcpServer::newConnection, this, [this] {
      while (QTcpSocket* s = server_.nextPendingConnection()) {
        QObject::connect(s, &QTcpSocket::readyRead, s, [this, s] { readRequest(s); });
      }
    });
  }

private:
  void readRequest(QTcpSocket* s) {
    buf_[s] += s->readAll();
    QByteArray& buf = buf_[s];
    const int headerEnd = buf.indexOf("\r\n\r\n");
    if (headerEnd < 0) return;
    const int lenAt = buf.toLower().indexOf("content-length:");
    int len = 0;
    if (lenAt >= 0) len = buf.mid(lenAt + 15, buf.indexOf("\r\n", lenAt) - lenAt - 15).trimmed().toInt();
    if (buf.size() - headerEnd - 4 < len) return;
    const QByteArray head = buf.left(headerEnd);
    const QByteArray body = buf.mid(headerEnd + 4, len);
    buf.clear();

    const bool isModels = head.startsWith("GET /v1/models") || head.contains(" GET /v1/models");
    // Capture the request's headers (lower-cased) for the runtime-contract asserts.
    lastHeaders_.clear();
    for (const QByteArray& line : head.split('\n')) {
      const int at = line.indexOf(':');
      if (at > 0) lastHeaders_.insert(QString::fromLatin1(line.left(at)).toLower().trimmed(), line.mid(at + 1).trimmed());
    }
    QJsonObject reply;
    if (isModels) {
      ++modelsHits_;
      reply = QJsonObject{{QStringLiteral("data"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("fake-model")}}}}};
    } else {
      ++chatHits_;
      lastChat_ = QJsonDocument::fromJson(body).object();
      QJsonObject message;
      if (!chatQueue_.empty()) {
        message = chatQueue_.front();
        chatQueue_.erase(chatQueue_.begin());
      } else {
        message = QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")}, {QStringLiteral("content"), QStringLiteral("ok")}};
      }
      reply = QJsonObject{{QStringLiteral("choices"), QJsonArray{QJsonObject{{QStringLiteral("message"), message}}}}};
    }
    int status = 200;
    if (!isModels && nextChatStatus_ != 0) {
      status = nextChatStatus_;
      nextChatStatus_ = 0; // one-shot
    }
    if (!isModels && !nextChatBody_.isEmpty()) {
      reply = nextChatBody_;
      nextChatBody_ = QJsonObject{}; // one-shot
    }
    const QByteArray payload = QJsonDocument(reply).toJson(QJsonDocument::Compact);
    s->write("HTTP/1.1 " + QByteArray::number(status) + " x\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(payload.size()) +
             "\r\nConnection: close\r\n\r\n" + payload);
    s->disconnectFromHost();
  }

  QTcpServer server_;
  std::map<QTcpSocket*, QByteArray> buf_;
  std::vector<QJsonObject> chatQueue_;
  QJsonObject lastChat_;
  int chatHits_ = 0;
  int modelsHits_ = 0;
  int nextChatStatus_ = 0;
  QJsonObject nextChatBody_;
  QHash<QString, QByteArray> lastHeaders_;
};

} // namespace sf::test
