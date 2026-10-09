#pragma once

// Zero-copy hand-off between the decoder and the compositor (see the plan in video_decoder.h).

#include <QImage>
#include <QString>
#include <QtGlobal>

#include <memory>
#include <optional>

namespace sf {

enum class GpuFormat { Nv12, P010 };

// The D3D11 device the whole process shares: decoders (D3D11VA) and the compositor's QRhi both run
// on it, so decoded surfaces can be sampled without leaving the GPU. Created on first use.
// Hand it to Qt as an imported device: QQuickGraphicsDevice::fromDeviceAndContext(device, context)
// for the window, QRhiD3D11NativeHandles for an offscreen QRhi. All pointers stay valid for the
// life of the process.
struct D3D11DeviceInfo {
  void* device = nullptr;  // ID3D11Device*
  void* context = nullptr; // ID3D11DeviceContext*
  int featureLevel = 0;    // D3D_FEATURE_LEVEL
  quint32 adapterLuidLow = 0;
  qint32 adapterLuidHigh = 0;
  QString adapterName; // "NVIDIA GeForce RTX 4060 Laptop GPU"
};
// nullopt when D3D11 (or its video support) is unavailable.
std::optional<D3D11DeviceInfo> sharedD3D11Device();
// The app lets Qt Quick create the window's device (importing one makes Qt 6.8.3 crash in its HDR
// swap chain query, which looks the adapter up by a LUID Qt Quick never fills in) and adopts it here
// instead. Call before anything opens a decoder or asks for sharedD3D11Device(); false when a device
// was already chosen or this one can't decode video. Both pointers must outlive the process' use.
bool adoptD3D11Device(void* device, void* context);

// The GPU everything should run on: the high-performance adapter (a laptop's discrete GPU even
// though the display hangs off the integrated one), overridable with SF_ADAPTER=<index>. The app
// hands its LUID to Qt Quick (QQuickGraphicsDevice::fromAdapter) so the window's device lands there too.
struct GpuAdapter {
  quint32 luidLow = 0;
  qint32 luidHigh = 0;
  QString name;
};
std::optional<GpuAdapter> preferredGpuAdapter();

// A decoded picture still sitting in a D3D11VA surface. Owns a reference that keeps the pool
// surface from being reused, so keep few of these alive (FrameCache limits them per asset).
struct GpuFrame {
  void* device = nullptr;  // ID3D11Device* the texture lives on
  void* texture = nullptr; // ID3D11Texture2D*, a texture array; valid while `hold` is
  int slice = 0;           // array slice of this picture
  int width = 0;           // visible size, before rotation (the texture itself is padded to the codec's alignment)
  int height = 0;
  GpuFormat format = GpuFormat::Nv12;
  int bitDepth = 8;
  // YUV -> RGB the same way frame_convert does it: stream matrix (untagged: BT.709 from 720p, else
  // BT.601) and range (untagged: limited).
  double kr = 0.2126;
  double kb = 0.0722;
  bool fullRange = false;
  std::shared_ptr<void> hold; // the AVFrame reference; opaque so this header stays FFmpeg-free
};

struct VideoFrame;
// Pixels of any frame as RGBA: the frame's own image, or a download + conversion for a GPU frame.
// Slow for GPU frames (that is the point of not doing it); for fallbacks, tests and demotion.
QImage frameImage(const VideoFrame& frame);

} // namespace sf
