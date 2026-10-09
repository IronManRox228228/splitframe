#pragma once

// Rendering without a window: its own QRhi (D3D11, on the shared decode device when there is one)
// and a Compositor, for tests and for export (M4), which will keep the canvas texture on the GPU
// and hand it to the encoder instead of reading it back.

#include "render/compositor.h"

#include <QImage>

#include <memory>

namespace sf::render {

class OffscreenRenderer {
public:
  // nullptr (with *error) when no D3D11 device can be created or the shaders are missing.
  static std::unique_ptr<OffscreenRenderer> create(QString* error = nullptr);
  ~OffscreenRenderer();

  QRhi* rhi() const;
  Compositor& compositor();

  // One frame, read back as RGBA8888 at the project's size. Blocks until the GPU is done.
  QImage render(const TimelineDoc& doc, Frame frame, MediaProvider& media, RenderStats* stats = nullptr);

  // Same, but without the readback: returns once the GPU has finished the frame (an event query
  // waits for it). What preview costs, and what export pays before handing the texture to an encoder.
  RenderStats renderAndWait(const TimelineDoc& doc, Frame frame, MediaProvider& media);
  // Same without the wait: the frame is queued on the GPU and sits in canvasTexture(). Preview works
  // like this (it never waits for the GPU); fence now and then with waitForGpu().
  RenderStats renderQueued(const TimelineDoc& doc, Frame frame, MediaProvider& media);
  void waitForGpu(); // everything queued so far is done on return

private:
  OffscreenRenderer();
  struct Impl;
  std::unique_ptr<Impl> d;
};

} // namespace sf::render
