#include "render/offscreen.h"

#include "media/gpu_frame.h"
#include "render/gpu_planes.h"

#ifdef Q_OS_WIN
#include <rhi/qrhi_platform.h>
#endif

namespace sf::render {

struct OffscreenRenderer::Impl {
  std::unique_ptr<QRhi> rhi;
  std::unique_ptr<Compositor> compositor; // destroyed before the rhi it uses
};

OffscreenRenderer::OffscreenRenderer() : d(std::make_unique<Impl>()) {}
OffscreenRenderer::~OffscreenRenderer() {
  d->compositor.reset();
  d->rhi.reset();
}

std::unique_ptr<OffscreenRenderer> OffscreenRenderer::create(QString* error) {
  std::unique_ptr<OffscreenRenderer> r(new OffscreenRenderer());
#ifdef Q_OS_WIN
  QRhiD3D11InitParams params;
  if (const auto shared = sharedD3D11Device()) {
    // adopt the decoders' device so their surfaces can be sampled without leaving the GPU
    QRhiD3D11NativeHandles handles;
    handles.dev = shared->device;
    handles.context = shared->context;
    handles.featureLevel = shared->featureLevel;
    handles.adapterLuidLow = shared->adapterLuidLow;
    handles.adapterLuidHigh = shared->adapterLuidHigh;
    r->d->rhi.reset(QRhi::create(QRhi::D3D11, &params, QRhi::Flags(), &handles));
  }
  if (!r->d->rhi) r->d->rhi.reset(QRhi::create(QRhi::D3D11, &params));
#endif
  if (!r->d->rhi) {
    if (error) *error = QStringLiteral("Can't create a D3D11 QRhi");
    return nullptr;
  }
  r->d->compositor = std::make_unique<Compositor>(r->d->rhi.get());
  if (!r->d->compositor->ok()) {
    if (error) *error = QStringLiteral("Compositor shaders or pipelines failed to build");
    return nullptr;
  }
  return r;
}

RenderStats OffscreenRenderer::renderQueued(const TimelineDoc& doc, Frame frame, MediaProvider& media) {
  RenderStats stats;
  QRhiCommandBuffer* cb = nullptr;
  if (d->rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) return stats;
  stats = d->compositor->render(cb, doc, frame, media);
  d->rhi->endOffscreenFrame();
  return stats;
}

RenderStats OffscreenRenderer::renderAndWait(const TimelineDoc& doc, Frame frame, MediaProvider& media) {
  const RenderStats stats = renderQueued(doc, frame, media);
  waitForGpu();
  return stats;
}

void OffscreenRenderer::waitForGpu() { sf::render::waitForGpu(d->rhi.get()); }

QRhi* OffscreenRenderer::rhi() const { return d->rhi.get(); }
Compositor& OffscreenRenderer::compositor() { return *d->compositor; }

QImage OffscreenRenderer::render(const TimelineDoc& doc, Frame frame, MediaProvider& media, RenderStats* stats) {
  QRhiCommandBuffer* cb = nullptr;
  if (d->rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) return {};
  const RenderStats s = d->compositor->render(cb, doc, frame, media);
  if (stats) *stats = s;
  QRhiReadbackResult result;
  QRhiResourceUpdateBatch* u = d->rhi->nextResourceUpdateBatch();
  if (QRhiTexture* canvas = d->compositor->canvasTexture()) u->readBackTexture(QRhiReadbackDescription(canvas), &result);
  cb->resourceUpdate(u);
  d->rhi->endOffscreenFrame(); // D3D11 completes the readback here
  if (result.data.isEmpty()) return {};
  // D3D11 textures are top-left origin, so rows come back in image order
  return QImage(reinterpret_cast<const uchar*>(result.data.constData()), result.pixelSize.width(), result.pixelSize.height(), QImage::Format_RGBA8888).copy();
}

} // namespace sf::render
