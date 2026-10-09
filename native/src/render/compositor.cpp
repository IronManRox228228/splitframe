#include "render/compositor.h"

#include "core/timeline_doc.h"
#include "render/gpu_planes.h"
#include "render/layer_painter.h"
#include "render/layout.h"

#include <QFile>
#include <QMatrix4x4>
#include <QtGlobal>

#include <algorithm>
#include <cstring>

namespace sf::render {

namespace {

// std140 layout of the uniform block in layer.vert / layer.frag
struct Params {
  float mvp[16];
  float uvx[4];
  float uvy[4];
  float yuv[3][4];
  float fx[3][4];
  float params[4]; // opacity, mode
};
static_assert(sizeof(Params) == 208);

enum Mode { Straight = 0, Premultiplied = 1, Yuv = 2 };

QShader loadShader(const QString& name) {
  QFile f(QStringLiteral(":/sf/render/shaders/") + name);
  if (!f.open(QIODevice::ReadOnly)) return {};
  return QShader::fromSerialized(f.readAll());
}

void fill(Params& p, const QMatrix4x4& mvp, const UvMap& uv, const ColorMatrix& yuv, const ColorMatrix& fx, double opacity, Mode mode) {
  std::memcpy(p.mvp, mvp.constData(), sizeof p.mvp);
  for (size_t i = 0; i < 3; ++i) {
    p.uvx[i] = uv.x[i];
    p.uvy[i] = uv.y[i];
  }
  p.uvx[3] = p.uvy[3] = 0;
  for (size_t r = 0; r < 3; ++r) {
    for (size_t c = 0; c < 4; ++c) {
      p.yuv[r][c] = yuv[r][c];
      p.fx[r][c] = fx[r][c];
    }
  }
  p.params[0] = static_cast<float>(opacity);
  p.params[1] = static_cast<float>(mode);
  p.params[2] = p.params[3] = 0;
}

} // namespace

struct Compositor::Impl {
  // One drawable layer's GPU state. Slots are reused frame to frame by layer position, so a clip that
  // stays in place keeps its textures and (for a still or a paused frame) skips re-uploading.
  struct Slot {
    enum class Kind { None, Rgba, Planes } kind = Kind::None;
    std::unique_ptr<QRhiTexture> rgba;
    QSize rgbaSize;
    quint64 rgbaGen = 0;
    std::unique_ptr<GpuPlanes> planes;
    quint64 planesGen = 0;
    std::unique_ptr<QRhiShaderResourceBindings> srb;
    QRhiTexture* bound0 = nullptr;
    QRhiTexture* bound1 = nullptr;
    quint64 boundGen = 0;
    quint64 boundUbufGen = 0;
    // what the textures currently hold
    VideoFramePtr frame;
    std::shared_ptr<const QImage> still;
    QImage converted; // download of a GPU frame we couldn't sample directly
    QString key;
    // frames not drawn this time are released so a surface isn't pinned by a layer that left
    void releaseContent() {
      frame.reset();
      still.reset();
      converted = QImage();
      key.clear();
      kind = Kind::None;
    }
  };

  QRhi* rhi = nullptr;
  bool ready = false;
  bool gpuEnabled = true;
  QShader vs, fs;
  std::unique_ptr<QRhiBuffer> vbuf;
  bool staticUploaded = false;
  std::unique_ptr<QRhiBuffer> ubuf;
  quint64 ubufGen = 0;
  int ubufSlots = 0;
  quint32 stride = 0;
  std::unique_ptr<QRhiSampler> sampler;
  std::unique_ptr<QRhiTexture> dummy;
  std::unique_ptr<QRhiBuffer> layoutUbuf; // only gives pipelines a binding layout to be compatible with
  std::unique_ptr<QRhiShaderResourceBindings> layoutSrb;

  std::unique_ptr<QRhiTexture> canvas;
  std::unique_ptr<QRhiTextureRenderTarget> canvasRt;
  std::unique_ptr<QRhiRenderPassDescriptor> canvasRpd;
  std::unique_ptr<QRhiGraphicsPipeline> canvasPipeline;
  quint64 canvasGen = 0;

