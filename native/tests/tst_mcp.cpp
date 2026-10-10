// MCP server over stdio: drives `splitframe.exe --mcp` (and `splitframe-cli mcp`) as a client would.

#include "engine/engine.h"
#include "engine/mcp.h"
#include "media_testutil.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>

using namespace sf;
using namespace sf::engine;
using namespace sf::test;

namespace {
QString Q(const char* s) { return QString::fromLatin1(s); }

class McpClient {
public:
  explicit McpClient(const QString& exe, const QStringList& args) {
    proc_.setProcessChannelMode(QProcess::SeparateChannels);
    proc_.start(exe, args);
    started_ = proc_.waitForStarted(30000);
  }
  ~McpClient() {
    proc_.closeWriteChannel();
    if (!proc_.waitForFinished(5000)) {
      proc_.kill();
      proc_.waitForFinished();
    }
  }
  bool started() const { return started_; }
  QProcess& process() { return proc_; }

  void send(const QJsonObject& o) {
    proc_.write(QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n');
    proc_.waitForBytesWritten(5000);
  }
  QJsonObject request(const QString& method, const QJsonObject& params = {}, int timeoutMs = 60000) {
    const int id = ++nextId_;
    send({{Q("jsonrpc"), Q("2.0")}, {Q("id"), id}, {Q("method"), method}, {Q("params"), params}});
    return waitResponse(id, timeoutMs);
  }
  QJsonObject call(const QString& tool, const QJsonObject& args = {}, const QJsonObject& meta = {}, int timeoutMs = 60000) {
    QJsonObject p{{Q("name"), tool}, {Q("arguments"), args}};
    if (!meta.isEmpty()) p.insert(Q("_meta"), meta);
    return request(Q("tools/call"), p, timeoutMs);
  }
  // Reads messages until the response with `id`; everything else lands in notifications().
  QJsonObject waitResponse(int id, int timeoutMs) {
    QElapsedTimer t;
    t.start();
    for (;;) {
      while (!lines_.isEmpty()) {
        const QJsonObject o = QJsonDocument::fromJson(lines_.takeFirst()).object();
        if (o.contains(Q("id")) && !o.contains(Q("method")) && o.value(Q("id")).toInt() == id) return o;
        notes_.append(o);
      }
      if (t.elapsed() > timeoutMs) return {{Q("timeout"), true}};
      pump(200);
    }
  }
  // Waits until a notification matching `method` (and optionally satisfying `pred`) has arrived.
  bool waitNotification(const QString& method, int timeoutMs, const std::function<bool(const QJsonObject&)>& pred = {}) {
    QElapsedTimer t;
    t.start();
    for (;;) {
      for (const QJsonObject& n : std::as_const(notes_))
        if (n.value(Q("method")).toString() == method && (!pred || pred(n.value(Q("params")).toObject()))) return true;
      while (!lines_.isEmpty()) notes_.append(QJsonDocument::fromJson(lines_.takeFirst()).object());
      for (const QJsonObject& n : std::as_const(notes_))
        if (n.value(Q("method")).toString() == method && (!pred || pred(n.value(Q("params")).toObject()))) return true;
      if (t.elapsed() > timeoutMs) return false;
      pump(200);
    }
  }
  const QList<QJsonObject>& notifications() const { return notes_; }

private:
  void pump(int ms) {
    proc_.waitForReadyRead(ms);
    buf_ += proc_.readAllStandardOutput();
    int nl;
    while ((nl = static_cast<int>(buf_.indexOf('\n'))) >= 0) {
      const QByteArray line = buf_.left(nl).trimmed();
      buf_.remove(0, nl + 1);
      if (!line.isEmpty()) lines_.append(line);
    }
  }
  QProcess proc_;
  bool started_ = false;
  int nextId_ = 0;
  QByteArray buf_;
  QList<QByteArray> lines_;
  QList<QJsonObject> notes_;
};

QJsonObject structured(const QJsonObject& resp) { return resp.value(Q("result")).toObject().value(Q("structuredContent")).toObject(); }
bool isError(const QJsonObject& resp) { return resp.value(Q("result")).toObject().value(Q("isError")).toBool(); }
} // namespace

class TstMcp : public QObject {
  Q_OBJECT

  QTemporaryDir dir_;
  QString clip_;

