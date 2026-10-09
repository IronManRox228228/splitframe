#include "app/preview_item.h"

#include "render/compositor.h"

#include <rhi/qrhi.h>

#include <QQuickRhiItemRenderer>

namespace sf::app {

namespace {

class PreviewRenderer final : public QQuickRhiItemRenderer {
public:
  void initialize(QRhiCommandBuffer*) override {
    if (rhi() != rhi_) {
      compositor_.reset(); // the previous one belonged to the old device
      rhi_ = rhi();
      compositor_ = std::make_unique<render::Compositor>(rhi_);
    }
  }

  // Runs with the UI thread blocked: the only place item state is read.
  void synchronize(QQuickRhiItem* item) override {
    player_ = static_cast<PreviewItem*>(item)->player();
    session_.reset();
    if (!player_) return;
    session_ = player_->session();
    const auto pos = player_->clock().poll();
    frame_ = pos.frame;
    playing_ = pos.playing;
    rate_ = pos.rate;
    if (pos.reachedEnd) player_->clockChanged();
  }

  void render(QRhiCommandBuffer* cb) override {
    const QColor letterbox(10, 11, 13);
    if (!compositor_ || !compositor_->ok() || !session_) {
      cb->beginPass(renderTarget(), letterbox, {1.0f, 0}, rhi()->nextResourceUpdateBatch());
      cb->endPass();
      return;
    }
    // The canvas only needs recomposing when the frame changed or a picture it was waiting for may
    // have arrived; otherwise just show it again (window resize, repaint).
    const bool fresh = session_ != composited_ || frame_ != compositedFrame_ || !compositedComplete_;
    if (fresh) {
      const int dir = playing_ ? (rate_ < 0 ? -1 : 1) : 0;
      session_->media->provider->prepare(session_->doc, frame_, dir);
      const render::RenderStats stats = compositor_->render(cb, session_->doc, frame_, *session_->media->provider);
      composited_ = session_;
      compositedFrame_ = frame_;
      compositedComplete_ = stats.complete();
      player_->notePresented(frame_, stats.complete(), stats.gpuLayers, stats.cpuLayers, playing_, rate_);
    }
    compositor_->present(cb, renderTarget(), letterbox);
    if (playing_) update(); // the clock decides what the next vsync shows
  }

private:
  QRhi* rhi_ = nullptr;
  std::unique_ptr<render::Compositor> compositor_;
  QPointer<Player> player_;
  std::shared_ptr<Session> session_;
  std::shared_ptr<Session> composited_; // what the canvas texture currently holds
  qint64 compositedFrame_ = -1;
  bool compositedComplete_ = false;
  qint64 frame_ = 0;
  bool playing_ = false;
  double rate_ = 1;
};

} // namespace

PreviewItem::PreviewItem(QQuickItem* parent) : QQuickRhiItem(parent) {}

void PreviewItem::setPlayer(Player* p) {
  if (player_ == p) return;
  if (player_) disconnect(player_, nullptr, this, nullptr);
  player_ = p;
  if (player_) {
    // repaint when something moved (seek, open) or a decoded frame landed
    connect(player_, &Player::renderRequested, this, &QQuickRhiItem::update);
    connect(player_, &Player::mediaReady, this, &QQuickRhiItem::update);
    connect(player_, &Player::transportChanged, this, &QQuickRhiItem::update);
  }
  emit playerChanged();
  update();
}

QQuickRhiItemRenderer* PreviewItem::createRenderer() { return new PreviewRenderer; }

} // namespace sf::app
