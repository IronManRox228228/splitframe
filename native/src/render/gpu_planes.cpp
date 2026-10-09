#define NOMINMAX
#include "render/gpu_planes.h"

#include <algorithm>

#ifdef Q_OS_WIN
#include <rhi/qrhi_platform.h>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#endif

namespace sf::render {

#ifdef Q_OS_WIN

using Microsoft::WRL::ComPtr;

bool runsOnSharedDevice(QRhi* rhi) {
  if (!rhi || rhi->backend() != QRhi::D3D11) return false;
  const auto* h = static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles());
  const auto shared = sharedD3D11Device();
  return h && shared && h->dev == shared->device;
}

bool waitForGpu(QRhi* rhi) {
  if (!rhi || rhi->backend() != QRhi::D3D11) return false;
  const auto* h = static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles());
  if (!h || !h->dev || !h->context) return false;
  auto* device = static_cast<ID3D11Device*>(h->dev);
  auto* context = static_cast<ID3D11DeviceContext*>(h->context);
  D3D11_QUERY_DESC desc{};
  desc.Query = D3D11_QUERY_EVENT;
  ComPtr<ID3D11Query> query;
  if (FAILED(device->CreateQuery(&desc, &query))) return false;
  context->End(query.Get());
  context->Flush();
  // poll sparsely: every GetData takes the device lock the decoder threads need too
  while (context->GetData(query.Get(), nullptr, 0, 0) == S_FALSE) {
    for (int i = 0; i < 4000; ++i) YieldProcessor();
  }
  return true;
}

struct GpuPlanes::Impl {
  ComPtr<ID3D11Texture2D> texture;
  std::unique_ptr<QRhiTexture> y;
  std::unique_ptr<QRhiTexture> uv;
  GpuFormat format = GpuFormat::Nv12;
  QSize size;
  quint64 generation = 0;
  ID3D11DeviceContext* context = nullptr;
};

GpuPlanes::GpuPlanes() : d(std::make_unique<Impl>()) {}
GpuPlanes::~GpuPlanes() = default;

bool GpuPlanes::ensure(QRhi* rhi, GpuFormat format, QSize size) {
  Impl& s = *d;
  if (s.texture && s.format == format && s.size == size) return true;
  s.y.reset();
  s.uv.reset();
  s.texture.Reset();
  const auto shared = sharedD3D11Device();
  if (!shared || !runsOnSharedDevice(rhi)) return false;
  auto* device = static_cast<ID3D11Device*>(shared->device);
  s.context = static_cast<ID3D11DeviceContext*>(shared->context);

  // 4:2:0 needs even dimensions
  const QSize even((size.width() + 1) & ~1, (size.height() + 1) & ~1);
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = static_cast<UINT>(even.width());
  desc.Height = static_cast<UINT>(even.height());
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format == GpuFormat::P010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &s.texture))) return false;

  const bool wide = format == GpuFormat::P010;
  s.y.reset(rhi->newTexture(wide ? QRhiTexture::R16 : QRhiTexture::R8, even));
  s.uv.reset(rhi->newTexture(wide ? QRhiTexture::RG16 : QRhiTexture::RG8, QSize(even.width() / 2, even.height() / 2)));
  // the plane views are D3D shader resource views of the NV12 texture with a one/two channel format
  if (!s.y->createFrom({reinterpret_cast<quint64>(s.texture.Get()), 0}) || !s.uv->createFrom({reinterpret_cast<quint64>(s.texture.Get()), 0})) {
    s.y.reset();
    s.uv.reset();
    s.texture.Reset();
    return false;
  }
  s.format = format;
  s.size = size;
  ++s.generation;
  return true;
}

void GpuPlanes::copyFrom(const GpuFrame& frame) {
  Impl& s = *d;
  if (!s.texture || !s.context || !frame.texture) return;
  D3D11_BOX box{};
  box.right = static_cast<UINT>(std::min(frame.width, (s.size.width() + 1) & ~1));
  box.bottom = static_cast<UINT>(std::min(frame.height, (s.size.height() + 1) & ~1));
  box.back = 1;
  s.context->CopySubresourceRegion(s.texture.Get(), 0, 0, 0, 0, static_cast<ID3D11Texture2D*>(frame.texture),
                                   static_cast<UINT>(frame.slice), &box);
}

QRhiTexture* GpuPlanes::luma() const { return d->y.get(); }
QRhiTexture* GpuPlanes::chroma() const { return d->uv.get(); }
quint64 GpuPlanes::generation() const { return d->generation; }

#else

bool runsOnSharedDevice(QRhi*) { return false; }
bool waitForGpu(QRhi*) { return false; }
struct GpuPlanes::Impl {};
GpuPlanes::GpuPlanes() : d(std::make_unique<Impl>()) {}
GpuPlanes::~GpuPlanes() = default;
bool GpuPlanes::ensure(QRhi*, GpuFormat, QSize) { return false; }
void GpuPlanes::copyFrom(const GpuFrame&) {}
QRhiTexture* GpuPlanes::luma() const { return nullptr; }
QRhiTexture* GpuPlanes::chroma() const { return nullptr; }
quint64 GpuPlanes::generation() const { return 0; }

#endif

} // namespace sf::render