  std::unique_ptr<QRhiBuffer> presentUbuf;
  std::unique_ptr<QRhiShaderResourceBindings> presentSrb;
  quint64 presentSrbGen = 0;
  std::unique_ptr<QRhiGraphicsPipeline> presentPipeline;
  QRhiRenderPassDescriptor* presentRpd = nullptr;

  std::vector<Slot> layerSlots;

  struct Layer {
    size_t slot;
    Params params;
  };

  explicit Impl(QRhi* r) : rhi(r) {}

  std::unique_ptr<QRhiShaderResourceBindings> makeSrb(QRhiBuffer* ub, QRhiTexture* t0, QRhiTexture* t1) {
    std::unique_ptr<QRhiShaderResourceBindings> srb(rhi->newShaderResourceBindings());
    srb->setBindings({
        QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, ub,
                                                                  sizeof(Params)),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, t0, sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, t1, sampler.get()),
    });
    if (!srb->create()) return nullptr;
    return srb;
  }

  std::unique_ptr<QRhiGraphicsPipeline> makePipeline(QRhiRenderPassDescriptor* rpd) {
    std::unique_ptr<QRhiGraphicsPipeline> ps(rhi->newGraphicsPipeline());
    ps->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, fs}});
    QRhiVertexInputLayout input;
    input.setBindings({{2 * sizeof(float)}});
    input.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
    ps->setVertexInputLayout(input);
    ps->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    ps->setCullMode(QRhiGraphicsPipeline::None);
    // premultiplied source-over: the shader already multiplied colour by alpha * opacity
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::One;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    ps->setTargetBlends({blend});
    ps->setShaderResourceBindings(layoutSrb.get());
    ps->setRenderPassDescriptor(rpd);
    if (!ps->create()) return nullptr;
    return ps;
  }

  bool init() {
    vs = loadShader(QStringLiteral("layer.vert.qsb"));
    fs = loadShader(QStringLiteral("layer.frag.qsb"));
    if (!vs.isValid() || !fs.isValid()) return false;
    stride = static_cast<quint32>(rhi->ubufAligned(sizeof(Params)));

    vbuf.reset(rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, 8 * sizeof(float)));
    if (!vbuf->create()) return false;
    sampler.reset(rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    if (!sampler->create()) return false;
    dummy.reset(rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
    if (!dummy->create()) return false;
    growUbuf(8);
    if (!ubuf) return false;
    presentUbuf.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, stride));
    if (!presentUbuf->create()) return false;
    layoutUbuf.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, stride));
    if (!layoutUbuf->create()) return false;
    layoutSrb = makeSrb(layoutUbuf.get(), dummy.get(), dummy.get());
    return layoutSrb != nullptr;
  }

  void growUbuf(int slotsNeeded) {
    if (ubuf && ubufSlots >= slotsNeeded) return;
    const int n = std::max(slotsNeeded, ubufSlots * 2);
    ubuf.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, static_cast<quint32>(n) * stride));
    if (!ubuf->create()) {
      ubuf.reset();
      ubufSlots = 0;
      return;
    }
    ubufSlots = n;
    ++ubufGen; // every slot's bindings point at the old buffer
  }

  bool ensureCanvas(QSize size) {
    if (canvas && canvas->pixelSize() == size) return true;
    canvasPipeline.reset();
    canvasRt.reset();
    canvasRpd.reset();
    canvas.reset(rhi->newTexture(QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!canvas->create()) return false;
    canvasRt.reset(rhi->newTextureRenderTarget({canvas.get()}));
    canvasRpd.reset(canvasRt->newCompatibleRenderPassDescriptor());
    canvasRt->setRenderPassDescriptor(canvasRpd.get());
    if (!canvasRt->create()) return false;
    canvasPipeline = makePipeline(canvasRpd.get());
    ++canvasGen;
    return canvasPipeline != nullptr;
  }

  // (Re)builds a slot's bindings if what they point at changed
  bool bindSlot(Slot& s, QRhiTexture* t0, QRhiTexture* t1, quint64 texGen) {
    if (s.srb && s.bound0 == t0 && s.bound1 == t1 && s.boundGen == texGen && s.boundUbufGen == ubufGen) return true;
    s.srb = makeSrb(ubuf.get(), t0, t1);
    s.bound0 = t0;
    s.bound1 = t1;
    s.boundGen = texGen;
    s.boundUbufGen = ubufGen;
    return s.srb != nullptr;
  }

  // Returns whether the texture's contents must be (re)uploaded.
  bool ensureRgba(Slot& s, QSize size) {
    if (s.rgba && s.rgbaSize == size) return false;
    s.rgba.reset(rhi->newTexture(QRhiTexture::RGBA8, size));
    if (!s.rgba->create()) {
      s.rgba.reset();
      return false;
    }
    s.rgbaSize = size;
    ++s.rgbaGen;
    return true;
  }
};

