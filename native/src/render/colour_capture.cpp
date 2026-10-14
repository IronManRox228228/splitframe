#include "render/colour_capture.h"

#include "media/frame_service.h"

#include <algorithm>
#include <cmath>

namespace sf::render {

TimelineDoc isolateItem(const TimelineDoc& doc, const QString& itemId, bool keepPrimaries) {
  TimelineDoc d = doc;
  std::erase_if(d.items, [&](const Item& it) { return it.id != itemId; });
  for (Track& t : d.tracks) t.hidden = false;
  for (Item& it : d.items) {
    if (!keepPrimaries && it.color) {
      it.color->primaries.reset();
    }
  }
  return d;
}

std::vector<Frame> sampleFrames(const Item& item, int count) {
  std::vector<Frame> out;
  count = std::max(1, count);
  const Frame dur = std::max<Frame>(item.durationFrames, 1);
  for (int i = 0; i < count; ++i) {
    const Frame f = item.startFrame + static_cast<Frame>((static_cast<double>(i) + 0.5) * static_cast<double>(dur) / count);
    out.push_back(std::clamp(f, item.startFrame, item.startFrame + dur - 1));
  }
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

Capture captureFrames(OffscreenRenderer& renderer, const TimelineDoc& doc, const AssetTable& assets, const std::vector<Frame>& frames, bool isolated) {
  Capture cap;
  FrameService::Options o;
  o.cacheBytes = 128ll << 20;
  o.workerThreads = 2;
  o.gpuFrames = false;
  o.hw = HwMode::Off; // software decode: the GPU is shared with other work
  FrameService service(o);
  FrameServiceProvider provider(service, assets, FrameServiceProvider::Mode::Blocking);
  provider.openAll();
  renderer.compositor().setDelivery(std::nullopt);

  TimelineDoc white = doc;
  white.project.styleConfig.backgroundColor = QStringLiteral("#ffffff");
  TimelineDoc black = doc;
  black.project.styleConfig.backgroundColor = QStringLiteral("#000000");

  for (const Frame f : frames) {
    provider.prepare(black, f, 0);
    const QImage display = renderer.render(black, f, provider);
    const QImage linear = renderer.renderLinear(black, f, provider);
    if (display.isNull() || linear.isNull()) {
      cap.error = QStringLiteral("The renderer returned no picture for frame %1.").arg(f);
      return cap;
    }
    std::vector<unsigned char> coverage;
    if (isolated) {
      const QImage over = renderer.renderLinear(white, f, provider);
      if (over.size() == linear.size() && over.format() == linear.format()) {
        coverage.resize(static_cast<size_t>(linear.width()) * static_cast<size_t>(linear.height()));
        for (int y = 0; y < linear.height(); ++y) {
          const float* a = reinterpret_cast<const float*>(linear.constScanLine(y));
          const float* b = reinterpret_cast<const float*>(over.constScanLine(y));
          for (int x = 0; x < linear.width(); ++x) {
            const bool same = std::fabs(a[x * 4] - b[x * 4]) < 1e-4f && std::fabs(a[x * 4 + 1] - b[x * 4 + 1]) < 1e-4f && std::fabs(a[x * 4 + 2] - b[x * 4 + 2]) < 1e-4f;
            coverage[static_cast<size_t>(y) * static_cast<size_t>(linear.width()) + static_cast<size_t>(x)] = same ? 1 : 0;
          }
        }
      }
    }
    cap.frames.push_back(pixelsFromImages(linear, &display, kScopeMaxSide, coverage.empty() ? nullptr : &coverage));
  }
  if (cap.frames.empty()) cap.error = QStringLiteral("No frames to measure.");
  return cap;
}

} // namespace sf::render
