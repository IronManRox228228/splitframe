#include "media/ffmpeg.h"

namespace sf::av {

FormatPtr openInput(const QString& path, QString* error) {
  AVFormatContext* raw = nullptr;
  const QByteArray utf8 = path.toUtf8();
  if (const int rc = avformat_open_input(&raw, utf8.constData(), nullptr, nullptr); rc < 0) {
    if (error) *error = QStringLiteral("Can't open %1: %2").arg(path, errorString(rc));
    return nullptr;
  }
  FormatPtr fmt(raw);
  if (const int rc = avformat_find_stream_info(fmt.get(), nullptr); rc < 0) {
    if (error) *error = QStringLiteral("Can't read streams of %1: %2").arg(path, errorString(rc));
    return nullptr;
  }
  return fmt;
}

} // namespace sf::av
