#pragma once

// Fake ComfyUI server for unit and integration tests: serves minimal HTTP/1.1 on loopback.

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace sf::test {

class FakeComfyServer {

  struct State {
    std::mutex mu;
    std::vector<QJsonObject> queued;
    std::atomic<int> uploads{0};
    int historyCalls = 0;
    QString outputFilename;
    bool hangHistory = false;
    bool emptyQueue = false;
    bool interrupted = false;
    bool responding = true;
    quint16 port = 0;
  };

  std::shared_ptr<State> state_;
  QThread thread_;

  class ServerWorker : public QTcpServer {
    std::shared_ptr<State> state_;
  public:
    ServerWorker(std::shared_ptr<State> state, QObject* parent = nullptr)
      : QTcpServer(parent), state_(std::move(state)) {}

  protected:
    void incomingConnection(qintptr socketDescriptor) override {
      bool resp = true;
      {
        std::lock_guard<std::mutex> lk(state_->mu);
        resp = state_->responding;
      }
      if (!resp) {
        QTcpSocket* s = new QTcpSocket(this);
        s->setSocketDescriptor(socketDescriptor);
        s->abort();
        s->deleteLater();
        return;
      }

      QTcpSocket* socket = new QTcpSocket(this);
      socket->setSocketDescriptor(socketDescriptor);

      auto buf = std::make_shared<QByteArray>();

      connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buf]() {
        buf->append(socket->readAll());

        const int headerEnd = buf->indexOf("\r\n\r\n");
        if (headerEnd == -1) return;

        const QByteArray headerBytes = buf->left(headerEnd);
        int contentLength = 0;
        const auto lines = headerBytes.split('\n');
        for (const auto& rawLine : lines) {
          const auto line = rawLine.trimmed();
          if (line.toLower().startsWith("content-length:")) {
            contentLength = line.mid(15).trimmed().toInt();
          }
        }

        if (buf->size() < headerEnd + 4 + contentLength) return;

        const QByteArray body = buf->mid(headerEnd + 4, contentLength);

        const QByteArray reqLine = lines.isEmpty() ? QByteArray() : lines[0].trimmed();
        const auto parts = reqLine.split(' ');
        const QString method = QString::fromLatin1(parts.value(0));
        const QString rawTarget = QString::fromLatin1(parts.value(1));

        QUrl targetUrl(rawTarget);
        const QString path = targetUrl.path();

        QByteArray respBody;
        int statusCode = 200;
        QString statusText = QStringLiteral("OK");
        QString contentType = QStringLiteral("application/json");

        if (method == QLatin1String("GET") && path == QLatin1String("/system_stats")) {
          QJsonObject sysObj{
            {QStringLiteral("system"), QJsonObject{{QStringLiteral("comfyui_version"), QStringLiteral("fake")}}},
            {QStringLiteral("devices"), QJsonArray{}}
          };
          respBody = QJsonDocument(sysObj).toJson(QJsonDocument::Compact);
        } else if (method == QLatin1String("GET") && path == QLatin1String("/object_info")) {
          QString fixPath;
#ifdef SF_COMFY_FIXTURES
          fixPath = QStringLiteral(SF_COMFY_FIXTURES) + QStringLiteral("/object_info_min.json");
#else
          fixPath = QStringLiteral("fixtures/comfy/object_info_min.json");
#endif
          QFile f(fixPath);
          if (f.open(QIODevice::ReadOnly)) {
            respBody = f.readAll();
          } else {
            respBody = "{}";
          }
        } else if (method == QLatin1String("POST") && path == QLatin1String("/prompt")) {
          QJsonDocument promptDoc = QJsonDocument::fromJson(body);
          QJsonObject graph = promptDoc.object().value(QStringLiteral("prompt")).toObject();

          bool fail = false;
          QString failNodeId;
          for (auto it = graph.begin(); it != graph.end(); ++it) {
            if (it.value().toObject().value(QStringLiteral("class_type")).toString() == QLatin1String("FailMe")) {
              fail = true;
              failNodeId = it.key();
              break;
            }
          }

          if (fail) {
            statusCode = 400;
            statusText = QStringLiteral("Bad Request");
            QJsonObject errObj{
              {QStringLiteral("error"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("prompt_outputs_failed_validation")},
                {QStringLiteral("message"), QStringLiteral("Prompt outputs failed validation")}
              }},
              {QStringLiteral("node_errors"), QJsonObject{
                {failNodeId, QJsonObject{
                  {QStringLiteral("class_type"), QStringLiteral("FailMe")},
                  {QStringLiteral("errors"), QJsonArray{
                    QJsonObject{
                      {QStringLiteral("message"), QStringLiteral("Node failed validation")},
                      {QStringLiteral("details"), QStringLiteral("FailMe requested failure")}
                    }
                  }}
                }}
              }}
            };
            respBody = QJsonDocument(errObj).toJson(QJsonDocument::Compact);
          } else {
            QString pid;
            {
              std::lock_guard<std::mutex> lk(state_->mu);
              state_->queued.push_back(graph);
              pid = QStringLiteral("p%1").arg(state_->queued.size());
            }
            QJsonObject okObj{
              {QStringLiteral("prompt_id"), pid},
              {QStringLiteral("number"), 1},
              {QStringLiteral("node_errors"), QJsonObject{}}
            };
            respBody = QJsonDocument(okObj).toJson(QJsonDocument::Compact);
          }
        } else if (method == QLatin1String("GET") && path.startsWith(QLatin1String("/history/"))) {
          const QString pid = path.mid(9);
          int callIdx = 0;
          bool hang = false;
          {
            std::lock_guard<std::mutex> lk(state_->mu);
            callIdx = ++state_->historyCalls;
            hang = state_->hangHistory;
          }
          if (hang || callIdx <= 2) {
            respBody = "{}";
          } else {
            QString nStr = pid;
            if (nStr.startsWith(QLatin1Char('p'))) nStr = nStr.mid(1);
            if (nStr.isEmpty()) nStr = QStringLiteral("1");

            QString outFn = QStringLiteral("out_%1.png").arg(nStr);
            {
              std::lock_guard<std::mutex> lk(state_->mu);
              if (!state_->outputFilename.isEmpty()) outFn = state_->outputFilename;
            }

            QJsonObject histObj{
              {pid, QJsonObject{
                {QStringLiteral("outputs"), QJsonObject{
                  {QStringLiteral("9"), QJsonObject{
                    {QStringLiteral("images"), QJsonArray{
                      QJsonObject{
                        {QStringLiteral("filename"), outFn},
                        {QStringLiteral("subfolder"), QStringLiteral("")},
                        {QStringLiteral("type"), QStringLiteral("output")}
                      }
                    }}
                  }}
                }},
                {QStringLiteral("status"), QJsonObject{
                  {QStringLiteral("status_str"), QStringLiteral("success")},
                  {QStringLiteral("completed"), true},
                  {QStringLiteral("messages"), QJsonArray{}}
                }}
              }}
            };
            respBody = QJsonDocument(histObj).toJson(QJsonDocument::Compact);
          }
        } else if (method == QLatin1String("GET") && path == QLatin1String("/queue")) {
          QJsonArray queueRunning;
          QJsonArray queuePending;
          {
            std::lock_guard<std::mutex> lk(state_->mu);
            if (!state_->emptyQueue && !state_->queued.empty()) {
              QString pid = QStringLiteral("p%1").arg(state_->queued.size());
              queueRunning.append(QJsonArray{1, pid});
            }
          }
          QJsonObject qObj{
            {QStringLiteral("queue_running"), queueRunning},
            {QStringLiteral("queue_pending"), queuePending}
          };
          respBody = QJsonDocument(qObj).toJson(QJsonDocument::Compact);
        } else if (method == QLatin1String("GET") && path == QLatin1String("/view")) {
          contentType = QStringLiteral("image/png");
          QImage img(64, 64, QImage::Format_RGB32);
          img.fill(Qt::red);
          QBuffer b(&respBody);
          b.open(QIODevice::WriteOnly);
          img.save(&b, "PNG");
        } else if (method == QLatin1String("POST") && path == QLatin1String("/upload/image")) {
          state_->uploads++;
          QJsonObject upObj{
            {QStringLiteral("name"), QStringLiteral("up.png")},
            {QStringLiteral("subfolder"), QStringLiteral("")},
            {QStringLiteral("type"), QStringLiteral("input")}
          };
          respBody = QJsonDocument(upObj).toJson(QJsonDocument::Compact);
        } else if (method == QLatin1String("POST") && path == QLatin1String("/interrupt")) {
          {
            std::lock_guard<std::mutex> lk(state_->mu);
            state_->interrupted = true;
          }
          QJsonObject intObj{{QStringLiteral("status"), QStringLiteral("interrupted")}};
          respBody = QJsonDocument(intObj).toJson(QJsonDocument::Compact);
        } else {
          statusCode = 404;
          statusText = QStringLiteral("Not Found");
          respBody = "{}";
        }

        QByteArray resp = QByteArray("HTTP/1.1 ") + QByteArray::number(statusCode) + " " + statusText.toUtf8() + "\r\n" +
                          "Content-Type: " + contentType.toUtf8() + "\r\n" +
                          "Content-Length: " + QByteArray::number(respBody.size()) + "\r\n" +
                          "Connection: close\r\n\r\n" +
                          respBody;

        socket->write(resp);
        socket->disconnectFromHost();
      });
    }
  };

  ServerWorker* worker_ = nullptr;