  void handshake(McpClient& c) {
    const QJsonObject init = c.request(Q("initialize"), {{Q("protocolVersion"), Q("2025-06-18")}, {Q("capabilities"), QJsonObject{}}, {Q("clientInfo"), QJsonObject{{Q("name"), Q("test")}, {Q("version"), Q("1")}}}});
    QCOMPARE(init.value(Q("result")).toObject().value(Q("protocolVersion")).toString(), Q("2025-06-18"));
    c.send({{Q("jsonrpc"), Q("2.0")}, {Q("method"), Q("notifications/initialized")}});
  }

private slots:
  void initTestCase() {
    QVERIFY(dir_.isValid());
    if (ffmpegExe().isEmpty()) QSKIP("SF_FFMPEG_EXE is not set");
    clip_ = dir_.filePath(QStringLiteral("clip.mp4"));
    QString log;
    QVERIFY2(runFfmpeg({Q("-f"), Q("lavfi"), Q("-i"), indexSource(320, 180, 25, 2), Q("-f"), Q("lavfi"), Q("-i"), Q("sine=frequency=440:sample_rate=48000:duration=2"), Q("-c:v"), Q("libx264"),
                        Q("-preset"), Q("ultrafast"), Q("-pix_fmt"), Q("yuv420p"), Q("-c:a"), Q("aac"), Q("-shortest"), clip_}, &log), qPrintable(log));
  }

  void initializeNegotiatesVersion() {
    McpClient c(QStringLiteral(SF_APP_EXE), {Q("--mcp")});
    QVERIFY2(c.started(), qPrintable(c.process().errorString()));
    QJsonObject r = c.request(Q("initialize"), {{Q("protocolVersion"), Q("2024-11-05")}, {Q("capabilities"), QJsonObject{}}, {Q("clientInfo"), QJsonObject{{Q("name"), Q("t")}, {Q("version"), Q("1")}}}});
    QJsonObject res = r.value(Q("result")).toObject();
    QCOMPARE(res.value(Q("protocolVersion")).toString(), Q("2024-11-05"));
    QVERIFY(res.value(Q("capabilities")).toObject().contains(Q("tools")));
    QVERIFY(res.value(Q("capabilities")).toObject().value(Q("resources")).toObject().value(Q("subscribe")).toBool());
    QCOMPARE(res.value(Q("serverInfo")).toObject().value(Q("name")).toString(), Q("splitframe"));
    QVERIFY(!res.value(Q("instructions")).toString().isEmpty());
    // unknown version: the server answers with its latest
    r = c.request(Q("initialize"), {{Q("protocolVersion"), Q("1999-01-01")}});
    QCOMPARE(r.value(Q("result")).toObject().value(Q("protocolVersion")).toString(), McpServer::supportedVersions().first());
    c.send({{Q("jsonrpc"), Q("2.0")}, {Q("method"), Q("notifications/initialized")}});
    QVERIFY(c.request(Q("ping")).contains(Q("result")));
    // unknown method / malformed
    QCOMPARE(c.request(Q("no/such")).value(Q("error")).toObject().value(Q("code")).toInt(), -32601);
    c.process().write("{broken\n");
    QElapsedTimer t;
    t.start();
    QByteArray got;
    while (!got.contains('\n') && t.elapsed() < 10000) {
      c.process().waitForReadyRead(200);
      got += c.process().readAllStandardOutput();
    }
    QCOMPARE(QJsonDocument::fromJson(got).object().value(Q("error")).toObject().value(Q("code")).toInt(), -32700);
    // closing stdin ends the process cleanly
    c.process().closeWriteChannel();
    QVERIFY(c.process().waitForFinished(15000));
    QCOMPARE(c.process().exitCode(), 0);
  }

  void toolsListMatchesRegistry() {
    McpClient c(QStringLiteral(SF_APP_EXE), {Q("--mcp")});
    QVERIFY(c.started());
    handshake(c);
    const QJsonArray tools = c.request(Q("tools/list")).value(Q("result")).toObject().value(Q("tools")).toArray();
    Engine local;
    QCOMPARE(tools.size(), static_cast<int>(local.commands().size()));
    QSet<QString> names;
    for (const QJsonValue& v : tools) {
      const QJsonObject tool = v.toObject();
      const QString name = tool.value(Q("name")).toString();
      QVERIFY2(QRegularExpression(Q("^[a-zA-Z0-9_-]{1,64}$")).match(name).hasMatch(), qPrintable(name));
      names.insert(name);
      const CommandSpec* spec = local.find(name);
      QVERIFY2(spec, qPrintable(name));
      QCOMPARE(McpServer::toolName(spec->name), name);
      QCOMPARE(tool.value(Q("inputSchema")).toObject(), spec->inputSchema);
      QCOMPARE(tool.value(Q("annotations")).toObject().value(Q("readOnlyHint")).toBool(), !spec->mutates);
      QCOMPARE(tool.value(Q("_meta")).toObject().value(Q("splitframe/domain")).toString(), spec->domain);
      QCOMPARE(tool.value(Q("description")).toString(), spec->description);
    }
    QCOMPARE(names.size(), tools.size());
    QVERIFY(names.contains(Q("timeline_get")) && names.contains(Q("ops_apply")) && names.contains(Q("export_start")));
  }

