#pragma once

// image://projthumb/<percent-encoded project file path>: a picture for a project's Home card. The first video or image on the
// project's timeline (else its first asset), decoded as one keyframe. Qt calls requestImage on its own thread; results are cached
// for the session, keyed by path and modification time.

#include "core/schema.h"
#include "media/thumbnails.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QMutex>
#include <QQuickImageProvider>
#include <QUrl>

#include <algorithm>
#include <map>

namespace sf::app {

class ProjectThumbProvider : public QQuickImageProvider {
public:
  ProjectThumbProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

  QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override {
    const QString path = QUrl::fromPercentEncoding(id.section(QLatin1Char('?'), 0, 0).toUtf8());
    const QString key = path + QLatin1Char('|') + QString::number(QFileInfo(path).lastModified().toMSecsSinceEpoch());
    QImage out;
    {
      QMutexLocker lock(&mutex_);
      const auto it = cache_.find(key);
      if (it != cache_.end()) out = it->second;
    }
    if (out.isNull()) {
      out = load(path);
      QMutexLocker lock(&mutex_);
      cache_[key] = out;
    }
    if (!out.isNull() && requestedSize.isValid() && requestedSize.width() > 0 && requestedSize.height() > 0 && out.size() != requestedSize)
      out = out.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (size) *size = out.size();
    return out;
  }

private:
  static QImage load(const QString& projectPath) {
    try {
      const ProjectBundle b = loadProjectBundle(projectPath);
      const QDir dir = QFileInfo(projectPath).absoluteDir();
      const auto assetPath = [&](const Asset& a) { return QFileInfo(a.path).isRelative() ? dir.filePath(a.path) : a.path; };
      const Asset* pick = nullptr;
      // earliest item on the timeline that has a visual asset
      const Item* firstItem = nullptr;
      for (const Item& it : b.doc.items) {
        if (!it.assetId) continue;
        const auto a = std::find_if(b.assets.begin(), b.assets.end(), [&](const Asset& x) { return x.id == *it.assetId; });
        if (a == b.assets.end() || a->kind == AssetKind::Audio) continue;
        if (!firstItem || it.startFrame < firstItem->startFrame) {
          firstItem = &it;
          pick = &*a;
        }
      }
      if (!pick) {
        const auto a = std::find_if(b.assets.begin(), b.assets.end(), [](const Asset& x) { return x.kind != AssetKind::Audio; });
        if (a != b.assets.end()) pick = &*a;
      }
      if (!pick) return {};
      const QString path = assetPath(*pick);
      if (!QFileInfo::exists(path)) return {};
      if (pick->kind == AssetKind::Image) {
        QImageReader r(path);
        r.setAutoTransform(true);
        if (r.size().isValid()) r.setScaledSize(r.size().scaled(640, 400, Qt::KeepAspectRatio));
        return r.read();
      }
      const auto strip = extractThumbnailStrip(path, 1, QSize(640, 400));
      return strip.empty() ? QImage() : strip.front().image;
    } catch (const std::exception&) {
      return {};
    }
  }

  QMutex mutex_;
  std::map<QString, QImage> cache_;
};

} // namespace sf::app