public:
  explicit FakeComfyServer(quint16 requestedPort = 0) : state_(std::make_shared<State>()) {
    worker_ = new ServerWorker(state_);
    worker_->listen(QHostAddress(QStringLiteral("127.0.0.1")), requestedPort);
    state_->port = worker_->serverPort();
    worker_->moveToThread(&thread_);
    thread_.start();
  }

  ~FakeComfyServer() {
    QMetaObject::invokeMethod(worker_, [w = worker_]() { w->close(); }, Qt::BlockingQueuedConnection);
    worker_->deleteLater();
    thread_.quit();
    thread_.wait();
  }

  QUrl url() const {
    return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(state_->port));
  }

  quint16 port() const {
    return state_->port;
  }

  void setOutputFilename(const QString& fn) {
    std::lock_guard<std::mutex> lk(state_->mu);
    state_->outputFilename = fn;
  }

  void setHangHistory(bool hang) {
    std::lock_guard<std::mutex> lk(state_->mu);
    state_->hangHistory = hang;
  }

  void setEmptyQueue(bool empty) {
    std::lock_guard<std::mutex> lk(state_->mu);
    state_->emptyQueue = empty;
  }

  bool wasInterrupted() const {
    std::lock_guard<std::mutex> lk(state_->mu);
    return state_->interrupted;
  }

  void setResponding(bool resp) {
    std::lock_guard<std::mutex> lk(state_->mu);
    state_->responding = resp;
  }

  struct QueuedHelper {
    std::shared_ptr<State> s;
    size_t size() const { std::lock_guard<std::mutex> lk(s->mu); return s->queued.size(); }
    bool empty() const { std::lock_guard<std::mutex> lk(s->mu); return s->queued.empty(); }
    std::vector<QJsonObject> operator()() const { std::lock_guard<std::mutex> lk(s->mu); return s->queued; }
    QJsonObject operator[](size_t idx) const { std::lock_guard<std::mutex> lk(s->mu); return s->queued.at(idx); }
  };

  QueuedHelper queued{state_};

  struct UploadsHelper {
    std::shared_ptr<State> s;
    operator int() const { return s->uploads.load(); }
    int operator()() const { return s->uploads.load(); }
  };

  UploadsHelper uploads{state_};
};

} // namespace sf::test

using sf::test::FakeComfyServer;
