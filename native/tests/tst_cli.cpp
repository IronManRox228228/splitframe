// splitframe-cli: subcommands, --json output and exit codes; `serve` over HTTP and stdio.

#include "core/schema.h"
#include "core/timeline_doc.h"
#include "media_testutil.h"
#include "render_testutil.h"

#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

using namespace sf;
using namespace sf::test;

namespace {
QString Q(const char* s) { return QString::fromLatin1(s); }
} // namespace

class TstCli : public QObject {
  Q_OBJECT

  QTemporaryDir dir_;
  QString project_, clip_;

  struct Run {
    int code = -1;
    QString out, err;
    QJsonObject json() const { return QJsonDocument::fromJson(out.toUtf8()).object(); }
  };
  Run run(const QStringList& args, int timeoutMs = 120000) {
    QProcess p;
    p.start(QStringLiteral(SF_CLI_EXE), args);
    Run r;
    if (!p.waitForStarted(30000)) {
      r.err = p.errorString();
      return r;
    }
    if (!p.waitForFinished(timeoutMs)) {
      p.kill();
      p.waitForFinished();
      r.err = Q("timed out");
      return r;
    }
    r.code = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -2;
    r.out = QString::fromUtf8(p.readAllStandardOutput());
    r.err = QString::fromUtf8(p.readAllStandardError());
    return r;
  }
  QString writeJson(const QString& name, const QJsonValue& v) {
    const QString path = dir_.filePath(name);
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(v.isArray() ? QJsonDocument(v.toArray()).toJson() : QJsonDocument(v.toObject()).toJson());
    return path;
  }
  int markers(const QString& projectFile) { return static_cast<int>(loadProjectBundle(projectFile).doc.markers.size()); }

private slots:
  void initTestCase() {
    QVERIFY(dir_.isValid());
    if (ffmpegExe().isEmpty()) QSKIP("SF_FFMPEG_EXE is not set");
    clip_ = dir_.filePath(Q("clip.mp4"));
    QString log;
    QVERIFY2(runFfmpeg({Q("-f"), Q("lavfi"), Q("-i"), indexSource(320, 180, 25, 2), Q("-f"), Q("lavfi"), Q("-i"), Q("sine=frequency=440:sample_rate=48000:duration=2"), Q("-c:v"), Q("libx264"),
                        Q("-preset"), Q("ultrafast"), Q("-pix_fmt"), Q("yuv420p"), Q("-c:a"), Q("aac"), Q("-shortest"), clip_}, &log), qPrintable(log));
    ProjectBundle b;
    b.doc = newDoc(320, 180, 25);
    b.doc.project.name = Q("CliFixture");
    addMedia(b.doc, ItemType::Video, mainTrack(b.doc), Q("itm_a"), Q("ast_a"), 0, 50);
    Asset a;
    a.id = Q("ast_a");
    a.projectId = b.doc.project.id;
    a.kind = AssetKind::Video;
    a.path = Q("clip.mp4");
    a.originalName = Q("clip.mp4");
    a.status = AssetStatus::Analyzed;
    a.durationMs = 2000;
    a.width = 320;
    a.height = 180;
    a.fps = 25;
    a.hasAudio = true;
    a.createdAt = Q("2026-01-01T00:00:00.000Z");
    b.assets.push_back(a);
    project_ = dir_.filePath(Q("p.json"));
    saveProjectBundle(project_, b);
  }

  void usageAndExitCodes() {
    QCOMPARE(run({}).code, 2);
    QCOMPARE(run({Q("--help")}).code, 0);
    QVERIFY(run({Q("--version")}).out.startsWith(Q("splitframe-cli")));
    Run r = run({Q("frobnicate"), project_});
    QCOMPARE(r.code, 2);
    r = run({Q("info")});
    QCOMPARE(r.code, 2);
    QVERIFY(r.err.contains(Q("project")));
    QCOMPARE(run({Q("render-frame"), project_}).code, 2); // --out missing
    QCOMPARE(run({Q("timeline"), project_, Q("--from")}).code, 2); // value missing
    QCOMPARE(run({Q("info"), dir_.filePath(Q("missing.json"))}).code, 4);
    r = run({Q("--json"), Q("info"), dir_.filePath(Q("missing.json"))});
    QCOMPARE(r.code, 4);
    QVERIFY(!r.json().value(Q("ok")).toBool());
    QCOMPARE(r.json().value(Q("error")).toObject().value(Q("code")).toString(), Q("not_found"));
    QFile bad(dir_.filePath(Q("bad.json")));
    QVERIFY(bad.open(QIODevice::WriteOnly));
    bad.write("{ not a project");
    bad.close();
    QCOMPARE(run({Q("info"), bad.fileName()}).code, 4);
  }

