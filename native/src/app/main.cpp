#include "app/editor.h"
#include "app/player.h"
#include "app/pool_image_provider.h"
#include "export/export_cli.h"
#include "media/gpu_frame.h"
#include "media/probe.h"

#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickGraphicsDevice>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>

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
  // The compositor samples decoded D3D11 surfaces directly, so Qt Quick has to run on D3D11
  if (qEnvironmentVariableIsEmpty("QSG_RHI_BACKEND")) QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
  QQuickStyle::setStyle(QStringLiteral("Basic")); // the one style that follows our dark palette everywhere

  QGuiApplication app(argc, argv);
  QGuiApplication::setApplicationName(QStringLiteral("SplitFrame"));
  QGuiApplication::setOrganizationName(QStringLiteral("SplitFrame"));
  QGuiApplication::setApplicationVersion(QStringLiteral(PROJECT_VERSION_STRING));

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
  for (const auto* o : {&screenshotOpt, &frameOpt, &cpuOpt, &swOpt, &playOpt, &settingsOpt, &recoveryOpt, &selectOpt, &fitOpt}) cli.addOption(*o);
  cli.process(app);

  sf::app::Player player;
  player.setZeroCopy(!cli.isSet(cpuOpt));
  player.setSoftwareDecode(cli.isSet(swOpt));

  sf::app::Editor::Options options;
  options.settingsFile = cli.value(settingsOpt);
  options.recoveryDir = cli.value(recoveryOpt);
  sf::app::Editor editor(player, options);

  QQmlApplicationEngine engine;
  engine.addImageProvider(QStringLiteral("pool"), new sf::app::PoolImageProvider(editor.pool()));
  engine.rootContext()->setContextProperty(QStringLiteral("appPlayer"), &player);
  engine.rootContext()->setContextProperty(QStringLiteral("appEditor"), &editor);
  engine.rootContext()->setContextProperty(QStringLiteral("appOptions"),
                                           QVariantMap{{QStringLiteral("fit"), cli.isSet(fitOpt)}, {QStringLiteral("startup"), cli.positionalArguments().isEmpty()}});
  QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
                   Qt::QueuedConnection);
  engine.loadFromModule(QStringLiteral("SplitFrame"), QStringLiteral("Main"));
  if (engine.rootObjects().isEmpty()) return 1;
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
  if (!window) return 1;

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
    QObject::connect(timer, &QTimer::timeout, window, [wait, window, out, opened, &player, &editor] {
      // wait for the playhead's frame to be fully composited (decoders caught up) and the thumbnails to
      // exist, then a few frames more so the window has presented it; give up after a while and shoot what is there
      const bool ready = *opened && editor.pool()->pending() == 0 && player.loaded() && player.settled();
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
