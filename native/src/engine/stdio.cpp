#include "engine/stdio.h"

#include <QCoreApplication>
#include <QPointer>

#include <cstdio>
#include <mutex>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace sf::engine {

struct StdioTransport::Shared {
  std::mutex writeMutex;
  std::atomic<bool> alive{true};
};

namespace {

#ifdef Q_OS_WIN
HANDLE outHandle() {
  static HANDLE h = [] {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE) {
      if (AttachConsole(ATTACH_PARENT_PROCESS)) out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    return out;
  }();
  return h;
}
#endif

} // namespace

StdioTransport::StdioTransport(QObject* parent) : QObject(parent), shared_(std::make_shared<Shared>()) {}

StdioTransport::~StdioTransport() { shared_->alive = false; }

void StdioTransport::start() {
  QPointer<StdioTransport> self(this);
  std::shared_ptr<Shared> shared = shared_;
  // The thread blocks in a read the process cannot interrupt: detach it, it ends with the process.
  std::thread([self, shared] {
    QByteArray pending;
    char chunk[16384];
    auto deliver = [&](const QByteArray& line) {
      QMetaObject::invokeMethod(QCoreApplication::instance(), [self, shared, line] {
        if (self && shared->alive) emit self->lineReceived(line);
      }, Qt::QueuedConnection);
    };
#ifdef Q_OS_WIN
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    for (;;) {
      DWORD got = 0;
      if (in == nullptr || in == INVALID_HANDLE_VALUE || !ReadFile(in, chunk, sizeof chunk, &got, nullptr) || got == 0) break;
      pending.append(chunk, static_cast<qsizetype>(got));
#else
    for (;;) {
      const size_t got = std::fread(chunk, 1, 1, stdin);
      if (got == 0) break;
      pending.append(chunk, 1);
#endif
      qsizetype nl;
      while ((nl = pending.indexOf('\n')) >= 0) {
        QByteArray line = pending.left(nl);
        pending.remove(0, nl + 1);
        if (line.endsWith('\r')) line.chop(1);
        if (!line.trimmed().isEmpty()) deliver(line);
      }
    }
    if (!pending.trimmed().isEmpty()) deliver(pending);
    QMetaObject::invokeMethod(QCoreApplication::instance(), [self, shared] {
      if (self && shared->alive) emit self->closed();
    }, Qt::QueuedConnection);
  }).detach();
}

void StdioTransport::send(const QByteArray& message) {
  const QByteArray bytes = message + '\n';
  std::lock_guard<std::mutex> lock(shared_->writeMutex);
#ifdef Q_OS_WIN
  HANDLE h = outHandle();
  if (h && h != INVALID_HANDLE_VALUE) {
    qsizetype off = 0;
    while (off < bytes.size()) {
      DWORD written = 0;
      if (!WriteFile(h, bytes.constData() + off, static_cast<DWORD>(bytes.size() - off), &written, nullptr) || written == 0) return;
      off += written;
    }
    return;
  }
#endif
  std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
  std::fflush(stdout);
}

} // namespace sf::engine