  void infoAndTimeline() {
    Run r = run({Q("info"), project_});
    QCOMPARE(r.code, 0);
    QVERIFY2(r.out.contains(Q("CliFixture")) && r.out.contains(Q("320x180 @ 25 fps")), qPrintable(r.out));
    r = run({Q("--json"), Q("info"), project_});
    QCOMPARE(r.code, 0);
    QVERIFY(r.json().value(Q("ok")).toBool());
    QCOMPARE(r.json().value(Q("result")).toObject().value(Q("items")).toInt(), 1);
    r = run({Q("timeline"), project_});
    QCOMPARE(r.code, 0);
    QVERIFY2(r.out.contains(Q("itm_a")) && r.out.contains(Q("video")), qPrintable(r.out));
    r = run({Q("--json"), Q("timeline"), project_, Q("--detail")});
    QVERIFY(r.json().value(Q("result")).toObject().value(Q("tracks")).toArray().size() >= 2);
    r = run({Q("--json"), Q("timeline"), project_, Q("--from"), Q("60"), Q("--to"), Q("70")});
    int n = 0;
    for (const QJsonValue& t : r.json().value(Q("result")).toObject().value(Q("tracks")).toArray()) n += t.toObject().value(Q("items")).toArray().size();
    QCOMPARE(n, 0);
  }

  void applyOps() {
    const QString work = dir_.filePath(Q("work.json"));
    QVERIFY(QFile::copy(project_, work));
    const QString clipCopy = dir_.filePath(Q("clip.mp4"));
    Q_UNUSED(clipCopy);
    const QJsonArray ops{QJsonObject{{Q("type"), Q("marker.add")}, {Q("marker"), QJsonObject{{Q("id"), Q("mrk_0123456789abcdef")}, {Q("frame"), 3}, {Q("label"), Q("x")}}}}};
    const QString opsFile = writeJson(Q("ops.json"), ops);
    Run r = run({Q("apply"), work, opsFile});
    QCOMPARE(r.code, 0);
    QVERIFY2(r.out.contains(Q("not saved")), qPrintable(r.out));
    QCOMPARE(markers(work), 0);
    r = run({Q("--json"), Q("apply"), work, opsFile, Q("--save"), Q("--origin"), Q("agent:test")});
    QCOMPARE(r.code, 0);
    QVERIFY(r.json().value(Q("result")).toObject().value(Q("saved")).toBool());
    QCOMPARE(r.json().value(Q("result")).toObject().value(Q("applied")).toInt(), 1);
    QCOMPARE(markers(work), 1);
    // ops from an {"ops": [...]} wrapper into another file
    const QString copy = dir_.filePath(Q("copy.json"));
    r = run({Q("apply"), project_, writeJson(Q("ops2.json"), QJsonObject{{Q("ops"), ops}}), Q("--out"), copy});
    QCOMPARE(r.code, 0);
    QCOMPARE(markers(copy), 1);
    QCOMPARE(markers(project_), 0);
    // a failing op: exit 1, error value, nothing written
    const QString badOps = writeJson(Q("badops.json"), QJsonArray{QJsonObject{{Q("type"), Q("item.move")}, {Q("itemId"), Q("itm_missing")}, {Q("startFrame"), 1}}});
    r = run({Q("--json"), Q("apply"), work, badOps, Q("--save")});
    QCOMPARE(r.code, 1);
    QCOMPARE(r.json().value(Q("error")).toObject().value(Q("code")).toString(), Q("edit_rejected"));
    // malformed / missing ops file
    QCOMPARE(run({Q("apply"), work, dir_.filePath(Q("nope.json"))}).code, 4);
    QCOMPARE(run({Q("apply"), work, writeJson(Q("empty.json"), QJsonArray{})}).code, 2);
    r = run({Q("--json"), Q("apply"), work, writeJson(Q("invalid.json"), QJsonArray{QJsonObject{{Q("type"), Q("item.move")}, {Q("itemId"), 5}}})});
    QCOMPARE(r.code, 1);
    QCOMPARE(r.json().value(Q("error")).toObject().value(Q("code")).toString(), Q("invalid_params"));
  }

  void callAndCommands() {
    Run r = run({Q("--json"), Q("call"), Q("engine.ping")});
    QCOMPARE(r.code, 0);
    QCOMPARE(r.json().value(Q("result")).toObject().value(Q("engine")).toString(), Q("splitframe"));
    r = run({Q("--json"), Q("call"), Q("timeline.get"), Q("{\"mode\":\"summary\"}"), Q("--project"), project_});
    QCOMPARE(r.code, 0);
    QCOMPARE(r.json().value(Q("result")).toObject().value(Q("project")).toObject().value(Q("name")).toString(), Q("CliFixture"));
    r = run({Q("--json"), Q("call"), Q("clip.add"), Q("{}")});
    QCOMPARE(r.code, 1);
    QCOMPARE(r.json().value(Q("error")).toObject().value(Q("code")).toString(), Q("invalid_params"));
    QCOMPARE(run({Q("call"), Q("clip.add"), Q("[1]")}).code, 2);
    QCOMPARE(run({Q("call")}).code, 2);
    r = run({Q("--json"), Q("call"), Q("nope.nothing")});
    QCOMPARE(r.json().value(Q("error")).toObject().value(Q("code")).toString(), Q("unknown_command"));
    r = run({Q("--json"), Q("commands")});
    QCOMPARE(r.code, 0);
    QVERIFY(r.json().value(Q("result")).toObject().value(Q("commands")).toArray().size() > 50);
    r = run({Q("commands"), Q("--domain"), Q("colour")});
    QVERIFY(r.out.contains(Q("item.color.set")) && !r.out.contains(Q("clip.add")));
  }

