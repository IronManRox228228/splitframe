#pragma once

// Frame-accurate video decoding for the editor.
//
// Model: frame N is the Nth *presented* frame of the video stream. A packet pre-pass at open time
// builds the frame table (pts + keyframe flag per frame), so N maps to an exact timestamp even for
// variable-frame-rate clips, B-frame reordering and non-zero start times. Time is expressed in
// seconds from the container start, the same origin the probe's *StartSec fields and AudioDecoder use.
//
// Output today: VideoFrame::image, an RGBA8888 QImage in the stream's own orientation (rotation is
// metadata the compositor applies, see MediaInfo::rotation). Hardware frames are downloaded and
// converted on the decoder's thread.
//
// ---------------------------------------------------------------------------------------------
// Zero-copy D3D11 frames (VideoOpenOptions::gpu)
//
//  * D3D11VA decodes into an ID3D11Texture2D *array* (NV12/P010), one slice per surface.
//    AVFrame::data[0] is the texture, data[1] the slice index.
//  * The device is the process-wide one from sharedD3D11Device() (gpu_frame.h): the compositor's QRhi
//    adopts it, so decoder output is already on the device that samples it.
//  * With `gpu` set, a hardware frame comes out as VideoFrame::gpu (a GpuFrame) and `image` stays
//    empty: no download, no CPU conversion. GpuFrame owns a reference to the AVFrame, which pins its
//    pool surface. The compositor copies the slice (GPU to GPU, QRhi cannot view one slice of an
//    array) into a plain NV12 texture, binds its planes as R8 + R8G8 (R16 + R16G16 for P010) and
//    does YUV->RGB in its shader.
//  * Surface pool pressure: every live GpuFrame pins one surface. The pool holds `gpuPoolExtra` more
//    surfaces than the codec itself needs; once that many GpuFrames of one decoder are alive the
//    decoder hands out ordinary RGBA frames instead (so it can never run dry), and FrameCache keeps
//    only a few GpuFrames per asset, demoting older ones to RGBA for deep scrubbing.
//  * Cross-device (compositor on D3D12/Vulkan) would instead need shared NT handles
//    (D3D11_RESOURCE_MISC_SHARED_NTHANDLE) imported as D3D12 resources; ID3D11Fence keeps decode
//    and sampling ordered. Not built.
// ---------------------------------------------------------------------------------------------

#include "media/gpu_frame.h"

#include <QImage>
#include <QString>

#include <functional>
#include <memory>

namespace sf {

enum class HwMode {
  Auto,     // try D3D11VA, quietly fall back to software (the default)
  Off,      // software only
  Required, // fail to open (or to decode) rather than fall back; for tests and benchmarks
};

struct VideoOpenOptions {
  HwMode hw = HwMode::Auto;
  int swThreads = 0; // software decoder threads; 0 = FFmpeg decides
  // Hardware frames stay on the GPU (VideoFrame::gpu) instead of being downloaded to `image`.
  // Only for consumers that sample the shared D3D11 device; software frames always carry `image`.
  bool gpu = false;
  int gpuPoolExtra = 8; // extra D3D11 surfaces beyond what the codec needs = max live GpuFrames per decoder
};

struct VideoStreamInfo {
  qint64 frameCount = 0;
  int width = 0; // coded size, before rotation
  int height = 0;
  int rotation = 0; // degrees clockwise to display upright
  double avgFps = 0;
  bool vfr = false;
  double startSec = 0;    // time of frame 0, seconds from container start
  double durationSec = 0; // time of the last frame plus one frame duration
  QString codec;
  QString pixelFormat; // as stored; hardware output is NV12/P010 before conversion
};

struct VideoFrame {
  qint64 index = 0;
  double ptsSec = 0; // seconds from container start
  QImage image;      // RGBA8888, display colours (matrix and range applied), not rotated; empty when `gpu` is set
  std::shared_ptr<const GpuFrame> gpu; // set instead of `image` for zero-copy hardware frames
  ColorTrc trc = ColorTrc::Unspecified; // the stream's transfer / primaries tags (also set on `gpu` frames)
  ColorPrim prim = ColorPrim::Unspecified;
  bool hardware = false; // came out of D3D11VA rather than the software decoder
  // Pixel data held, for cache accounting (GPU frames: the NV12/P010 surface they pin)
  qint64 byteSize() const {
    if (gpu) {
      const qint64 px = static_cast<qint64>(gpu->width) * gpu->height;
      return gpu->format == GpuFormat::P010 ? px * 3 : px * 3 / 2;
    }
    return image.sizeInBytes();
  }
};
using VideoFramePtr = std::shared_ptr<const VideoFrame>;

// Decodes one video stream. NOT thread-safe: it keeps decoder state between calls (that is the point,
// sequential and nearby requests reuse it), so use it from one thread at a time. Move between threads
// is fine. FrameService runs one per asset on its worker pool.
class VideoDecoder {
public:
  using Cancel = std::function<bool()>;            // polled between frames; true aborts the call
  using FrameSink = std::function<void(VideoFramePtr)>;

  // Reads headers and scans packets (disk-bound for big files; do this off the UI thread).
  static std::unique_ptr<VideoDecoder> open(const QString& path, const VideoOpenOptions& options = {},
                                            QString* error = nullptr);
  ~VideoDecoder();
  VideoDecoder(const VideoDecoder&) = delete;
  VideoDecoder& operator=(const VideoDecoder&) = delete;

  const VideoStreamInfo& info() const;
  qint64 frameCount() const { return info().frameCount; }

  // Whether the *current* decoding session runs on the GPU. Can flip to false mid-stream if the
  // hardware decoder errors; decoderName() says which and why.
  bool usingHardware() const;
  QString decoderName() const; // "h264 (d3d11va)", "hevc (software)", ...

  // Frame <-> time. indexAtTime picks the frame on screen at that time (last frame with pts <= t),
  // clamped to [0, frameCount-1].
  double timeOfFrame(qint64 index) const;
  qint64 indexAtTime(double sec) const;
  bool isKeyFrame(qint64 index) const;

  // The frame with this index; null (see lastError) on failure or cancel. Continues from decoder state
  // when that is cheaper than seeking (same GOP, or a few frames ahead), otherwise seeks to the
  // keyframe at or before `index` and decodes forward, dropping frames before it without converting.
  // Frames in [keepFrom, index) seen on the way are handed to `sink` (backward scrubbing wants them).
  VideoFramePtr frameAt(qint64 index, const Cancel& cancel = {}, qint64 keepFrom = -1, const FrameSink& sink = {});

  // The frame after the last one returned (frame 0 first); null at the end of the stream.
  VideoFramePtr next(const Cancel& cancel = {});

  QString lastError() const;

  // Counters for tests and the benchmark.
  struct Stats {
    qint64 seeks = 0;          // demuxer seeks + decoder flushes
    qint64 framesDecoded = 0;  // frames out of the decoder, converted or not
    qint64 framesConverted = 0;
    double convertSeconds = 0; // GPU download + colour conversion, i.e. the part that isn't codec work
  };
  Stats stats() const;

private:
  VideoDecoder();
  struct Impl;
  std::unique_ptr<Impl> d;
};

} // namespace sf
