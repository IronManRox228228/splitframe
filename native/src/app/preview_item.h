#pragma once

// The preview surface: a QQuickRhiItem that composites the player's current frame with the
// compositor and shows it letterboxed. Everything GPU happens in the renderer on the render thread.

#include "app/player.h"

#include <QPointer>
#include <QQmlEngine>
#include <QQuickRhiItem>

namespace sf::app {

class PreviewItem : public QQuickRhiItem {
  Q_OBJECT
  QML_ELEMENT
  Q_PROPERTY(sf::app::Player* player READ player WRITE setPlayer NOTIFY playerChanged)

public:
  explicit PreviewItem(QQuickItem* parent = nullptr);
  Player* player() const { return player_; }
  void setPlayer(Player* p);

  QQuickRhiItemRenderer* createRenderer() override;

signals:
  void playerChanged();

private:
  QPointer<Player> player_;
};

} // namespace sf::app
