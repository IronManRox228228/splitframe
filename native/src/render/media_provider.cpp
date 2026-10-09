#include "render/media_provider.h"

#include "core/apply.h"
#include "core/timeline_doc.h"
#include "render/layout.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageReader>
#include <QThread>

#include <algorithm>
#include <cmath>

namespace sf::render {

std::shared_ptr<const QImage> ImageStore::load(const QString& path) {
  {
    const std::lock_guard lock(m_);
    if (const auto it = images_.find(path); it != images_.end()) return it->second;
  }
  QImageReader reader(path);
  reader.setAutoTransform(true); // a browser shows photos upright; so do we
  QImage img = reader.read();
  std::shared_ptr<const QImage> out;
  if (!img.isNull()) out = std::make_shared<const QImage>(img.convertToFormat(QImage::Format_RGBA8888)); // what the GPU upload takes
  const std::lock_guard lock(m_);
  if (images_.size() >= 16) images_.clear(); // a project rarely shows more stills at once than this
  images_[path] = out;
  return out;
}

FrameServiceProvider::FrameServiceProvider(FrameService& service, AssetTable assets, Mode mode)
    : service_(service), assets_(std::move(assets)), mode_(mode) {}

std::optional<AssetRef> FrameServiceProvider::assetRef(const QString& assetId) const {
  const std::lock_guard lock(am_);
  const auto it = assets_.find(assetId);
  if (it == assets_.end()) return std::nullopt;
  return it->second;
}

AssetTable FrameServiceProvider::assets() const {
  const std::lock_guard lock(am_);
  return assets_;
}

void FrameServiceProvider::setAssets(AssetTable assets) {
  AssetTable old;
  {
    const std::lock_guard lock(am_);
    old = std::move(assets_);
    assets_ = std::move(assets);
  }
  const AssetTable now = this->assets();
  const std::lock_guard lock(m_);
  for (auto it = ids_.begin(); it != ids_.end();) {
    const auto before = old.find(it->first);
    const auto after = now.find(it->first);
    // changed or removed file: drop the decoder; an entry that was "unavailable" gets another look
    const bool stale = after == now.end() || before == old.end() || before->second.path != after->second.path || it->second == 0;
    if (!stale) {
      ++it;
      continue;
    }
    if (it->second != 0) {
      service_.closeAsset(it->second);
      lastShown_.erase(it->second);
    }
    it = ids_.erase(it);
  }
}

AssetId FrameServiceProvider::assetFor(const QString& assetId) {
  const auto ref = assetRef(assetId);
  if (!ref || ref->kind != AssetKind::Video) return 0;
  const std::lock_guard lock(m_);
  if (const auto it = ids_.find(assetId); it != ids_.end()) return it->second;
  if (!QFileInfo::exists(ref->path)) {
    ids_[assetId] = 0;
    return 0;
  }
  const AssetId id = service_.openAsset(ref->path);
  ids_[assetId] = id;
  return id;
}

std::optional<qint64> FrameServiceProvider::sourceIndex(AssetId id, const VideoStreamInfo& info, Frame sourceFrame, double fps) {
  // source frames are in project-fps units (docs/DECISIONS.md), i.e. time = sourceFrame / projectFps
  return service_.indexAtTime(id, info.startSec + static_cast<double>(sourceFrame) / fps);
}

Visual FrameServiceProvider::visual(const Item& item, Frame sourceFrame, double fps) {
  Visual v;
  if (!item.assetId) return v;
  const auto ref = assetRef(*item.assetId);
  if (!ref) return v;

  if (ref->kind == AssetKind::Image) {
    v.image = images_.load(ref->path);
    if (!v.image) return v;
    v.size = v.image->size();
    v.state = Visual::State::Ready;
    return v;
  }
  if (ref->kind != AssetKind::Video) return v;

  const AssetId id = assetFor(*item.assetId);
  if (id == 0) return v;

  std::optional<VideoStreamInfo> info = service_.info(id);
  if (mode_ == Mode::Blocking) {
    QElapsedTimer t;
    t.start();
    while (!(info = service_.info(id)) && !service_.failed(id) && t.elapsed() < 30000) QThread::msleep(2);
    if (!info) return v; // failed to open: treat as missing
  }
  if (!info) {
    if (service_.failed(id)) return v;
    v.state = Visual::State::Pending; // still opening
    return v;
  }
  v.rotation = info->rotation;
  const bool swap = info->rotation == 90 || info->rotation == 270;
  v.size = swap ? QSize(info->height, info->width) : QSize(info->width, info->height);

  const std::optional<qint64> index = sourceIndex(id, *info, sourceFrame, fps);
  if (!index) {
    v.state = Visual::State::Pending;
    return v;
  }
  if (mode_ == Mode::Blocking) {
    v.video = service_.frameBlocking(id, *index);
    if (!v.video) return Visual{}; // can't be decoded: unavailable
    v.state = Visual::State::Ready;
    return v;
  }
  if (VideoFramePtr f = service_.cached(id, *index)) {
    v.video = f;
    v.state = Visual::State::Ready;
    const std::lock_guard lock(m_);
    lastShown_[id] = std::move(f);
    return v;
  }
  service_.requestFrame(id, *index);
  v.state = Visual::State::Pending;
  v.exact = false;
  const std::lock_guard lock(m_);
  if (const auto it = lastShown_.find(id); it != lastShown_.end()) v.video = it->second; // hold the last picture
  return v;
}

void FrameServiceProvider::openAll() {
  for (const auto& [id, ref] : assets()) {
    if (ref.kind == AssetKind::Video) assetFor(id);
  }
}

QString FrameServiceProvider::decoderSummary() {
  QStringList names;
  const std::lock_guard lock(m_);
  for (const auto& [asset, id] : ids_) {
    if (id == 0) continue;
    const QString n = service_.decoderName(id);
    if (!n.isEmpty() && !names.contains(n)) names << n;
  }
  return names.join(QStringLiteral(", "));
}

void FrameServiceProvider::prepare(const TimelineDoc& doc, Frame frame, int direction) {
  const double fps = static_cast<double>(doc.project.fps);
  const auto visit = [&](const Item& item, Frame at, bool ahead) {
    if (item.type() != ItemType::Video || !item.assetId) return;
    const AssetId id = assetFor(*item.assetId);
    if (id == 0) return;
    const auto info = service_.info(id);
    if (!info) return;
    const auto index = sourceIndex(id, *info, sourceFrameAt(item, at), fps);
    if (!index) return;
    if (!service_.cache().contains(id, *index)) service_.requestFrame(id, *index);
    if (!ahead) {
      // a negative speed or remap slope walks the source backwards
      const int srcDir = direction == 0 ? 0 : (effectiveSpeed(item) < 0 ? -direction : direction);
      service_.setPlayhead(id, *index, srcDir);
    }
  };

  if (direction > 0) {
    // clips about to start: have their first picture ready before the cut
    const Frame horizon = frame + static_cast<Frame>(std::max(1.0, fps / 2));
    for (const Item& item : doc.items) {
      if (item.startFrame > frame && item.startFrame <= horizon) visit(item, item.startFrame, true);
    }
  }
  for (const Item* item : activeItems(doc, frame)) visit(*item, frame, false);
}

} // namespace sf::render
