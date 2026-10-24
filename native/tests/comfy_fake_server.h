#pragma once

// Fake ComfyUI server for unit and integration tests: serves minimal HTTP/1.1 on loopback.

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QDataStream>
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
    QString outputKind = QStringLiteral("image"); // image | video | audio: history array key + default filename ext
    QString objectInfoPath;                       // when set, /object_info serves this file instead of the fixture
    bool routerMode = false;   // serve the minimal Comfy Router API surface for 15-GEN Part G tests
    int routerPolls = 0;       // status polls per request id, to make IN_PROGRESS -> COMPLETED deterministic
    QString routerApiKey;      // X-API-Key value seen on the last router request (for key handling asserts)
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
        QByteArray extraHeaders;
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
          {
            std::lock_guard<std::mutex> lk(state_->mu);
            fixPath = state_->objectInfoPath;
          }
          if (fixPath.isEmpty()) {
#ifdef SF_COMFY_FIXTURES
            fixPath = QStringLiteral(SF_COMFY_FIXTURES) + QStringLiteral("/object_info_min.json");
#else
            fixPath = QStringLiteral("fixtures/comfy/object_info_min.json");
#endif
          }
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

            QString outKey, outExt;
            {
              std::lock_guard<std::mutex> lk(state_->mu);
              if (state_->outputKind == QLatin1String("video")) {
                outKey = QStringLiteral("gifs");
                outExt = QStringLiteral("webp");
              } else if (state_->outputKind == QLatin1String("audio")) {
                outKey = QStringLiteral("audio");
                outExt = QStringLiteral("wav");
              } else {
                outKey = QStringLiteral("images");
                outExt = QStringLiteral("png");
              }
            }
            QString outFn = QStringLiteral("out_%1.%2").arg(nStr, outExt);
            {
              std::lock_guard<std::mutex> lk(state_->mu);
              if (!state_->outputFilename.isEmpty()) outFn = state_->outputFilename;
            }

            QJsonObject histObj{
              {pid, QJsonObject{
                {QStringLiteral("outputs"), QJsonObject{
                  {QStringLiteral("9"), QJsonObject{
                    {outKey, QJsonArray{
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
          const QUrlQuery viewQuery(targetUrl.query());
          const QString viewFn = viewQuery.queryItemValue(QStringLiteral("filename"));
          if (viewFn.endsWith(QStringLiteral(".wav"), Qt::CaseInsensitive)) {
            // 0.5 s of 16-bit mono 8 kHz PCM silence so the probe sees a real audio file.
            contentType = QStringLiteral("audio/wav");
            QDataStream s(&respBody, QIODevice::WriteOnly);
            s.setByteOrder(QDataStream::LittleEndian);
            const int sampleRate = 8000;
            const int n = sampleRate / 2;
            s.writeRawData("RIFF", 4);
            s << quint32(36 + n * 2);
            s.writeRawData("WAVE", 4);
            s.writeRawData("fmt ", 4);
            s << quint32(16);
            s << quint16(1) << quint16(1);
            s << quint32(sampleRate);
            s << quint32(sampleRate * 2);
            s << quint16(2) << quint16(16);
            s.writeRawData("data", 4);
            s << quint32(n * 2);
            for (int i = 0; i < n; ++i) s << qint16(0);
          } else {
            contentType = QStringLiteral("image/png");
            QImage img(64, 64, QImage::Format_RGB32);
            img.fill(Qt::red);
            QBuffer b(&respBody);
            b.open(QIODevice::WriteOnly);
            img.save(&b, "PNG");
          }
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
        } else if (path == QLatin1String("/v2/models") || path.startsWith(QLatin1String("/v2/models/"))) {
          // Minimal Comfy Router API surface (15-GEN Part G tests): catalog, one model, schema with
          // ETag, queued submit/status/collect/cancel, and the error shapes the engine maps.
          QString apiKey, etagHdr;
          for (const auto& rawLine : lines) {
            const auto line = rawLine.trimmed();
            if (line.toLower().startsWith("x-api-key:")) apiKey = line.mid(line.indexOf(':') + 1).trimmed();
            if (line.toLower().startsWith("if-none-match:")) etagHdr = QString::fromUtf8(line.mid(14).trimmed());
          }
          QString errorHdr;
          {
            std::lock_guard<std::mutex> lk(state_->mu);
            state_->routerApiKey = apiKey;
          }
          if (apiKey.isEmpty()) {
            statusCode = 401;
            statusText = QStringLiteral("Unauthorized");
            errorHdr = QStringLiteral("unauthorized");
            respBody = QJsonDocument(QJsonObject{{QStringLiteral("detail"), QStringLiteral("Missing API key")}}).toJson(QJsonDocument::Compact);
          } else {
            QString sub = path.mid(QLatin1String("/v2/models").size()); if (sub.startsWith(QLatin1Char('/'))) sub.remove(0, 1); const QStringList seg = sub.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            const QString modelId = seg.size() >= 2 ? seg.at(0) + QLatin1Char('/') + seg.at(1) : QString();
            const QString suffix = seg.size() >= 3 ? seg.mid(2).join(QLatin1Char('/')) : QString();

            if (suffix == QLatin1String("openapi.json")) {
              extraHeaders += "ETag: \"v1\"\r\n";
              // openapi.json with an ETag; If-None-Match "v1" -> 304 (no body)
              if (etagHdr == QLatin1String("\"v1\"")) {
                statusCode = 304;
                statusText = QStringLiteral("Not Modified");
              } else {
                respBody = QJsonDocument(QJsonObject{
                  {QStringLiteral("openapi"), QStringLiteral("3.1.0")},
                  {QStringLiteral("x-comfy-input-schema-authored"), true}
                }).toJson(QJsonDocument::Compact);
              }
            } else if (suffix.isEmpty() && seg.size() == 2 && modelId == QLatin1String("test/one")) {
              respBody = QJsonDocument(QJsonObject{
                {QStringLiteral("id"), QStringLiteral("test/one")},
                {QStringLiteral("provider"), QStringLiteral("test")},
                {QStringLiteral("model"), QStringLiteral("one")}
              }).toJson(QJsonDocument::Compact);
            } else if (suffix.isEmpty()) {
              // catalog list (GET /v2/models)
              respBody = QJsonDocument(QJsonObject{
                {QStringLiteral("data"), QJsonArray{
                  QJsonObject{{QStringLiteral("id"), QStringLiteral("test/one")},
                              {QStringLiteral("provider"), QStringLiteral("test")}, {QStringLiteral("model"), QStringLiteral("one")}},
                  QJsonObject{{QStringLiteral("id"), QStringLiteral("test/insufficient")},
                              {QStringLiteral("provider"), QStringLiteral("test")}, {QStringLiteral("model"), QStringLiteral("insufficient")}}
                }},
                {QStringLiteral("has_more"), false},
                {QStringLiteral("limit"), 20}
              }).toJson(QJsonDocument::Compact);
            } else if (suffix.startsWith(QLatin1String("requests/")) || suffix == QLatin1String("requests")) {
              const bool isStatus = seg.size() >= 5 && seg.at(4) == QLatin1String("status");
              const bool isCancel = seg.size() >= 5 && seg.at(4) == QLatin1String("cancel");
              const QString rid = seg.size() >= 4 ? seg.at(3) : QString();
              QString mode;
              if (!rid.isEmpty()) mode = rid.mid(4); // "req-<mode>" -> <mode>

              if (method == QLatin1String("POST") && suffix == QLatin1String("requests")) {
                if (modelId == QLatin1String("test/insufficient")) {
                  statusCode = 402;
                  statusText = QStringLiteral("Payment Required");
                  errorHdr = QStringLiteral("insufficient_credits");
                } else if (modelId == QLatin1String("test/missing")) {
                  statusCode = 404;
                  statusText = QStringLiteral("Not Found");
                  errorHdr = QStringLiteral("model_not_found");
                } else {
                  QJsonDocument bodyDoc = QJsonDocument::fromJson(body);
                  QString bmode = bodyDoc.object().value(QStringLiteral("mode")).toString();
                  if (bmode.isEmpty()) bmode = QStringLiteral("1");
                  std::lock_guard<std::mutex> lk(state_->mu);
                  state_->routerPolls = 0;
                  Q_UNUSED(lk);
                  QJsonObject subObj{
                    {QStringLiteral("request_id"), QStringLiteral("req-%1").arg(bmode)},
                    {QStringLiteral("status"), QStringLiteral("IN_QUEUE")},
                    {QStringLiteral("queue_position"), 1},
                    {QStringLiteral("status_url"), QStringLiteral("status_url")},
                    {QStringLiteral("response_url"), QStringLiteral("response_url")},
                    {QStringLiteral("cancel_url"), QStringLiteral("cancel_url")}
                  };
                  respBody = QJsonDocument(subObj).toJson(QJsonDocument::Compact);
                  statusCode = 201;
                  statusText = QStringLiteral("Created");
                }
              } else if (isStatus) {
                if (mode == QLatin1String("fail")) {
                  respBody = QJsonDocument(QJsonObject{
                    {QStringLiteral("request_id"), QStringLiteral("req-fail")},
                    {QStringLiteral("status"), QStringLiteral("COMPLETED")},
                    {QStringLiteral("error_type"), QStringLiteral("content_policy_violation")}
                  }).toJson(QJsonDocument::Compact);
                } else {
                  std::lock_guard<std::mutex> lk(state_->mu);
                  const int poll = ++state_->routerPolls;
                  Q_UNUSED(lk);
                  QJsonObject st{
                    {QStringLiteral("request_id"), rid},
                    {QStringLiteral("queue_position"), 0},
                    {QStringLiteral("status"), poll < 2 ? QStringLiteral("IN_PROGRESS") : QStringLiteral("COMPLETED")}
                  };
                  respBody = QJsonDocument(st).toJson(QJsonDocument::Compact);
                }
              } else if (isCancel) {
                statusCode = 202;
                statusText = QStringLiteral("Accepted");
                respBody = QJsonDocument(QJsonObject{{QStringLiteral("status"), QStringLiteral("CANCELLATION_REQUESTED")}}).toJson(QJsonDocument::Compact);
              } else if (method == QLatin1String("GET")) {
                // collect: result read
                if (mode == QLatin1String("fail")) {
                  statusCode = 500;
                  statusText = QStringLiteral("Internal Server Error");
                  errorHdr = QStringLiteral("content_policy_violation");
                  respBody = QJsonDocument(QJsonObject{{QStringLiteral("detail"), QStringLiteral("refused by policy")}}).toJson(QJsonDocument::Compact);
                } else if (mode == QLatin1String("bin")) {
                  statusCode = 200;
                  contentType = QStringLiteral("image/png");
                  QImage img(32, 32, QImage::Format_RGB32);
                  img.fill(Qt::green);
                  QBuffer b(&respBody);
                  b.open(QIODevice::WriteOnly);
                  img.save(&b, "PNG");
                } else {
                  respBody = QJsonDocument(QJsonObject{
                    {QStringLiteral("result"), QJsonObject{
                      {QStringLiteral("sample"), QStringLiteral("https://example.invalid/sample.png")}
                    }}
                  }).toJson(QJsonDocument::Compact);
                }
              }
            }
            if (!errorHdr.isEmpty()) extraHeaders += "X-Comfy-Error-Type: " + errorHdr.toUtf8() + "\r\n";
            if (statusCode == 304 && !extraHeaders.contains("ETag:")) extraHeaders += "ETag: \"v1\"\r\n";
          }
        } else {
          statusCode = 404;
          statusText = QStringLiteral("Not Found");
          respBody = "{}";
        }

        QByteArray resp = QByteArray("HTTP/1.1 ") + QByteArray::number(statusCode) + " " + statusText.toUtf8() + "\r\n" +
                          "Content-Type: " + contentType.toUtf8() + "\r\n" +
                          extraHeaders +
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

  // Switches the /history output array (images | gifs | audio) and the default filename extension.
  void setOutputKind(const QString& kind) {
    std::lock_guard<std::mutex> lk(state_->mu);
    state_->outputKind = kind;
  }

  // Serves this object_info JSON instead of the fixture when non-empty.
  void setObjectInfoPath(const QString& path) {
    std::lock_guard<std::mutex> lk(state_->mu);
    state_->objectInfoPath = path;
  }

  // The API key string the most recent Router request carried (tests assert key handling).
  QString routerApiKey() const {
    std::lock_guard<std::mutex> lk(state_->mu);
    return state_->routerApiKey;
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
