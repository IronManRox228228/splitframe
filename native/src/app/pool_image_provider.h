#pragma once

// image://pool/<assetId>?<revision>: the first thumbnail of an asset for the media pool cards.
// Qt calls requestImage on its own thread; MediaPool::preview is safe for that.

#include "editor/media_pool.h"

#include <QPointer>
#include <QQuickImageProvider>

namespace sf::app {

class PoolImageProvider : public QQuickImageProvider {
public:
  explicit PoolImageProvider(editor::MediaPool* pool) : QQuickImageProvider(QQuickImageProvider::Image), pool_(pool) {}

  QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override {
    QImage out;
    if (pool_) {
      if (const auto preview = pool_->preview(id.section(QLatin1Char('?'), 0, 0))) out = preview->poster;
    }
    if (!out.isNull() && requestedSize.isValid() && requestedSize.width() > 0 && requestedSize.height() > 0 && out.size() != requestedSize)
      out = out.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (size) *size = out.size();
    return out;
  }

private:
  QPointer<editor::MediaPool> pool_;
};

} // namespace sf::app