  void renderFrame() {
    const QString png = dir_.filePath(Q("f.png"));
    Run r = run({Q("render-frame"), project_, Q("--frame"), Q("3"), Q("--out"), png});
    if (r.code == 1 && r.err.contains(Q("renderer"))) QSKIP("no D3D11 device for the offscreen renderer");
    QVERIFY2(r.code == 0, qPrintable(r.out + r.err));
    QCOMPARE(QImage(png).size(), QSize(320, 180));
    r = run({Q("--json"), Q("render-frame"), project_, Q("--frame"), Q("3"), Q("--out"), dir_.filePath(Q("f2.png")), Q("--max-width"), Q("160")});
    QCOMPARE(r.code, 0);
    QCOMPARE(r.json().value(Q("result")).toObject().value(Q("width")).toInt(), 160);
  }

  void exportReusesTheExportCli() {
    const QString out = dir_.filePath(Q("cli.mp4"));
    Run r = run({Q("export"), project_, Q("--out"), out, Q("--quality"), Q("draft"), Q("--range"), Q("0:10")});
    QVERIFY2(r.code == 0, qPrintable(r.out + r.err));
    QVERIFY2(r.out.contains(Q("plan container=mp4")) && r.out.contains(Q("done path=")) && r.out.contains(Q("frames=10")), qPrintable(r.out));
    QVERIFY(QFileInfo(out).size() > 1000);
    QCOMPARE(run({Q("export"), project_, Q("--out"), out, Q("--codec"), Q("nope")}).code, 2);
    QCOMPARE(run({Q("export"), project_}).code, 2);
    QCOMPARE(run({Q("export")}).code, 2);
    QVERIFY(run({Q("export"), project_, Q("--list-presets"), Q("--out"), out}).out.contains(Q("1080p")));
  }

  void serveOverHttp() {
    QProcess p;
    p.start(QStringLiteral(SF_CLI_EXE), {Q("serve"), project_, Q("--token-file"), dir_.filePath(Q("serve-token"))});
    QVERIFY(p.waitForStarted(30000));
    QByteArray out;
    QElapsedTimer t;
    t.start();
    while (!out.contains('\n') && t.elapsed() < 30000) {
      p.waitForReadyRead(200);
      out += p.readAllStandardOutput();
    }
    const QRegularExpressionMatch m = QRegularExpression(Q("listening http://127\\.0\\.0\\.1:(\\d+)")).match(QString::fromUtf8(out));
    QVERIFY2(m.hasMatch(), out.constData());
    const quint16 port = static_cast<quint16>(m.captured(1).toUInt());
    QFile tf(dir_.filePath(Q("serve-token")));
    QVERIFY(tf.open(QIODevice::ReadOnly));
    const QByteArray token = tf.readAll();
    auto post = [&](const QByteArray& auth) {
      QTcpSocket s;
      s.connectToHost(QHostAddress::LocalHost, port);
      if (!s.waitForConnected(5000)) return QByteArray();
      const QByteArray body = R"({"jsonrpc":"2.0","method":"project.info","id":1})";
      s.write("POST /rpc HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) + "\r\n" + auth + "Connection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
      QByteArray resp;
      while (s.state() != QAbstractSocket::UnconnectedState || s.bytesAvailable()) {
        s.waitForReadyRead(2000);
        resp += s.readAll();
        if (s.state() == QAbstractSocket::UnconnectedState && !s.bytesAvailable()) break;
      }
      return resp;
    };
    QVERIFY(post({}).startsWith("HTTP/1.1 401"));
    const QByteArray ok = post("Authorization: Bearer " + token + "\r\n");
    QVERIFY2(ok.startsWith("HTTP/1.1 200") && ok.contains("CliFixture"), ok.constData());
    p.kill();
    p.waitForFinished(10000);
  }

  void serveOverStdio() {
    QProcess p;
    p.start(QStringLiteral(SF_CLI_EXE), {Q("serve"), Q("--stdio"), project_});
    QVERIFY(p.waitForStarted(30000));
    p.write("{\"jsonrpc\":\"2.0\",\"method\":\"project.info\",\"id\":9}\n");
    QByteArray out;
    QElapsedTimer t;
    t.start();
    while (!out.contains('\n') && t.elapsed() < 30000) {
      p.waitForReadyRead(200);
      out += p.readAllStandardOutput();
    }
    const QJsonObject resp = QJsonDocument::fromJson(out.left(static_cast<int>(out.indexOf('\n')))).object();
    QCOMPARE(resp.value(Q("id")).toInt(), 9);
    QCOMPARE(resp.value(Q("result")).toObject().value(Q("name")).toString(), Q("CliFixture"));
    p.closeWriteChannel();
    QVERIFY(p.waitForFinished(15000));
    QCOMPARE(p.exitCode(), 0);
  }
};

QTEST_GUILESS_MAIN(TstCli)
#include "tst_cli.moc"
