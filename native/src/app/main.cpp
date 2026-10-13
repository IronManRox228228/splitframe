#include "ai/ai_services.h"
#include "app/editor.h"
#include "app/player.h"
#include "app/pool_image_provider.h"
#include "app/window_recorder.h"
#include "engine/headless.h"
#include "export/export_cli.h"
#include "media/gpu_frame.h"
#include "media/probe.h"

#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickGraphicsDevice>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>

#include <algorithm>
#include <memory>

int main(int argc, char* argv[]) {
  // Headless export (export/export_cli.h): no window, no QML, progress on stdout, exit code = result.
  for (int i = 1; i < argc; ++i) {
    const QByteArray arg(argv[i]);
    if (arg == "--export" || arg.startsWith("--export=")) {
      QGuiApplication cliApp(argc, argv);
      QGuiApplication::setApplicationName(QStringLiteral("SplitFrame"));
      QGuiApplication::setOrganizationName(QStringLiteral("SplitFrame"));
      return sf::xport::runExportCli(QCoreApplication::arguments());
    }
  }
  // MCP server on stdio (engine/mcp.h): `splitframe.exe --mcp [project.json]`. No window; stdout carries only protocol messages.
  for (int i = 1; i < argc; ++i) {
    if (QByteArray(argv[i]) == "--mcp") {
      QGuiApplication mcpApp(argc, argv);
      QGuiApplication::setApplicationName(QStringLiteral("SplitFrame"));
      QGuiApplication::setOrganizationName(QStringLiteral("SplitFrame"));
      QGuiApplication::setApplicationVersion(QStringLiteral(PROJECT_VERSION_STRING));
      QString project;
      sf::ai::AiOptions ai; // SF_LLM_* from the environment, --llm-endpoint / --llm-model override
      for (int j = 1; j < argc; ++j) {
        const QByteArray a(argv[j]);
        if (a == "--llm-endpoint" && j + 1 < argc) ai.llm.endpoint = QString::fromLocal8Bit(argv[++j]);
        else if (a == "--llm-model" && j + 1 < argc) ai.llm.model = QString::fromLocal8Bit(argv[++j]);
        else if (a == "--llm-mock-script" && j + 1 < argc) ai.llm.mockScript = QString::fromLocal8Bit(argv[++j]);
        else if (!a.startsWith("--")) project = QString::fromLocal8Bit(argv[j]);
      }
      return sf::engine::runMcp(project, sf::ai::engineSetup(ai));
    }
  }
  // The compositor samples decoded D3D11 surfaces directly, so Qt Quick has to run on D3D11
  if (qEnvironmentVariableIsEmpty("QSG_RHI_BACKEND")) QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
  QQuickStyle::setStyle(QStringLiteral("Basic")); // the one style that follows our dark palette everywhere

  QGuiApplication app(argc, argv);
  QGuiApplication::setApplicationName(QStringLiteral("SplitFrame"));
  QGuiApplication::setOrganizationName(QStringLiteral("SplitFrame"));
  QGuiApplication::setApplicationVersion(QStringLiteral(PROJECT_VERSION_STRING));

  // Geist / Geist Mono (SIL OFL 1.1, shipped as variable woff2). Where the platform's font engine will not take them
  // the app falls back to Segoe UI / Consolas, which is what the substitutions below are for.
  const int geist = QFontDatabase::addApplicationFont(QStringLiteral(":/qt/qml/SplitFrame/fonts/Geist-Variable.woff2"));
  const int geistMono = QFontDatabase::addApplicationFont(QStringLiteral(":/qt/qml/SplitFrame/fonts/GeistMono-Variable.woff2"));
  if (geist < 0 || geistMono < 0) qInfo("Geist fonts not accepted (%d, %d): using Segoe UI and Consolas", geist, geistMono);
  QFont::insertSubstitution(QStringLiteral("Geist"), QStringLiteral("Segoe UI"));
  QFont::insertSubstitution(QStringLiteral("Geist Mono"), QStringLiteral("Consolas"));
  QGuiApplication::setFont(QFont(QStringLiteral("Geist")));

  QCommandLineParser cli;
  cli.addHelpOption();
  cli.addPositionalArgument(QStringLiteral("media"), QStringLiteral("A project .json, or a video, image or audio file to open as a one-clip project"));
  const QCommandLineOption screenshotOpt(QStringLiteral("screenshot"),
                                         QStringLiteral("Save the window to <png> once the frame has rendered, then quit."),
                                         QStringLiteral("png"));
  const QCommandLineOption frameOpt(QStringLiteral("frame"), QStringLiteral("Park the playhead on frame <n> (deterministic screenshots)."),
                                    QStringLiteral("n"));
  const QCommandLineOption cpuOpt(QStringLiteral("cpu"), QStringLiteral("Disable the zero-copy GPU video path (decode, download, upload)."));
  const QCommandLineOption swOpt(QStringLiteral("sw"), QStringLiteral("Software decoding only: no hardware decoder sessions."));
  const QCommandLineOption playOpt(QStringLiteral("play"), QStringLiteral("Play from the start for <seconds>, print the pacing numbers to stderr and quit."),
                                   QStringLiteral("seconds"));
  const QCommandLineOption settingsOpt(QStringLiteral("settings"), QStringLiteral("Keep recent files and options in this .ini instead of the registry."), QStringLiteral("ini"));
  const QCommandLineOption recoveryOpt(QStringLiteral("recovery-dir"), QStringLiteral("Where autosaves go (default: the app's local data folder)."), QStringLiteral("dir"));
  const QCommandLineOption selectOpt(QStringLiteral("select"), QStringLiteral("Select clips by index in timeline order, comma separated (screenshots)."), QStringLiteral("indices"));
  const QCommandLineOption fitOpt(QStringLiteral("fit"), QStringLiteral("Zoom the timeline to fit the project."));
  const QCommandLineOption apiOpt(QStringLiteral("api"), QStringLiteral("Start the local JSON-RPC / SSE API on 127.0.0.1 for the open project (also in the File menu)."));
  const QCommandLineOption apiPortOpt(QStringLiteral("api-port"), QStringLiteral("Port for --api (default: any free port)."), QStringLiteral("port"));
  const QCommandLineOption pageOpt(QStringLiteral("page"), QStringLiteral("Open on this page: edit, colour or audio."), QStringLiteral("page"));
  const QCommandLineOption recordDirOpt(QStringList{QStringLiteral("record"), QStringLiteral("record-dir")}, QStringLiteral("Directory to write recorded window PNG frames into (create it if missing)."), QStringLiteral("dir"));
  const QCommandLineOption recordFpsOpt(QStringLiteral("record-fps"), QStringLiteral("Recording framerate in fps (default 30, clamp to 1..60)."), QStringLiteral("n"));
  const QCommandLineOption recordSecOpt(QStringList{QStringLiteral("record-seconds"), QStringLiteral("record-sec")}, QStringLiteral("Duration in seconds to record (default 0 = record until the app quits)."), QStringLiteral("s"));
  const QCommandLineOption recordDelayOpt(QStringLiteral("record-delay"), QStringLiteral("Seconds to wait after the window first shows before recording starts (default 0)."), QStringLiteral("s"));
  const QCommandLineOption llmEndpointOpt(QStringLiteral("llm-endpoint"), QStringLiteral("Chat-completions URL for the agent commands (default: $SF_LLM_ENDPOINT or http://127.0.0.1:8080/v1/chat/completions; \"mock\" for offline)."), QStringLiteral("url"));
  const QCommandLineOption llmModelOpt(QStringLiteral("llm-model"), QStringLiteral("Model name for the agent commands (default: $SF_LLM_MODEL)."), QStringLiteral("name"));
  for (const auto* o : {&apiOpt, &apiPortOpt, &pageOpt, &llmEndpointOpt, &llmModelOpt}) cli.addOption(*o);
  for (const auto* o : {&screenshotOpt, &frameOpt, &cpuOpt, &swOpt, &playOpt, &settingsOpt, &recoveryOpt, &selectOpt, &fitOpt,
                        &recordDirOpt, &recordFpsOpt, &recordSecOpt, &recordDelayOpt}) cli.addOption(*o);
  cli.process(app);

  sf::app::Player player;
  player.setZeroCopy(!cli.isSet(cpuOpt));
  player.setSoftwareDecode(cli.isSet(swOpt));

  sf::app::Editor::Options options;
  options.settingsFile = cli.value(settingsOpt);
  options.recoveryDir = cli.value(recoveryOpt);
  if (cli.isSet(llmEndpointOpt)) options.ai.llm.endpoint = cli.value(llmEndpointOpt);
  if (cli.isSet(llmModelOpt)) options.ai.llm.model = cli.value(llmModelOpt);
  sf::app::Editor editor(player, options);
  if (cli.isSet(apiPortOpt)) editor.setApiPort(static_cast<quint16>(cli.value(apiPortOpt).toUInt()));
  if (cli.isSet(apiOpt)) editor.setApiEnabled(true);

  QQmlApplicationEngine engine;
  engine.addImageProvider(QStringLiteral("pool"), new sf::app::PoolImageProvider(editor.pool()));
  engine.rootContext()->setContextProperty(QStringLiteral("appPlayer"), &player);
  engine.rootContext()->setContextProperty(QStringLiteral("appEditor"), &editor);
  engine.rootContext()->setContextProperty(QStringLiteral("appOptions"),
                                           QVariantMap{{QStringLiteral("fit"), cli.isSet(fitOpt)}, {QStringLiteral("page"), cli.value(pageOpt)}, {QStringLiteral("startup"), cli.positionalArguments().isEmpty()}});
  QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
                   Qt::QueuedConnection);
  engine.loadFromModule(QStringLiteral("SplitFrame"), QStringLiteral("Main"));
  if (engine.rootObjects().isEmpty()) return 1;
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
  if (!window) return 1;

  if (cli.isSet(recordDirOpt)) {
    sf::app::WindowRecorder::Options recOpt;
    recOpt.dir = cli.value(recordDirOpt);
    if (cli.isSet(recordFpsOpt)) {
      bool ok = false;
      const int fps = cli.value(recordFpsOpt).toInt(&ok);
      if (ok) recOpt.fps = std::clamp(fps, 1, 60);
    }
    if (cli.isSet(recordSecOpt)) {
      bool ok = false;
      const double sec = cli.value(recordSecOpt).toDouble(&ok);
      if (ok && sec > 0.0) recOpt.sec = sec;
    }
    if (cli.isSet(recordDelayOpt)) {
      bool ok = false;
      const double delay = cli.value(recordDelayOpt).toDouble(&ok);
      if (ok && delay > 0.0) recOpt.delaySec = delay;
    }
    auto* recorder = new sf::app::WindowRecorder(window, recOpt, window);
    recorder->start();
  }

  // Put the window's device on the high-performance GPU, where the decoders will run too.
  if (const auto gpu = sf::preferredGpuAdapter()) window->setGraphicsDevice(QQuickGraphicsDevice::fromAdapter(gpu->luidLow, gpu->luidHigh));

  // Qt Quick creates the window's D3D11 device when the scene graph starts. Decoders and compositor
  // must run on that very device, so nothing opens (no decoder exists yet) until it is known.
  const QString target = cli.positionalArguments().value(0);
  auto opened = std::make_shared<bool>(target.isEmpty());
  QObject::connect(
      window, &QQuickWindow::sceneGraphInitialized, &app,
      [&player, &editor, window, target, opened, frame = cli.value(frameOpt), hasFrame = cli.isSet(frameOpt), play = cli.value(playOpt),
       select = cli.value(selectOpt)] {
        QSGRendererInterface* ri = window->rendererInterface();
        void* device = ri->getResource(window, QSGRendererInterface::DeviceResource);
        void* context = ri->getResource(window, QSGRendererInterface::DeviceContextResource);
        if (!sf::adoptD3D11Device(device, context)) qWarning("Zero-copy video unavailable: could not adopt Qt's D3D11 device");
        if (!target.isEmpty()) {
          if (editor.openPath(target)) {
            if (hasFrame) player.seek(frame.toLongLong());
            if (!select.isEmpty()) {
              QStringList ids;
              const auto& rows = editor.timeline()->rows();
              for (const QString& s : select.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                const auto i = static_cast<size_t>(s.toInt());
                if (i < rows.size()) ids << rows[i].id;
              }
              editor.project()->setSelection(ids);
            }
          }
          *opened = true;
          if (!play.isEmpty() && player.loaded()) {
            // pacing check from the command line: how many frames a real-time run shows vs skips
            const double seconds = play.toDouble();
            player.play();
            QTimer::singleShot(static_cast<int>(seconds * 1000), &player, [&player, seconds] {
              const qint64 shown = player.presentedFrames();
              const qint64 dropped = player.droppedFrames();
              qWarning().noquote() << QStringLiteral("play: %1 s, %2 presented (%3/s), %4 dropped, playhead at frame %5 (expected %6), %7")
                                          .arg(seconds).arg(shown).arg(static_cast<double>(shown) / seconds, 0, 'f', 1).arg(dropped)
                                          .arg(player.frame()).arg(static_cast<qint64>(seconds * player.fps())).arg(player.stats());
              QCoreApplication::quit();
            });
          }
        }
      },
      Qt::QueuedConnection);
  window->show();

  // headless visual checks: grab the window's own pixels (not the screen, which may show other windows)
  if (cli.isSet(screenshotOpt)) {
    const QString out = cli.value(screenshotOpt);
    auto* timer = new QTimer(window);
    struct Wait {
      QElapsedTimer clock;
      int steady = 0;
    };
    auto wait = std::make_shared<Wait>();
    wait->clock.start();
    timer->setInterval(50);
    // the Audio page has no preview, so no frame is ever composited for it
    const bool wantFrame = cli.value(pageOpt) != QLatin1String("audio");
    QObject::connect(timer, &QTimer::timeout, window, [wait, window, out, opened, wantFrame, &player, &editor] {
      // wait for the playhead's frame to be fully composited (decoders caught up) and the thumbnails to
      // exist, then a few frames more so the window has presented it; give up after a while and shoot what is there
      const bool ready = *opened && editor.pool()->pending() == 0 && player.loaded() && (!wantFrame || player.settled());
      wait->steady = ready ? wait->steady + 1 : 0;
      const bool timedOut = wait->clock.elapsed() > 20000;
      if (wait->steady >= 6 || timedOut) {
        const bool ok = window->grabWindow().save(out);
        if (timedOut) qWarning().noquote() << "screenshot: frame was not fully decoded after 20 s;" << player.stats() << "pending" << editor.pool()->pending() << "frame" << player.frame();
        QCoreApplication::exit(ok ? (timedOut ? 3 : 0) : 2);
      }
    });
    timer->start();
  }
  return QGuiApplication::exec();
}
