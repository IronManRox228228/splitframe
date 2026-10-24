#include "comfy_fake_server.h"

#include <QCoreApplication>
#include <QProcessEnvironment>
#include <QTimer>
#include <iostream>

int main(int argc, char* argv[]) {
  QCoreApplication app(argc, argv);

  quint16 port = 0;
  QString objectInfoPath;
  QString outputKind;
  QString outputFilename;

  for (int i = 1; i < argc; ++i) {
    const QString arg = QString::fromLatin1(argv[i]);
    if (arg == QLatin1String("--port") && i + 1 < argc) {
      port = static_cast<quint16>(QString::fromLatin1(argv[++i]).toUShort());
    } else if (arg == QLatin1String("--object-info") && i + 1 < argc) {
      objectInfoPath = QString::fromLatin1(argv[++i]);
    } else if (arg == QLatin1String("--output-kind") && i + 1 < argc) {
      outputKind = QString::fromLatin1(argv[++i]);
    } else if (arg == QLatin1String("--output-filename") && i + 1 < argc) {
      outputFilename = QString::fromLatin1(argv[++i]);
    }
  }

  FakeComfyServer server(port);
  if (port == 0) {
    port = server.port();
  }

  const auto env = QProcessEnvironment::systemEnvironment();
  if (objectInfoPath.isEmpty()) {
    objectInfoPath = env.value(QStringLiteral("FAKE_COMFY_OBJECT_INFO"));
  }
  if (!objectInfoPath.isEmpty()) {
    server.setObjectInfoPath(objectInfoPath);
  }
  if (outputKind.isEmpty()) {
    outputKind = env.value(QStringLiteral("FAKE_COMFY_OUTPUT_KIND"));
  }
  if (!outputKind.isEmpty()) {
    server.setOutputKind(outputKind);
  }
  if (outputFilename.isEmpty()) {
    outputFilename = env.value(QStringLiteral("FAKE_COMFY_OUTPUT_FILENAME"));
  }
  if (!outputFilename.isEmpty()) {
    server.setOutputFilename(outputFilename);
  }

  if (env.value(QStringLiteral("FAKE_COMFY_HANG_HISTORY")) == QLatin1String("1")) {
    server.setHangHistory(true);
  }

  std::cout << "To see the GUI go to: http://127.0.0.1:" << port << std::endl;

  if (env.contains(QStringLiteral("FAKE_COMFY_CRASH_AFTER_MS"))) {
    const int crashMs = env.value(QStringLiteral("FAKE_COMFY_CRASH_AFTER_MS")).toInt();
    QTimer::singleShot(crashMs, []() {
      std::exit(3);
    });
  }

  return app.exec();
}
