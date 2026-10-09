#pragma once

// Internal: the process-wide D3D11VA device decoders share, so N clips don't create N devices.
//
// Zero-copy plan: the compositor's QRhi (D3D11 backend) owns an ID3D11Device. Instead of letting
// FFmpeg create its own device here, setSharedD3D11Device() will wrap that ID3D11Device in an
// AVHWDeviceContext (AVD3D11VADeviceContext::device), so decoded textures live on the device that
// samples them and need no copy or cross-device share handle.

#include "media/ffmpeg.h"

namespace sf::media {

// New reference to the shared device, or null when D3D11VA is unavailable. Thread-safe.
av::BufferPtr sharedD3D11Device();

// Whether libavcodec can decode this codec through D3D11VA (a necessary, not sufficient, condition:
// profile/bit depth are only known once the stream header is parsed).
bool codecHasD3D11(const AVCodec* codec);

} // namespace sf::media