  void editAndReadBack() {
    McpClient c(QStringLiteral(SF_APP_EXE), {Q("--mcp")});
    QVERIFY(c.started());
    handshake(c);
    // the server starts with an empty project session already; make a known one
    QJsonObject r = c.call(Q("project_new"), {{Q("name"), Q("Mcp")}, {Q("width"), 320}, {Q("height"), 180}, {Q("fps"), 25}});
    QVERIFY(!isError(r));
    QCOMPARE(structured(r).value(Q("name")).toString(), Q("Mcp"));
    QVERIFY(r.value(Q("result")).toObject().value(Q("content")).toArray().at(0).toObject().value(Q("type")).toString() == Q("text"));
    r = c.call(Q("media_import"), {{Q("paths"), QJsonArray{clip_}}});
    QVERIFY(!isError(r));
    const QString asset = structured(r).value(Q("added")).toArray().at(0).toString();
    QVERIFY(!asset.isEmpty());
    r = c.call(Q("clip_add"), {{Q("assetId"), asset}, {Q("origin"), Q("agent:editor")}});
    QVERIFY(!isError(r));
    const QString itemId = structured(r).value(Q("created")).toObject().value(Q("items")).toArray().at(0).toString();
    r = c.call(Q("item_split"), {{Q("itemId"), itemId}, {Q("atFrame"), 20}});
    QVERIFY(!isError(r));
    r = c.call(Q("timeline_get"));
    int n = 0;
    for (const QJsonValue& t : structured(r).value(Q("tracks")).toArray()) n += t.toObject().value(Q("items")).toArray().size();
    QCOMPARE(n, 2);
    r = c.call(Q("history_list"));
    QCOMPARE(structured(r).value(Q("entries")).toArray().at(0).toObject().value(Q("actor")).toString(), Q("agent:editor"));
    // the Electron alias works as a tool name too
    QVERIFY(!isError(c.call(Q("getTimeline"))));

    // errors: bad arguments and engine failures are results with isError, an unknown tool is a protocol error
    r = c.call(Q("clip_add"), {});
    QVERIFY(isError(r));
    QVERIFY(r.value(Q("result")).toObject().value(Q("content")).toArray().at(0).toObject().value(Q("text")).toString().contains(Q("assetId")));
    r = c.call(Q("clip_add"), {{Q("assetId"), 12}});
    QVERIFY(isError(r));
    r = c.call(Q("item_get"), {{Q("itemId"), Q("itm_nope")}});
    QVERIFY(isError(r));
    QCOMPARE(structured(r).value(Q("error")).toObject().value(Q("code")).toString(), Q("not_found"));
    r = c.call(Q("not_a_tool"));
    QVERIFY(!r.contains(Q("result")));
    QCOMPARE(r.value(Q("error")).toObject().value(Q("code")).toInt(), -32602);

    // resources
    const QJsonArray res = c.request(Q("resources/list")).value(Q("result")).toObject().value(Q("resources")).toArray();
    QSet<QString> uris;
    for (const QJsonValue& v : res) uris.insert(v.toObject().value(Q("uri")).toString());
    QVERIFY(uris.contains(Q("splitframe://timeline")) && uris.contains(Q("splitframe://project")) && uris.contains(Q("splitframe://assets")));
    QCOMPARE(c.request(Q("resources/templates/list")).value(Q("result")).toObject().value(Q("resourceTemplates")).toArray().size(), 1);
    r = c.request(Q("resources/read"), {{Q("uri"), Q("splitframe://timeline")}});
    const QJsonObject content = r.value(Q("result")).toObject().value(Q("contents")).toArray().at(0).toObject();
    QCOMPARE(content.value(Q("mimeType")).toString(), Q("application/json"));
    const QJsonObject tl = QJsonDocument::fromJson(content.value(Q("text")).toString().toUtf8()).object();
    QCOMPARE(tl.value(Q("project")).toObject().value(Q("name")).toString(), Q("Mcp"));
    QCOMPARE(c.request(Q("resources/read"), {{Q("uri"), Q("splitframe://assets")}}).value(Q("result")).toObject().value(Q("contents")).toArray().size(), 1);
    QCOMPARE(c.request(Q("resources/read"), {{Q("uri"), Q("splitframe://nothing")}}).value(Q("error")).toObject().value(Q("code")).toInt(), -32002);
    QCOMPARE(c.request(Q("resources/read"), {{Q("uri"), Q("http://example.com/x")}}).value(Q("error")).toObject().value(Q("code")).toInt(), -32002);
    // a rendered frame as a blob (tiny canvas; skipped when this machine has no D3D11 device)
    r = c.request(Q("resources/read"), {{Q("uri"), Q("splitframe://frame/3")}});
    if (r.contains(Q("result"))) {
      const QJsonObject blob = r.value(Q("result")).toObject().value(Q("contents")).toArray().at(0).toObject();
      QCOMPARE(blob.value(Q("mimeType")).toString(), Q("image/png"));
      QImage img;
      QVERIFY(img.loadFromData(QByteArray::fromBase64(blob.value(Q("blob")).toString().toLatin1()), "PNG"));
      QCOMPARE(img.size(), QSize(320, 180));
      // frame_render with inline=true: image content block
      r = c.call(Q("frame_render"), {{Q("frame"), 2}, {Q("inline"), true}});
      const QJsonArray blocks = r.value(Q("result")).toObject().value(Q("content")).toArray();
      QCOMPARE(blocks.at(1).toObject().value(Q("type")).toString(), Q("image"));
      QVERIFY(!structured(r).contains(Q("base64")));
    } else {
      qInfo() << "frame resource unavailable:" << r.value(Q("error")).toObject().value(Q("message")).toString();
    }

    // subscribe: the next edit sends notifications/resources/updated
    QVERIFY(c.request(Q("resources/subscribe"), {{Q("uri"), Q("splitframe://timeline")}}).contains(Q("result")));
    QVERIFY(!isError(c.call(Q("marker_add"), {{Q("frame"), 4}, {Q("label"), Q("n")}})));
    QVERIFY(c.waitNotification(Q("notifications/resources/updated"), 10000, [](const QJsonObject& p) { return p.value(Q("uri")).toString() == Q("splitframe://timeline"); }));
    // logging: events are forwarded as notifications/message after setLevel
    QVERIFY(c.request(Q("logging/setLevel"), {{Q("level"), Q("info")}}).contains(Q("result")));
    c.call(Q("marker_add"), {{Q("frame"), 5}, {Q("label"), Q("m2")}});
    QVERIFY(c.waitNotification(Q("notifications/message"), 10000, [](const QJsonObject& p) { return p.value(Q("data")).toObject().value(Q("event")).toString() == Q("ops.applied"); }));
  }

