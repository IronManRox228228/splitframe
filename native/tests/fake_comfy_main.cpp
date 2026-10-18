#include "comfy_fake_server.h"

#include <QCoreApplication>
#include <QProcessEnvironment>
#include <QTimer>
#include <iostream>

int main(int argc, char* argv[]) {
  QCoreApplication app(argc, argv);

  quint16 port = 0;
  for (int i = 1; i < argc; ++i) {
    if (QString::fromLatin1(argv[i]) == QLatin1String("--port") && i + 1 < argc) {
      port = static_cast<quint16>(QString::fromLatin1(argv[i + 1]).toUShort());
      break;
    }
  }

  FakeComfyServer server(port);
  if (port == 0) {
    port = server.port();
  }

  const auto env = QProcessEnvironment::systemEnvironment();
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
