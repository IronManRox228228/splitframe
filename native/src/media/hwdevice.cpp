#include "media/hwdevice.h"

#include "media/gpu_frame.h"

#ifdef _WIN32
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}
#endif

#include <QtGlobal>

#include <mutex>

namespace sf {
namespace {

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

struct Shared {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Multithread> multithread;
  av::BufferPtr hw; // the AVHWDeviceContext wrapping `device`
  D3D11DeviceInfo info;
};

std::mutex gMutex;
std::unique_ptr<Shared> gShared;
bool gChosen = false; // a device has been created or adopted (or creating one failed)

// Makes the decoders' view of an existing device: multithread protection, the AVHWDeviceContext.
std::unique_ptr<Shared> wrap(ComPtr<ID3D11Device> device, ComPtr<ID3D11DeviceContext> context) {
  auto s = std::make_unique<Shared>();
  s->device = std::move(device);
  s->context = std::move(context);
  if (FAILED(s->device.As(&s->multithread))) return nullptr;
  s->multithread->SetMultithreadProtected(TRUE);

  av::BufferPtr hw(av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA));
  if (!hw) return nullptr;
  auto* dev = reinterpret_cast<AVHWDeviceContext*>(hw->data);
  auto* d3d = static_cast<AVD3D11VADeviceContext*>(dev->hwctx);
  d3d->device = s->device.Get();
  d3d->device->AddRef(); // the AVHWDeviceContext releases it with its own reference
  // FFmpeg's own lock (left at its default) keeps decoder threads from interleaving their submissions;
  // multithread protection above makes every individual call on the shared context safe against the
  // render thread. Holding the D3D lock across a whole decode submission instead made the renderer wait for it.
  if (av_hwdevice_ctx_init(hw.get()) < 0) return nullptr;
  s->hw = std::move(hw);

  s->info.device = s->device.Get();
  s->info.context = s->context.Get();
  s->info.featureLevel = static_cast<int>(s->device->GetFeatureLevel());
  ComPtr<IDXGIDevice> dxgi;
  ComPtr<IDXGIAdapter> adapter;
  DXGI_ADAPTER_DESC desc{};
  if (SUCCEEDED(s->device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc))) {
    s->info.adapterLuidLow = desc.AdapterLuid.LowPart;
    s->info.adapterLuidHigh = desc.AdapterLuid.HighPart;
    s->info.adapterName = QString::fromWCharArray(desc.Description);
  }
  return s;
}

ComPtr<IDXGIAdapter1> pickAdapter() {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
  ComPtr<IDXGIAdapter1> adapter;
  bool ok = false;
  if (const int forced = qEnvironmentVariableIsSet("SF_ADAPTER") ? qEnvironmentVariableIntValue("SF_ADAPTER") : -1; forced >= 0) {
    ok = SUCCEEDED(factory->EnumAdapters1(static_cast<UINT>(forced), &adapter));
  } else {
    ComPtr<IDXGIFactory6> f6;
    if (SUCCEEDED(factory.As(&f6))) ok = SUCCEEDED(f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
  }
  if (!ok && FAILED(factory->EnumAdapters1(0, &adapter))) return nullptr;
  return adapter;
}

std::unique_ptr<Shared> createOwn() {
  const ComPtr<IDXGIAdapter1> adapter = pickAdapter();
  if (!adapter) return nullptr;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                               D3D11_SDK_VERSION, &device, nullptr, &context))) {
    return nullptr;
  }
  return wrap(std::move(device), std::move(context));
}

const Shared* shared() {
  const std::lock_guard lock(gMutex);
  if (!gChosen) {
    gChosen = true;
    gShared = createOwn();
  }
  return gShared.get();
}
#endif

} // namespace

std::optional<GpuAdapter> preferredGpuAdapter() {
#ifdef _WIN32
  const ComPtr<IDXGIAdapter1> adapter = pickAdapter();
  DXGI_ADAPTER_DESC1 desc{};
  if (!adapter || FAILED(adapter->GetDesc1(&desc))) return std::nullopt;
  return GpuAdapter{desc.AdapterLuid.LowPart, desc.AdapterLuid.HighPart, QString::fromWCharArray(desc.Description)};
#else
  return std::nullopt;
#endif
}

bool adoptD3D11Device(void* device, void* context) {
#ifdef _WIN32
  const std::lock_guard lock(gMutex);
  if (gChosen || !device || !context) return false;
  gChosen = true;
  ComPtr<ID3D11Device> d(static_cast<ID3D11Device*>(device));
  ComPtr<ID3D11DeviceContext> c(static_cast<ID3D11DeviceContext*>(context));
  gShared = wrap(std::move(d), std::move(c));
  return gShared != nullptr;
#else
  (void)device;
  (void)context;
  return false;
#endif
}

std::optional<D3D11DeviceInfo> sharedD3D11Device() {
#ifdef _WIN32
  if (const Shared* s = shared()) return s->info;
#endif
  return std::nullopt;
}

namespace media {

av::BufferPtr sharedD3D11Device() {
#ifdef _WIN32
  if (const Shared* s = shared()) return av::BufferPtr(av_buffer_ref(s->hw.get()));
#endif
  return nullptr;
}

bool codecHasD3D11(const AVCodec* codec) {
  for (int i = 0;; ++i) {
    const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
    if (!cfg) return false;
    if (cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA && (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) return true;
  }
}

} // namespace media
} // namespace sf
