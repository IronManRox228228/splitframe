#include "core/time.h"
#include "media/probe.h"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickImageProvider>
#include <QQuickWindow>
#include <QTimer>

namespace {

// image://frame/<ms>?<url-encoded path> -> that frame of that file
class FrameProvider final : public QQuickImageProvider {
public:
  FrameProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

  QImage requestImage(const QString& id, QSize* size, const QSize& requested) override {
    const qsizetype q = id.indexOf(u'?');
    if (q < 0) return {};
    const qint64 ms = id.left(q).toLongLong();
    const QString path = QUrl::fromPercentEncoding(id.mid(q + 1).toUtf8());
    QImage img = sf::decodeFrame(path, ms);
    if (size) *size = img.size();
    if (requested.isValid() && !img.isNull()) img = img.scaled(requested, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return img;
  }
};

} // namespace

int main(int argc, char* argv[]) {
  QGuiApplication app(argc, argv);
  QGuiApplication::setApplicationName(QStringLiteral("SplitFrame"));
  QGuiApplication::setApplicationVersion(QStringLiteral(PROJECT_VERSION_STRING));

  QCommandLineParser cli;
  cli.addHelpOption();
  cli.addPositionalArgument(QStringLiteral("media"), QStringLiteral("A video file to show"));
  const QCommandLineOption screenshotOpt(QStringLiteral("screenshot"),
                                         QStringLiteral("Save the window to <png> once it has rendered, then quit."),
                                         QStringLiteral("png"));
  cli.addOption(screenshotOpt);
  cli.process(app);

  QString mediaPath;
  QString mediaSummary = QStringLiteral("Drop a video file on the command line to preview it.");
  if (!cli.positionalArguments().isEmpty()) {
    mediaPath = cli.positionalArguments().first();
    QString error;
    if (const auto info = sf::probeMedia(mediaPath, &error)) {
      const double fps = info->fps > 0 ? info->fps : 30.0;
      mediaSummary = QStringLiteral("%1 · %2×%3 · %4 fps · %5")
                         .arg(info->videoCodec)
                         .arg(info->width)
                         .arg(info->height)
                         .arg(info->fps, 0, 'f', 2)
                         .arg(sf::formatTimecode(sf::msToFrames(info->durationMs, fps), fps));
    } else {
      mediaSummary = error;
      mediaPath.clear();
    }
  }

  QQmlApplicationEngine engine;
  engine.addImageProvider(QStringLiteral("frame"), new FrameProvider);
  engine.rootContext()->setContextProperty(QStringLiteral("ffmpegVersion"), sf::ffmpegVersion());
  engine.rootContext()->setContextProperty(QStringLiteral("mediaPath"), mediaPath);
  engine.rootContext()->setContextProperty(QStringLiteral("mediaSummary"), mediaSummary);
  QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
                   Qt::QueuedConnection);
  engine.loadFromModule(QStringLiteral("SplitFrame"), QStringLiteral("Main"));

  // headless visual checks: grab the window's own pixels (not the screen, which may show other windows)
  if (cli.isSet(screenshotOpt) && !engine.rootObjects().isEmpty()) {
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    const QString out = cli.value(screenshotOpt);
    QTimer::singleShot(2500, window, [window, out] {
      const bool ok = window->grabWindow().save(out);
      QCoreApplication::exit(ok ? 0 : 2);
    });
  }
  return QGuiApplication::exec();
}