Compositor::Compositor(QRhi* rhi) : d(std::make_unique<Impl>(rhi)) { d->ready = d->init(); }
Compositor::~Compositor() = default;

bool Compositor::ok() const { return d->ready; }
bool Compositor::gpuCapable() const { return runsOnSharedDevice(d->rhi); }
void Compositor::setGpuEnabled(bool enabled) { d->gpuEnabled = enabled; }
void Compositor::invalidate() {
  for (Impl::Slot& slot : d->layerSlots) slot.releaseContent();
}
QRhiTexture* Compositor::canvasTexture() const { return d->canvas.get(); }
QSize Compositor::canvasSize() const { return d->canvas ? d->canvas->pixelSize() : QSize(); }

RenderStats Compositor::render(QRhiCommandBuffer* cb, const TimelineDoc& doc, Frame frame, MediaProvider& media) {
  Impl& s = *d;
  RenderStats stats;
  if (!s.ready) return stats;
  const QSize size(static_cast<int>(doc.project.width), static_cast<int>(doc.project.height));
  if (size.isEmpty() || !s.ensureCanvas(size)) return stats;

  QRhiResourceUpdateBatch* updates = s.rhi->nextResourceUpdateBatch();
  if (!s.staticUploaded) {
    static const float quad[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    updates->uploadStaticBuffer(s.vbuf.get(), quad);
    const quint8 white[4] = {255, 255, 255, 255};
    QRhiTextureSubresourceUploadDescription px(white, 4);
    px.setSourceSize(QSize(1, 1));
    updates->uploadTexture(s.dummy.get(), QRhiTextureUploadDescription({QRhiTextureUploadEntry(0, 0, px)}));
    s.staticUploaded = true;
  }

  QMatrix4x4 proj = s.rhi->clipSpaceCorrMatrix();
  proj.ortho(0, static_cast<float>(size.width()), static_cast<float>(size.height()), 0, -1, 1);
  const double fps = static_cast<double>(doc.project.fps);

  std::vector<Impl::Layer> layers;
  std::vector<std::pair<GpuPlanes*, const GpuFrame*>> copies;

  const auto slotAt = [&](size_t i) -> Impl::Slot& {
    if (s.layerSlots.size() <= i) s.layerSlots.resize(i + 1);
    return s.layerSlots[i];
  };

  // an uploaded RGBA layer: `changed` says the pixels differ from what the slot's texture holds
  const auto rgbaLayer = [&](const QImage& img, bool changed, Impl::Slot& slot, const QMatrix4x4& model, const UvMap& uv, const ColorMatrix& fx,
                             double opacity, Mode mode) {
    changed |= s.ensureRgba(slot, img.size());
    if (!slot.rgba) return false;
    slot.kind = Impl::Slot::Kind::Rgba;
    if (changed) {
      QRhiTextureSubresourceUploadDescription sub(img);
      updates->uploadTexture(slot.rgba.get(), QRhiTextureUploadDescription({QRhiTextureUploadEntry(0, 0, sub)}));
    }
    if (!s.bindSlot(slot, slot.rgba.get(), s.dummy.get(), slot.rgbaGen)) return false;
    Impl::Layer l;
    l.slot = static_cast<size_t>(&slot - s.layerSlots.data());
    fill(l.params, proj * model, uv, kIdentityColor, fx, opacity, mode);
    layers.push_back(l);
    return true;
  };

  const auto rasterLayer = [&](std::optional<RasterLayer> r, double opacity) {
    if (!r) return;
    Impl::Slot& slot = slotAt(layers.size());
    const bool changed = slot.kind != Impl::Slot::Kind::Rgba || slot.key != r->key || slot.frame || slot.still;
    slot.frame.reset();
    slot.still.reset();
    slot.converted = QImage();
    QMatrix4x4 model;
    model.translate(static_cast<float>(r->origin.x()), static_cast<float>(r->origin.y()));
    model.scale(static_cast<float>(r->image.width()), static_cast<float>(r->image.height()));
    if (rgbaLayer(r->image, changed, slot, model, UvMap{}, kIdentityColor, opacity, Premultiplied)) slot.key = r->key;
  };

  for (const Item* item : activeItems(doc, frame)) {
    const double opacity = opacityAt(*item, frame);
    if (opacity <= 0) continue;
    const ItemType type = item->type();
    if (type == ItemType::Audio) continue;

    if (type != ItemType::Video && type != ItemType::Image) {
      rasterLayer(rasterizeItem(doc, *item, frame, size), opacity);
      continue;
    }

    const Visual v = media.visual(*item, sourceFrameAt(*item, frame), fps);
    if (v.state == Visual::State::Missing) {
      ++stats.missing;
      rasterLayer(rasterizeMissingMedia(*item, frame, size), opacity);
      continue;
    }
    if (v.state == Visual::State::Pending) ++stats.pending;
    if (!v.video && !v.image) continue; // nothing to show yet

    const Placement place = placeMedia(*item, frame, QSizeF(v.size), size);
    const QMatrix4x4 model(place.unitToCanvas());
    const ColorMatrix fx = effectsToColorMatrix(item->effects);
    Impl::Slot& slot = slotAt(layers.size());

    if (v.image) {
      const bool changed = slot.kind != Impl::Slot::Kind::Rgba || slot.still != v.image;
      slot.frame.reset();
      slot.converted = QImage();
      if (rgbaLayer(*v.image, changed, slot, model, UvMap{}, fx, opacity, Straight)) {
        slot.still = v.image;
        ++stats.cpuLayers;
      }
      continue;
    }

    const UvMap uv = uvForRotation(v.rotation);
    const VideoFrame& vf = *v.video;
    if (vf.gpu && s.gpuEnabled) {
      const GpuFrame& g = *vf.gpu;
      if (!slot.planes) slot.planes = std::make_unique<GpuPlanes>();
      if (slot.planes->ensure(s.rhi, g.format, QSize(g.width, g.height))) {
        // the plane views are new objects whenever ensure() rebuilt them; the generation tells
        const quint64 planesGen = slot.planes->generation();
        const bool fresh = slot.kind != Impl::Slot::Kind::Planes || slot.planesGen != planesGen || slot.frame != v.video;
        slot.planesGen = planesGen;
        if (s.bindSlot(slot, slot.planes->luma(), slot.planes->chroma(), planesGen)) {
          slot.kind = Impl::Slot::Kind::Planes;
          if (fresh) copies.emplace_back(slot.planes.get(), &g);
          slot.frame = v.video;
          slot.still.reset();
          slot.converted = QImage();
          Impl::Layer l;
          l.slot = static_cast<size_t>(&slot - s.layerSlots.data());
          fill(l.params, proj * model, uv, yuvToRgb(g.kr, g.kb, g.fullRange, g.bitDepth), fx, opacity, Yuv);
          layers.push_back(l);
          ++stats.gpuLayers;
          continue;
        }
      }
    }
    // CPU path: the frame's own image, or a download when it is a GPU frame we can't (or may not) sample
    const bool changed = slot.kind != Impl::Slot::Kind::Rgba || slot.frame != v.video;
    const QImage* img = &vf.image;
    if (vf.gpu) {
      if (slot.frame != v.video || slot.converted.isNull()) slot.converted = frameImage(vf);
      img = &slot.converted;
    }
    if (img->isNull()) continue;
    slot.still.reset();
    if (rgbaLayer(*img, changed, slot, model, uv, fx, opacity, Straight)) {
      slot.frame = v.video;
      ++stats.cpuLayers;
    }
  }
  stats.layers = static_cast<int>(layers.size());
  for (size_t i = layers.size(); i < s.layerSlots.size(); ++i) s.layerSlots[i].releaseContent();

  s.growUbuf(std::max<int>(8, static_cast<int>(layers.size())));
  // growing replaced the buffer the slots' bindings point at
  for (Impl::Layer& l : layers) {
    Impl::Slot& slot = s.layerSlots[l.slot];
    if (slot.boundUbufGen != s.ubufGen) s.bindSlot(slot, slot.bound0, slot.bound1, slot.boundGen);
    updates->updateDynamicBuffer(s.ubuf.get(), static_cast<quint32>(l.slot) * s.stride, sizeof(Params), &l.params);
  }

  if (!copies.empty()) {
    cb->beginExternal();
    for (const auto& [planes, gpu] : copies) planes->copyFrom(*gpu);
    cb->endExternal();
  }

  // the project's background, opaque (the reference fills it before drawing anything)
  QColor bg(doc.project.styleConfig.backgroundColor);
  if (!bg.isValid()) bg = Qt::black;
  cb->beginPass(s.canvasRt.get(), bg, {1.0f, 0}, updates);
  cb->setGraphicsPipeline(s.canvasPipeline.get());
  cb->setViewport({0, 0, static_cast<float>(size.width()), static_cast<float>(size.height())});
  const QRhiCommandBuffer::VertexInput vertices(s.vbuf.get(), 0);
  cb->setVertexInput(0, 1, &vertices);
  for (const Impl::Layer& l : layers) {
    const QRhiCommandBuffer::DynamicOffset dyn{0, static_cast<quint32>(l.slot) * s.stride};
    cb->setShaderResources(s.layerSlots[l.slot].srb.get(), 1, &dyn);
    cb->draw(4);
  }
  cb->endPass();
  return stats;
}

void Compositor::present(QRhiCommandBuffer* cb, QRhiRenderTarget* target, const QColor& letterbox) {
  Impl& s = *d;
  QRhiResourceUpdateBatch* updates = s.rhi->nextResourceUpdateBatch();
  const QSize px = target->pixelSize();
  if (!s.ready || !s.canvas || px.isEmpty()) {
    cb->beginPass(target, letterbox, {1.0f, 0}, updates);
    cb->endPass();
    return;
  }
  if (!s.presentPipeline || s.presentRpd != target->renderPassDescriptor()) {
    s.presentRpd = target->renderPassDescriptor();
    s.presentPipeline = s.makePipeline(s.presentRpd);
  }
  if (!s.presentSrb || s.presentSrbGen != s.canvasGen) {
    s.presentSrb = s.makeSrb(s.presentUbuf.get(), s.canvas.get(), s.dummy.get());
    s.presentSrbGen = s.canvasGen;
  }
  const QSize cs = s.canvas->pixelSize();
  const double fit = std::min(static_cast<double>(px.width()) / cs.width(), static_cast<double>(px.height()) / cs.height());
  const double w = cs.width() * fit, h = cs.height() * fit;
  QMatrix4x4 proj = s.rhi->clipSpaceCorrMatrix();
  proj.ortho(0, static_cast<float>(px.width()), static_cast<float>(px.height()), 0, -1, 1);
  QMatrix4x4 model;
  model.translate(static_cast<float>((px.width() - w) / 2), static_cast<float>((px.height() - h) / 2));
  model.scale(static_cast<float>(w), static_cast<float>(h));
  Params params;
  fill(params, proj * model, UvMap{}, kIdentityColor, kIdentityColor, 1.0, Premultiplied);
  updates->updateDynamicBuffer(s.presentUbuf.get(), 0, sizeof params, &params);

  cb->beginPass(target, letterbox, {1.0f, 0}, updates);
  if (s.presentPipeline && s.presentSrb) {
    cb->setGraphicsPipeline(s.presentPipeline.get());
    cb->setViewport({0, 0, static_cast<float>(px.width()), static_cast<float>(px.height())});
    const QRhiCommandBuffer::VertexInput vertices(s.vbuf.get(), 0);
    cb->setVertexInput(0, 1, &vertices);
    const QRhiCommandBuffer::DynamicOffset dyn{0, 0};
    cb->setShaderResources(s.presentSrb.get(), 1, &dyn);
    cb->draw(4);
  }
  cb->endPass();
}

} // namespace sf::render
