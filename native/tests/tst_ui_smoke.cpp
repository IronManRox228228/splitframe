// End to end: start the real app on a generated multi-track project, let it render, and check that
// it exits cleanly with a screenshot and no QML warnings. Uses software decoding (no hardware
// decoder sessions) but the window itself needs the GPU path the app always runs on (D3D11).
// With SF_SHOT_DIR set, the screenshots are kept there.

#include "editor_testutil.h"

#include <QProcess>

using namespace sf::test;

class TstUiSmoke : public QObject {
  Q_OBJECT

  QTemporaryDir dir_;
  QString project_;

  static QString shotDir(const QTemporaryDir& fallback) {
    const QString d = qEnvironmentVariable("SF_SHOT_DIR");
    return d.isEmpty() ? fallback.path() : d;
  }

  struct Run {
    int exitCode = -1;
    QString err;
  };
  Run runApp(const QStringList& extra, const QString& shot) {
    QProcess p;
    p.setProcessChannelMode(QProcess::SeparateChannels);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("QT_ASSUME_STDERR_HAS_CONSOLE"), QStringLiteral("1"));
    p.setProcessEnvironment(env);
    QStringList args{QStringLiteral("--sw"), QStringLiteral("--settings"), dir_.filePath(QStringLiteral("settings.ini")), QStringLiteral("--recovery-dir"),
                     dir_.filePath(QStringLiteral("rec")), QStringLiteral("--screenshot"), shot};
    args += extra;
    p.start(QStringLiteral(SF_APP_EXE), args);
    Run r;
    if (!p.waitForFinished(90000)) {
      p.kill();
      p.waitForFinished();
      r.err = QStringLiteral("timed out");
      return r;
    }
    r.exitCode = p.exitCode();
    r.err = QString::fromUtf8(p.readAllStandardError());
    return r;
  }

  // QML problems show up as "qrc:/...: ..." lines or as binding/type errors
  static void expectNoQmlWarnings(const QString& err) {
    QStringList bad;
    for (const QString& line : err.split(QLatin1Char('\n'))) {
      if (line.contains(QStringLiteral("qrc:")) || line.contains(QStringLiteral("TypeError")) || line.contains(QStringLiteral("ReferenceError")) ||
          line.contains(QStringLiteral("is not a type")) || line.contains(QStringLiteral("Binding loop")) || line.contains(QStringLiteral("QML ")) ||
          line.contains(QStringLiteral("Unable to assign")))
        bad << line;
    }
    QVERIFY2(bad.isEmpty(), qPrintable(bad.join(QLatin1Char('\n'))));
  }

private slots:
  void initTestCase() {
    QVERIFY(dir_.isValid());
    if (ffmpegExe().isEmpty()) QSKIP("SF_FFMPEG_EXE is not set");
    project_ = writeDemoProject(dir_.path());
    QVERIFY2(!project_.isEmpty(), "could not generate the demo project");
  }

  void emptyProjectLoadsWithoutWarnings() {
    const QString shot = shotDir(dir_) + QStringLiteral("/ui_empty.png");
    const Run r = runApp({}, shot);
    QCOMPARE(r.exitCode, 0);
    expectNoQmlWarnings(r.err);
    QVERIFY(QImage(shot).width() > 800);
  }

  void multiTrackProjectLoadsAndRenders() {
    const QString shot = shotDir(dir_) + QStringLiteral("/ui_project.png");
    const Run r = runApp({QStringLiteral("--frame"), QStringLiteral("100"), project_}, shot);
    QVERIFY2(r.exitCode == 0, qPrintable(r.err));
    expectNoQmlWarnings(r.err);
    const QImage img(shot);
    QVERIFY(img.width() > 800 && img.height() > 600);
  }

  void selectedClipShowsInspector() {
    const QString shot = shotDir(dir_) + QStringLiteral("/ui_selected.png");
    const Run r = runApp({QStringLiteral("--frame"), QStringLiteral("170"), QStringLiteral("--select"), QStringLiteral("2"), QStringLiteral("--fit"), project_}, shot);
    QVERIFY2(r.exitCode == 0, qPrintable(r.err));
    expectNoQmlWarnings(r.err);
  }

  void mediaFileOpensAsOneClipProject() {
    const QString shot = shotDir(dir_) + QStringLiteral("/ui_media.png");
    const Run r = runApp({dir_.filePath(QStringLiteral("clip_a.mp4"))}, shot);
    QVERIFY2(r.exitCode == 0, qPrintable(r.err));
    expectNoQmlWarnings(r.err);
  }
};

QTEST_GUILESS_MAIN(TstUiSmoke)
#include "tst_ui_smoke.moc"