  void exportReportsProgress() {
    McpClient c(QStringLiteral(SF_APP_EXE), {Q("--mcp")});
    QVERIFY(c.started());
    handshake(c);
    c.call(Q("project_new"), {{Q("width"), 320}, {Q("height"), 180}, {Q("fps"), 25}});
    const QString asset = structured(c.call(Q("media_import"), {{Q("paths"), QJsonArray{clip_}}})).value(Q("added")).toArray().at(0).toString();
    c.call(Q("clip_add"), {{Q("assetId"), asset}});
    const QString out = dir_.filePath(Q("mcp.mp4"));
    QJsonObject r = c.call(Q("export_start"), {{Q("out"), out}, {Q("quality"), Q("draft")}, {Q("range"), QJsonArray{0, 12}}}, {{Q("progressToken"), Q("tok-1")}});
    QVERIFY(!isError(r));
    const QString job = structured(r).value(Q("jobId")).toString();
    r = c.call(Q("export_status"), {{Q("jobId"), job}, {Q("waitMs"), 90000}}, {}, 120000);
    QCOMPARE(structured(r).value(Q("state")).toString(), Q("done"));
    QVERIFY(QFileInfo(out).size() > 1000);
    QVERIFY(c.waitNotification(Q("notifications/progress"), 10000, [](const QJsonObject& p) {
      return p.value(Q("progressToken")).toString() == Q("tok-1") && p.value(Q("progress")).toDouble() == p.value(Q("total")).toDouble();
    }));
  }

  void cliMcpSubcommand() {
    McpClient c(QStringLiteral(SF_CLI_EXE), {Q("mcp")});
    QVERIFY2(c.started(), qPrintable(c.process().errorString()));
    handshake(c);
    const QJsonObject r = c.call(Q("engine_ping"));
    QVERIFY(!isError(r));
    QCOMPARE(structured(r).value(Q("engine")).toString(), Q("splitframe"));
  }
};

QTEST_GUILESS_MAIN(TstMcp)
#include "tst_mcp.moc"
