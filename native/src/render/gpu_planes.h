#pragma once

// Internal: the D3D11 side of the zero-copy path.
//
// D3D11VA hands out slices of a texture array, and QRhi cannot create a view of one slice of an
// array. So each layer owns a plain NV12/P010 texture on the shared device: the decoded slice is
// copied into it on the GPU (CopySubresourceRegion, a few microseconds even at 4K, no CPU involved)
// and its two planes are exposed as QRhiTextures (R8 + RG8, or R16 + RG16) the shader samples.

#include "media/gpu_frame.h"

#include <rhi/qrhi.h>

#include <QSize>

#include <memory>

namespace sf::render {

// Does this QRhi run on the process-wide D3D11 device the decoders use?
bool runsOnSharedDevice(QRhi* rhi);

// Blocks until the GPU has finished everything submitted so far (an event query). False if the
// QRhi isn't D3D11. For benchmarks and export, never the preview.
bool waitForGpu(QRhi* rhi);

class GpuPlanes {
public:
  GpuPlanes();
  ~GpuPlanes();
  GpuPlanes(const GpuPlanes&) = delete;
  GpuPlanes& operator=(const GpuPlanes&) = delete;

  // (Re)creates the texture and plane views when format or size changed. False when D3D11 refuses.
  bool ensure(QRhi* rhi, GpuFormat format, QSize size);
  // Copies the frame's slice in. Must run inside QRhiCommandBuffer::beginExternal()/endExternal().
  void copyFrom(const GpuFrame& frame);
  QRhiTexture* luma() const;
  QRhiTexture* chroma() const;
  quint64 generation() const; // changes whenever the views are recreated

private:
  struct Impl;
  std::unique_ptr<Impl> d;
};

} // namespace sf::render
