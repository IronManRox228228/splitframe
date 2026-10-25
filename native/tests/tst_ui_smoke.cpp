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
  Run runApp(const QStringList& extra, const QString& shot, const QProcessEnvironment& more = {}) {
    QProcess p;
    p.setProcessChannelMode(QProcess::SeparateChannels);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("QT_ASSUME_STDERR_HAS_CONSOLE"), QStringLiteral("1"));
    env.insert(more);
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
    // SF_DEMO_DIR keeps the generated clips and project somewhere reusable (screenshots by hand)
    const QString demoDir = qEnvironmentVariable("SF_DEMO_DIR");
    if (!demoDir.isEmpty()) QDir().mkpath(demoDir);
    project_ = writeDemoProject(demoDir.isEmpty() ? dir_.path() : demoDir);
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

  // each page builds without QML errors, on an empty project and on the multi-track one
  void pagesLoadWithoutWarnings() {
    for (const QString page : {QStringLiteral("edit"), QStringLiteral("colour"), QStringLiteral("audio")}) {
      const QString shot = shotDir(dir_) + QStringLiteral("/ui_page_%1.png").arg(page);
      const Run r = runApp({QStringLiteral("--page"), page, QStringLiteral("--frame"), QStringLiteral("100"), QStringLiteral("--select"), QStringLiteral("1"), project_}, shot);
      QVERIFY2(r.exitCode == 0, qPrintable(page + ": " + r.err));
      expectNoQmlWarnings(r.err);
      QVERIFY2(QImage(shot).width() > 800, qPrintable(page));
      const Run empty = runApp({QStringLiteral("--page"), page}, shotDir(dir_) + QStringLiteral("/ui_page_%1_empty.png").arg(page));
      QVERIFY2(empty.exitCode == 0, qPrintable(page + " (empty): " + empty.err));
      expectNoQmlWarnings(empty.err);
    }
  }

  // the Colour page measures its scopes in the background and the screenshot waits for them: exit 0 (not 3, "timed out")
  // means a measurement came back, and the panel with the histogram built without QML warnings
  void colourPageMeasuresItsScopes() {
    const QString shot = shotDir(dir_) + QStringLiteral("/ui_colour_scopes.png");
    const Run r = runApp({QStringLiteral("--page"), QStringLiteral("colour"), QStringLiteral("--frame"), QStringLiteral("100"), QStringLiteral("--select"), QStringLiteral("1"), project_}, shot);
    QCOMPARE(r.exitCode, 0);
    expectNoQmlWarnings(r.err);
    QVERIFY(QImage(shot).width() > 800);
  }

  // ---- the glass shell: Home, the Assistant (a real crew run on the offline mock model), the command bar and the AI settings ----

  QString mockScript(const QString& json) {
    const QString path = dir_.filePath(QStringLiteral("mock_reply.json"));
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(json.toUtf8());
    return path;
  }

  void homeShowsRecentProjectsWithoutWarnings() {
    // opening the demo project remembers it; the next start (no file) opens on Home with its card
    const Run open = runApp({QStringLiteral("--frame"), QStringLiteral("100"), project_}, dir_.filePath(QStringLiteral("ignored.png")));
    QCOMPARE(open.exitCode, 0);
    const QString shot = shotDir(dir_) + QStringLiteral("/ui_home.png");
    const Run r = runApp({QStringLiteral("--screen"), QStringLiteral("home")}, shot);
    QVERIFY2(r.exitCode == 0, qPrintable(r.err));
    expectNoQmlWarnings(r.err);
    QVERIFY(QImage(shot).width() > 800);
    // an empty start with no file is Home too
    const Run empty = runApp({}, shotDir(dir_) + QStringLiteral("/ui_home_empty.png"));
    QVERIFY2(empty.exitCode == 0, qPrintable(empty.err));
    expectNoQmlWarnings(empty.err);
  }

  void assistantRunsTheCrewAndShowsItsEvents() {
    // first call: the Director reads the storyboard and the timeline; once their results are in the history, it answers
    const QString script = mockScript(QStringLiteral(
        "{\"scripted\":[{\"trigger\":\"storyboard\",\"response\":{\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"storyboard.get\",\"arguments\":\"{}\"}},"
        "{\"id\":\"c2\",\"type\":\"function\",\"function\":{\"name\":\"timeline.get\",\"arguments\":\"{}\"}},"
        "{\"id\":\"c3\",\"type\":\"function\",\"function\":{\"name\":\"no.such.command\",\"arguments\":\"{}\"}}]}},"
        "{\"trigger\":\"\\\"count\\\"\",\"response\":{\"content\":\"The project has **4 video clips** and no storyboard yet. Want me to draft one?\\n\\n- scenes from your clips\\n- a title card\"}}],"
        "\"default\":{\"content\":\"Done.\"}}"));
    QProcessEnvironment env;
    env.insert(QStringLiteral("SF_LLM_MOCK_SCRIPT"), script);
    const QString shot = shotDir(dir_) + QStringLiteral("/ui_assistant.png");
    const Run r = runApp({QStringLiteral("--llm-endpoint"), QStringLiteral("mock"), QStringLiteral("--assistant"), QStringLiteral("--ask"),
                          QStringLiteral("Look at the storyboard and tell me what you see"), QStringLiteral("--frame"), QStringLiteral("100"), project_},
                         shot, env);
    QVERIFY2(r.exitCode == 0, qPrintable(r.err));
    expectNoQmlWarnings(r.err);
    QVERIFY(QImage(shot).width() > 800);
  }

  void assistantReportsAnUnreachableModelPlainly() {
    QProcessEnvironment env;
    env.insert(QStringLiteral("SF_LLM_TIMEOUT_MS"), QStringLiteral("2000"));
    const QString shot = shotDir(dir_) + QStringLiteral("/ui_assistant_error.png");
    const Run r = runApp({QStringLiteral("--llm-endpoint"), QStringLiteral("http://127.0.0.1:9/v1/chat/completions"), QStringLiteral("--assistant"), QStringLiteral("--ask"),
                          QStringLiteral("Remove the silences"), QStringLiteral("--frame"), QStringLiteral("100"), project_},
                         shot, env);
    QVERIFY2(r.exitCode == 0, qPrintable(r.err));
    expectNoQmlWarnings(r.err);
  }

  void commandBarAndAiSettingsLoadWithoutWarnings() {
    const Run bar = runApp({QStringLiteral("--cmdbar"), QStringLiteral("--frame"), QStringLiteral("100"), project_}, shotDir(dir_) + QStringLiteral("/ui_commandbar.png"));
    QVERIFY2(bar.exitCode == 0, qPrintable(bar.err));
    expectNoQmlWarnings(bar.err);
    const Run settings = runApp({QStringLiteral("--ai-settings"), QStringLiteral("--assistant"), QStringLiteral("--frame"), QStringLiteral("100"), project_},
                                shotDir(dir_) + QStringLiteral("/ui_ai_settings.png"));
    QVERIFY2(settings.exitCode == 0, qPrintable(settings.err));
    expectNoQmlWarnings(settings.err);
  }

  void flatGlassFallbackLoadsWithoutWarnings() {
    const Run r = runApp({QStringLiteral("--glass"), QStringLiteral("flat"), QStringLiteral("--assistant"), QStringLiteral("--frame"), QStringLiteral("100"), project_},
                         shotDir(dir_) + QStringLiteral("/ui_flat.png"));
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
