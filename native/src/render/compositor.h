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
// source-over onto the project's background colour.
//
// Colour pipeline (docs in color.h, color_manager.h):
//   source -> [OCIO input LUT | transfer decode + primaries] -> scene-linear working space (RGBA16F,
//   premultiplied) -> blending -> output transform -> display (preview) / delivery (readback).
// The reference blends in gamma (the canvas 2D API does); linear-light blending is the default here and
// BlendSpace::Display (project.colorManagement.blendSpace = "display", or setBlendSpace) reproduces
// the reference's overlaps and fades exactly. Colour effects (brightness, contrast, saturation, hue,
// grayscale, sepia) always act on display-referred sRGB-encoded colour, as in the reference, so they look
// the same in both modes.
//
// Video comes from the provider as either a GPU surface (decoded on the shared D3D11 device: copied
// plane to plane on the GPU, converted to RGB in the shader) or an RGBA image (software decode, no
// shared device, or a frame demoted for the scrub cache): same result, different upload.

#include "core/schema.h"
#include "render/color.h"
#include "render/color_manager.h"
#include "render/media_provider.h"

#include <rhi/qrhi.h>

#include <QColor>
#include <QSize>

#include <memory>
#include <optional>

namespace sf::render {

struct RenderStats {
  int layers = 0;     // drawn
  int gpuLayers = 0;  // video layers that stayed on the GPU
  int cpuLayers = 0;  // video/image layers uploaded from system memory
  int pending = 0;    // video layers whose picture isn't decoded yet (stale or skipped)
  int missing = 0;    // layers drawn as "media unavailable"
  int colorErrors = 0; // colour settings that couldn't be applied (missing LUT / colour space / config); drawn with the built-in path
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

  // ---- colour management ----
  // OpenColorIO config for input spaces, LUT files, working space and display views. Without one only
  // the built-in path exists (sRGB / BT.709 / BT.1886 / PQ / HLG in, linear Rec.709 working, sRGB out).
  void setColorManager(std::shared_ptr<ColorManager> manager);
  // Overrides the project's blendSpace (nullopt: follow the project; absent there: Linear).
  void setBlendSpace(std::optional<BlendSpace> space);
  // Overrides the project's outputSpace for the delivery texture (nullopt: follow the project).
  void setDelivery(std::optional<Delivery> delivery);
  // How SDR video tagged BT.709 / untagged is decoded, and what "Rec.709" delivery encodes with:
  // Srgb (default) is display-referred like browsers and the reference; Bt1886 (pure 2.4) for
  // broadcast-style grading.
  void setSdrTransfer(Transfer transfer);
  // The delivery pass (canvasTexture()) can be skipped when only present() is used.
  void setDeliveryEnabled(bool enabled);
  // The latest colour problem ("LUT file not found: ..."), empty if none; cleared by the next render().
  QString lastColorError() const;

  // Records the frame into `cb` (outside any pass) and leaves it in canvasTexture(). The canvas is
  // the project's size. Resource creation and uploads happen here; nothing blocks on the GPU.
  RenderStats render(QRhiCommandBuffer* cb, const TimelineDoc& doc, Frame frame, MediaProvider& media);

  // Forget what the layer textures hold, so the next render() uploads / copies everything again
  // (after a device reset; for benchmarks that want the full per-frame cost of an unchanged frame).
  void invalidate();

  // The frame in its delivery encoding: RGBA8 (8-bit delivery) or RGB10A2 (10-bit), top-left origin.
  QRhiTexture* canvasTexture() const;
  // What the layers were blended into: RGBA16F, scene-linear working space (or sRGB-encoded Rec.709
  // when blending in the display space). Premultiplied, opaque.
  QRhiTexture* linearCanvas() const;
  QSize canvasSize() const;
  Delivery delivery() const; // of the last render()

  // Preview: draws the canvas into `target` (a window / QQuickRhiItem render target) scaled to fit
  // and centred, over `letterbox`, through the project's display transform (displayView, else sRGB).
  void present(QRhiCommandBuffer* cb, QRhiRenderTarget* target, const QColor& letterbox);

private:
  struct Impl;
  std::unique_ptr<Impl> d;
};

} // namespace sf::render
