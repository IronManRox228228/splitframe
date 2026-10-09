#include "media/hwdevice.h"

#include <mutex>

namespace sf::media {

av::BufferPtr sharedD3D11Device() {
  static std::mutex m;
  static av::BufferPtr device;
  static bool tried = false;
  const std::lock_guard lock(m);
  if (!tried) {
    tried = true;
    AVBufferRef* raw = nullptr;
    if (av_hwdevice_ctx_create(&raw, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) >= 0) device.reset(raw);
  }
  return device ? av::BufferPtr(av_buffer_ref(device.get())) : nullptr;
}

bool codecHasD3D11(const AVCodec* codec) {
  for (int i = 0;; ++i) {
    const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
    if (!cfg) return false;
    if (cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA && (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) return true;
  }
}

} // namespace sf::media
