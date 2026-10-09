#pragma once

// The compositor: one timeline frame -> one RGBA texture, on QRhi.
//
// Preview and export share it. Preview records into the window's command buffer and then lets
// present() put the canvas on screen; export and tests use OffscreenRenderer (offscreen.h), which
// owns a QRhi without a window and reads the canvas back.
//
// Semantics are those of packages/renderer/src/compositor.ts: items bottom to top (tracks are listed
// top-first; hidden tracks skipped), each fitted into the canvas ("contain"), scaled / rotated /
// moved by its transform about the canvas centre, drawn with its opacity (keyframes and fades)
// source-over onto the project's background colour, in non-linear (gamma) RGB like the canvas 2D API.
//
// Video comes from the provider as either a GPU surface (decoded on the shared D3D11 device: copied
// plane to plane on the GPU, converted to RGB in the shader) or an RGBA image (software decode, no
// shared device, or a frame demoted for the scrub cache): same result, different upload.

#include "core/schema.h"
#include "render/media_provider.h"

#include <rhi/qrhi.h>

#include <QColor>
#include <QSize>

#include <memory>

namespace sf::render {

struct RenderStats {
  int layers = 0;     // drawn
  int gpuLayers = 0;  // video layers that stayed on the GPU
  int cpuLayers = 0;  // video/image layers uploaded from system memory
  int pending = 0;    // video layers whose picture isn't decoded yet (stale or skipped)
  int missing = 0;    // layers drawn as "media unavailable"
  bool complete() const { return pending == 0; }
};

class Compositor {
public:
  explicit Compositor(QRhi* rhi);
  ~Compositor();
  Compositor(const Compositor&) = delete;
  Compositor& operator=(const Compositor&) = delete;

  bool ok() const; // shaders loaded and pipelines built

  // Zero-copy video needs the QRhi to run on the shared D3D11 device. True when it does.
  bool gpuCapable() const;
  // Force the CPU path even when the GPU one is possible (hw-vs-sw parity tests, benchmarks).
  void setGpuEnabled(bool enabled);

  // Records the frame into `cb` (outside any pass) and leaves it in canvasTexture(). The canvas is
  // the project's size. Resource creation and uploads happen here; nothing blocks on the GPU.
  RenderStats render(QRhiCommandBuffer* cb, const TimelineDoc& doc, Frame frame, MediaProvider& media);

  // Forget what the layer textures hold, so the next render() uploads / copies everything again
  // (after a device reset; for benchmarks that want the full per-frame cost of an unchanged frame).
  void invalidate();

  QRhiTexture* canvasTexture() const; // RGBA8, valid after render()
  QSize canvasSize() const;

  // Preview: draws the canvas into `target` (a window / QQuickRhiItem render target) scaled to fit
  // and centred, over `letterbox`.
  void present(QRhiCommandBuffer* cb, QRhiRenderTarget* target, const QColor& letterbox);

private:
  struct Impl;
  std::unique_ptr<Impl> d;
};

} // namespace sf::render
