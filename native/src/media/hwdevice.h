#pragma once

// Internal: the process-wide D3D11VA device decoders share, so N clips don't create N devices.
//
// The device is created here (not by FFmpeg) so that the compositor's QRhi can adopt the very same
// ID3D11Device: decoded textures then live on the device that samples them and need no copy or
// cross-device share handle. See sharedD3D11Device() in gpu_frame.h for the handles Qt needs.

#include "media/ffmpeg.h"

namespace sf::media {

// New reference to the shared device, or null when D3D11VA is unavailable. Thread-safe.
av::BufferPtr sharedD3D11Device();

// Whether libavcodec can decode this codec through D3D11VA (a necessary, not sufficient, condition:
// profile/bit depth are only known once the stream header is parsed).
bool codecHasD3D11(const AVCodec* codec);

} // namespace sf::media
