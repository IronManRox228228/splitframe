// splitframe.exe --export on a fixture project: exit codes, stdout protocol, the file it writes.

#include "core/schema.h"
#include "core/timeline_doc.h"
#include "media/probe.h"
#include "media_testutil.h"
#include "render_testutil.h"

#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

using namespace sf;
using namespace sf::test;

class TstExportCli : public QObject {
  Q_OBJECT

  QTemporaryDir dir_;
  QString project_, clip_;

  struct Run {
    int code = -1;
    QString out;
  };
  Run run(const QStringList& args) {
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(QStringLiteral(SF_APP_EXE), args);
    Run r;
    if (!p.waitForStarted(20000)) {
      r.out = QStringLiteral("could not start: ") + p.errorString();
      return r;
    }
    if (!p.waitForFinished(120000)) {
      p.kill();
      p.waitForFinished();
      r.out = QStringLiteral("timed out: ") + QString::fromUtf8(p.readAll());
      return r;
    }
    r.code = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -2;
    r.out = QString::fromUtf8(p.readAll());
    return r;
  }

private slots:
  void initTestCase() {
    QVERIFY(dir_.isValid());
    if (ffmpegExe().isEmpty()) QSKIP("SF_FFMPEG_EXE is not set");
    clip_ = dir_.filePath(QStringLiteral("clip.mp4"));
    QString log;
    QVERIFY2(runFfmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), indexSource(320, 180, 25, 2), QStringLiteral("-f"), QStringLiteral("lavfi"),
                        QStringLiteral("-i"), QStringLiteral("sine=frequency=440:sample_rate=48000:duration=2"), QStringLiteral("-c:v"), QStringLiteral("libx264"),
                        QStringLiteral("-preset"), QStringLiteral("ultrafast"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), QStringLiteral("-c:a"),
                        QStringLiteral("aac"), QStringLiteral("-shortest"), clip_}, &log), qPrintable(log));
    ProjectBundle b;
    b.doc = newDoc(320, 180, 25);
    addMedia(b.doc, ItemType::Video, mainTrack(b.doc), QStringLiteral("itm_a"), QStringLiteral("ast_a"), 0, 50);
    Asset a;
    a.id = QStringLiteral("ast_a");
    a.projectId = b.doc.project.id;
    a.kind = AssetKind::Video;
    a.path = QStringLiteral("clip.mp4"); // relative: resolved against the project file
    a.originalName = QStringLiteral("clip.mp4");
    a.status = AssetStatus::Analyzed;
    a.durationMs = 2000;
    a.width = 320;
    a.height = 180;
    a.fps = 25;
    a.hasAudio = true;
    a.createdAt = QStringLiteral("2026-01-01T00:00:00.000Z");
    b.assets.push_back(a);
    project_ = dir_.filePath(QStringLiteral("p.json"));
    saveProjectBundle(project_, b);
  }

  void exportsAFile() {
    const QString out = dir_.filePath(QStringLiteral("cli.mp4"));
    const Run r = run({QStringLiteral("--export"), project_, QStringLiteral("--out"), out, QStringLiteral("--quality"), QStringLiteral("draft")});
    QVERIFY2(r.code == 0, qPrintable(QStringLiteral("exit %1:\n%2").arg(r.code).arg(r.out)));
    QVERIFY2(r.out.contains(QStringLiteral("plan container=mp4 video=libx264 size=320x180 fps=25 frames=50")), qPrintable(r.out));
    QVERIFY2(r.out.contains(QStringLiteral("progress frame=")), qPrintable(r.out));
    const QStringList lines = r.out.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);
    QVERIFY2(lines.last().startsWith(QStringLiteral("done path=")) && lines.last().contains(QStringLiteral("frames=50")), qPrintable(r.out));
    const auto info = probeMedia(out, nullptr, ProbeOptions{.vfrScanLimit = 0});
    QVERIFY(info);
    QCOMPARE(info->frameCount, 50);
    QVERIFY(info->hasAudio);
  }

  void rangeAndPreset() {
    const QString out = dir_.filePath(QStringLiteral("cli_range.mov"));
    const Run r = run({QStringLiteral("--export"), project_, QStringLiteral("--out"), out, QStringLiteral("--codec"), QStringLiteral("prores"), QStringLiteral("--range"),
                       QStringLiteral("5:30"), QStringLiteral("--audio-codec"), QStringLiteral("pcm16")});
    QVERIFY2(r.code == 0, qPrintable(r.out));
    const auto info = probeMedia(out, nullptr, ProbeOptions{.vfrScanLimit = 0});
    QVERIFY(info);
    QCOMPARE(info->frameCount, 25);
    QCOMPARE(info->videoCodec, QStringLiteral("prores"));
  }

  void audioOnly() {
    const QString out = dir_.filePath(QStringLiteral("cli.wav"));
    const Run r = run({QStringLiteral("--export"), project_, QStringLiteral("--out"), out, QStringLiteral("--lufs"), QStringLiteral("-16")});
    QVERIFY2(r.code == 0, qPrintable(r.out));
    QVERIFY2(r.out.contains(QStringLiteral("gain_db=")), qPrintable(r.out));
    QVERIFY(QFileInfo(out).size() > 100000);
  }

  void failuresHaveExitCodes() {
    const QString out = dir_.filePath(QStringLiteral("cli.mp4"));
    // exists already and no --overwrite
    Run r = run({QStringLiteral("--export"), project_, QStringLiteral("--out"), out});
    QCOMPARE(r.code, 1);
    QVERIFY2(r.out.contains(QStringLiteral("error: ")) && r.out.contains(QStringLiteral("already exists")), qPrintable(r.out));
    // bad arguments / unreadable project
    r = run({QStringLiteral("--export"), dir_.filePath(QStringLiteral("nope.json")), QStringLiteral("--out"), out});
    QCOMPARE(r.code, 2);
    QVERIFY(r.out.contains(QStringLiteral("error: ")));
    r = run({QStringLiteral("--export"), project_});
    QCOMPARE(r.code, 2);
    r = run({QStringLiteral("--export"), project_, QStringLiteral("--out"), out, QStringLiteral("--preset"), QStringLiteral("bogus"), QStringLiteral("--overwrite")});
    QCOMPARE(r.code, 2);
    QVERIFY(r.out.contains(QStringLiteral("unknown preset")));
    r = run({QStringLiteral("--export"), project_, QStringLiteral("--out"), dir_.filePath(QStringLiteral("x.mp4")), QStringLiteral("--range"), QStringLiteral("40:40")});
    QCOMPARE(r.code, 2);
    QVERIFY(!QFileInfo::exists(dir_.filePath(QStringLiteral("x.mp4"))));
  }

  void listsPresets() {
    const Run r = run({QStringLiteral("--export"), project_, QStringLiteral("--list-presets")});
    QCOMPARE(r.code, 0);
    QVERIFY(r.out.contains(QStringLiteral("720p")));
    QVERIFY(r.out.contains(QStringLiteral("TikTok")));
  }
};

QTEST_GUILESS_MAIN(TstExportCli)
#include "tst_export_cli.moc"
