#include "render/offscreen.h"

#include "media/gpu_frame.h"
#include "render/gpu_planes.h"

#include <QtGlobal>
#include <qfloat16.h>

#include <cstring>

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

namespace {

float halfToFloat(quint16 bits) {
  qfloat16 h;
  std::memcpy(&h, &bits, sizeof bits);
  return static_cast<float>(h);
}

} // namespace

// Renders the frame and reads one of the compositor's textures back in the same frame.
QImage OffscreenRenderer::renderRead(const TimelineDoc& doc, Frame frame, MediaProvider& media, RenderStats* stats, Readback what) {
  QRhiCommandBuffer* cb = nullptr;
  if (d->rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) return {};
  const RenderStats s = d->compositor->render(cb, doc, frame, media);
  if (stats) *stats = s;
  QRhiTexture* tex = what == Readback::Linear ? d->compositor->linearCanvas() : d->compositor->canvasTexture();
  QRhiReadbackResult result;
  QRhiResourceUpdateBatch* u = d->rhi->nextResourceUpdateBatch();
  if (tex) u->readBackTexture(QRhiReadbackDescription(tex), &result);
  cb->resourceUpdate(u);
  d->rhi->endOffscreenFrame(); // D3D11 completes the readback here
  if (result.data.isEmpty()) return {};
  const int w = result.pixelSize.width(), h = result.pixelSize.height();
  const auto* bytes = reinterpret_cast<const uchar*>(result.data.constData());
  // D3D11 textures are top-left origin, so rows come back in image order
  switch (result.format) {
  case QRhiTexture::RGBA8: {
    if (what != Readback::Rgba64) return QImage(bytes, w, h, QImage::Format_RGBA8888).copy();
    QImage out(w, h, QImage::Format_RGBA64);
    for (int y = 0; y < h; ++y) {
      auto* dst = reinterpret_cast<quint16*>(out.scanLine(y));
      const uchar* src = bytes + static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
      for (int i = 0; i < w * 4; ++i) dst[i] = static_cast<quint16>(src[i] * 257);
    }
    return out;
  }
  case QRhiTexture::RGB10A2: {
    const bool wide = what == Readback::Rgba64;
    QImage out(w, h, wide ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y) {
      const auto* src = reinterpret_cast<const quint32*>(bytes) + static_cast<size_t>(y) * static_cast<size_t>(w);
      for (int x = 0; x < w; ++x) {
        const quint32 v = src[x];
        const quint32 c[3] = {v & 1023u, (v >> 10) & 1023u, (v >> 20) & 1023u};
        if (wide) {
          auto* dst = reinterpret_cast<quint16*>(out.scanLine(y)) + x * 4;
          for (int k = 0; k < 3; ++k) dst[k] = static_cast<quint16>((c[k] * 65535u + 511u) / 1023u);
          dst[3] = 65535;
        } else {
          uchar* dst = out.scanLine(y) + x * 4;
          for (int k = 0; k < 3; ++k) dst[k] = static_cast<uchar>((c[k] * 255u + 511u) / 1023u);
          dst[3] = 255;
        }
      }
    }
    return out;
  }
  case QRhiTexture::RGBA16F: {
    QImage out(w, h, QImage::Format_RGBA32FPx4_Premultiplied);
    for (int y = 0; y < h; ++y) {
      auto* dst = reinterpret_cast<float*>(out.scanLine(y));
      const auto* src = reinterpret_cast<const quint16*>(bytes) + static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
      for (int i = 0; i < w * 4; ++i) dst[i] = halfToFloat(src[i]);
    }
    return out;
  }
  default: return {};
  }
}

QImage OffscreenRenderer::render(const TimelineDoc& doc, Frame frame, MediaProvider& media, RenderStats* stats) {
  return renderRead(doc, frame, media, stats, Readback::Rgba8);
}
QImage OffscreenRenderer::renderRgba64(const TimelineDoc& doc, Frame frame, MediaProvider& media, RenderStats* stats) {
  return renderRead(doc, frame, media, stats, Readback::Rgba64);
}
QImage OffscreenRenderer::renderLinear(const TimelineDoc& doc, Frame frame, MediaProvider& media, RenderStats* stats) {
  return renderRead(doc, frame, media, stats, Readback::Linear);
}

} // namespace sf::render
