#include "render/compositor.h"

#include "core/timeline_doc.h"
#include "render/gpu_planes.h"
#include "render/layer_painter.h"
#include "render/layout.h"

#include <QFile>
#include <QMatrix4x4>
#include <QVarLengthArray>
#include <QtGlobal>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <unordered_map>

namespace sf::render {

namespace {

// std140 layout of the uniform block in layer.vert / layer.frag / output.frag
struct Params {
  float mvp[16];
  float uvx[4];
  float uvy[4];
  float yuv[3][4];
  float fx[3][4];
  float params[4]; // opacity, mode, transfer id, canvas encoding (1 = sRGB-encoded)
  float flags[4];  // bits (kLut, kToneMap, kEffects), LUT coord scale, LUT coord offset
  float prim[3][4];
  float toDisp[3][4];
  float fromDisp[3][4];
};
static_assert(sizeof(Params) == 368);

enum Mode { Straight = 0, Premultiplied = 1, Yuv = 2 };
enum Bits { kLut = 1, kToneMap = 2, kEffects = 4 };

QShader loadShader(const QString& name) {
  QFile f(QStringLiteral(":/sf/render/shaders/") + name);
  if (!f.open(QIODevice::ReadOnly)) return {};
  return QShader::fromSerialized(f.readAll());
}

void setRows(float (&dst)[3][4], const Mat3& m) {
  for (size_t r = 0; r < 3; ++r) {
    for (size_t c = 0; c < 3; ++c) dst[r][c] = static_cast<float>(m[r * 3 + c]);
    dst[r][3] = 0;
  }
}

// Which output transform a pass applies: a baked OCIO display LUT, or built-in primaries + encoding.
struct OutputPlan {
  std::shared_ptr<const Lut3D> lut;
  Transfer transfer = Transfer::Srgb;
  Mat3 prim = kIdentity3; // working -> target primaries
};

// Everything colour-related that one render() resolved from the project and the compositor's settings
struct ColorState {
  BlendSpace blend = BlendSpace::Linear;
  QString working; // empty: built-in linear Rec.709
  Mat3 fromRec709 = kIdentity3;
  Mat3 toRec709 = kIdentity3;
  Delivery delivery;
  OutputPlan out;     // delivery texture
  OutputPlan preview; // present()
  bool toneMapHdr = false;
};

// How one layer gets from its source values to the working space
struct LayerColor {
  Transfer transfer = Transfer::Srgb;
  Mat3 prim = kIdentity3;
  std::shared_ptr<const Lut3D> lut;
  bool toneMap = false;
};

void fillCommon(Params& p, const QMatrix4x4& mvp, const UvMap& uv, const ColorState& cs) {
  std::memset(&p, 0, sizeof p);
  std::memcpy(p.mvp, mvp.constData(), sizeof p.mvp);
  for (size_t i = 0; i < 3; ++i) {
    p.uvx[i] = uv.x[i];
    p.uvy[i] = uv.y[i];
  }
  setRows(p.toDisp, cs.toRec709);
  setRows(p.fromDisp, cs.fromRec709);
  p.params[3] = cs.blend == BlendSpace::Display ? 1.0f : 0.0f;
}

void setLut(Params& p, const Lut3D& lut) {
  const float n = static_cast<float>(lut.size);
  p.flags[1] = (n - 1.0f) / n; // texel centres at the grid points
  p.flags[2] = 0.5f / n;
}

void fillLayer(Params& p, const QMatrix4x4& mvp, const UvMap& uv, const ColorMatrix& yuv, const ColorMatrix& fx, double opacity, Mode mode,
               const LayerColor& lc, const ColorState& cs) {
  fillCommon(p, mvp, uv, cs);
  int bits = 0;
  for (size_t r = 0; r < 3; ++r) {
    for (size_t c = 0; c < 4; ++c) {
      p.yuv[r][c] = yuv[r][c];
      p.fx[r][c] = fx[r][c];
    }
  }
  if (mode != Premultiplied && fx != kIdentityColor) bits |= kEffects;
  p.params[0] = static_cast<float>(opacity);
  p.params[1] = static_cast<float>(mode);
  p.params[2] = static_cast<float>(static_cast<int>(lc.transfer));
  setRows(p.prim, lc.prim);
  if (lc.lut) {
    bits |= kLut;
    setLut(p, *lc.lut);
  }
  if (lc.toneMap) bits |= kToneMap;
  p.flags[0] = static_cast<float>(bits);
}

// The output pass: canvas -> encoded. `canvasDisplay` says the canvas holds sRGB-encoded colour.
void fillOutput(Params& p, const QMatrix4x4& mvp, const OutputPlan& plan, const ColorState& cs) {
  fillCommon(p, mvp, UvMap{}, cs);
  p.params[0] = 1.0f;
  p.params[1] = static_cast<float>(Premultiplied);
  p.params[2] = static_cast<float>(static_cast<int>(plan.transfer));
  setRows(p.prim, plan.prim);
  int bits = 0;
  if (plan.lut) {
    bits |= kLut;
    setLut(p, *plan.lut);
  }
  p.flags[0] = static_cast<float>(bits);
}

std::pair<QString, QString> splitDisplayView(const QString& s) {
  const qsizetype i = s.indexOf(QLatin1Char('/'));
  if (i < 0) return {s.trimmed(), QString()};
  return {s.left(i).trimmed(), s.mid(i + 1).trimmed()};
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
    QRhiTexture* bound2 = nullptr; // the colour LUT (or the dummy)
    quint64 boundLutId = 0;
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
  QShader vs, fs, outFs;
  std::unique_ptr<QRhiBuffer> vbuf;
  bool staticUploaded = false;
  std::unique_ptr<QRhiBuffer> ubuf;
  quint64 ubufGen = 0;
  int ubufSlots = 0;
  quint32 stride = 0;
  std::unique_ptr<QRhiSampler> sampler;
  std::unique_ptr<QRhiSampler> lutSampler;
  std::unique_ptr<QRhiTexture> dummy;
  std::unique_ptr<QRhiTexture> dummyLut;
  std::unique_ptr<QRhiBuffer> layoutUbuf; // only gives pipelines a binding layout to be compatible with
  std::unique_ptr<QRhiShaderResourceBindings> layoutSrb;

  // the blend target: RGBA16F, linear working space
  std::unique_ptr<QRhiTexture> canvas;
  std::unique_ptr<QRhiTextureRenderTarget> canvasRt;
  std::unique_ptr<QRhiRenderPassDescriptor> canvasRpd;
  std::unique_ptr<QRhiGraphicsPipeline> canvasPipeline;
  quint64 canvasGen = 0;

  // the delivery encoding of it: RGBA8 / RGB10A2
  std::unique_ptr<QRhiTexture> output;
  std::unique_ptr<QRhiTextureRenderTarget> outputRt;
  std::unique_ptr<QRhiRenderPassDescriptor> outputRpd;
  std::unique_ptr<QRhiGraphicsPipeline> outputPipeline;
  std::unique_ptr<QRhiBuffer> outputUbuf;
  std::unique_ptr<QRhiShaderResourceBindings> outputSrb;
  QRhiTexture* outputSrbCanvas = nullptr;
  QRhiTexture* outputSrbLut = nullptr;
  quint64 outputSrbLutId = 0;
  int outputBits = 0;

  std::unique_ptr<QRhiBuffer> presentUbuf;
  std::unique_ptr<QRhiShaderResourceBindings> presentSrb;
  QRhiTexture* presentSrbCanvas = nullptr;
  QRhiTexture* presentSrbLut = nullptr;
  quint64 presentSrbLutId = 0;
  std::unique_ptr<QRhiGraphicsPipeline> presentPipeline;
  QRhiRenderPassDescriptor* presentRpd = nullptr;

  std::vector<Slot> layerSlots;

  // colour
  std::shared_ptr<ColorManager> manager;
  std::optional<BlendSpace> blendOverride;
  std::optional<Delivery> deliveryOverride;
  Transfer sdrTransfer = Transfer::Srgb;
  bool deliveryEnabled = true;
  ColorState color; // of the last render(), present() draws with it
  QString colorError;
  struct WorkingMatrices {
    Mat3 from = kIdentity3, to = kIdentity3;
    bool ok = false;
    QString error;
  };
  std::map<QString, WorkingMatrices> workingCache;
  struct CachedInput {
    GpuTransform t;
    std::chrono::steady_clock::time_point at;
  };
  std::unordered_map<QString, CachedInput> inputCache; // re-asked every half second so edited / new LUT files are picked up

  // 3D LUT textures by Lut3D::id, least recently used dropped
  struct LutTex {
    std::unique_ptr<QRhiTexture> tex;
    quint64 used = 0;
  };
  std::map<quint64, LutTex> luts;
  quint64 frameNo = 0;

  struct Layer {
    size_t slot;
    Params params;
  };

  explicit Impl(QRhi* r) : rhi(r) {}

  std::unique_ptr<QRhiShaderResourceBindings> makeSrb(QRhiBuffer* ub, QRhiTexture* t0, QRhiTexture* t1, QRhiTexture* lut) {
    std::unique_ptr<QRhiShaderResourceBindings> srb(rhi->newShaderResourceBindings());
    srb->setBindings({
        QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, ub,
                                                                  sizeof(Params)),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, t0, sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, t1, sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage, lut, lutSampler.get()),
    });
    if (!srb->create()) return nullptr;
    return srb;
  }

  std::unique_ptr<QRhiGraphicsPipeline> makePipeline(QRhiRenderPassDescriptor* rpd, const QShader& frag, bool blendOver) {
    std::unique_ptr<QRhiGraphicsPipeline> ps(rhi->newGraphicsPipeline());
    ps->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, frag}});
    QRhiVertexInputLayout input;
    input.setBindings({{2 * sizeof(float)}});
    input.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
    ps->setVertexInputLayout(input);
    ps->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    ps->setCullMode(QRhiGraphicsPipeline::None);
    if (blendOver) {
      // premultiplied source-over: the shader already multiplied colour by alpha * opacity
      QRhiGraphicsPipeline::TargetBlend blend;
      blend.enable = true;
      blend.srcColor = QRhiGraphicsPipeline::One;
      blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
      blend.srcAlpha = QRhiGraphicsPipeline::One;
      blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
      ps->setTargetBlends({blend});
    }
    ps->setShaderResourceBindings(layoutSrb.get());
    ps->setRenderPassDescriptor(rpd);
    if (!ps->create()) return nullptr;
    return ps;
  }

  bool init() {
    vs = loadShader(QStringLiteral("layer.vert.qsb"));
    fs = loadShader(QStringLiteral("layer.frag.qsb"));
    outFs = loadShader(QStringLiteral("output.frag.qsb"));
    if (!vs.isValid() || !fs.isValid() || !outFs.isValid()) return false;
    if (!rhi->isFeatureSupported(QRhi::ThreeDimensionalTextures)) return false;
    stride = static_cast<quint32>(rhi->ubufAligned(sizeof(Params)));

    vbuf.reset(rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, 8 * sizeof(float)));
    if (!vbuf->create()) return false;
    sampler.reset(rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    if (!sampler->create()) return false;
    lutSampler.reset(rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge,
                                     QRhiSampler::ClampToEdge));
    if (!lutSampler->create()) return false;
    dummy.reset(rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
    if (!dummy->create()) return false;
    dummyLut.reset(rhi->newTexture(QRhiTexture::RGBA8, 1, 1, 1, 1, QRhiTexture::ThreeDimensional));
    if (!dummyLut->create()) return false;
    growUbuf(8);
    if (!ubuf) return false;
    presentUbuf.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, stride));
    if (!presentUbuf->create()) return false;
    outputUbuf.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, stride));
    if (!outputUbuf->create()) return false;
    layoutUbuf.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, stride));
    if (!layoutUbuf->create()) return false;
    layoutSrb = makeSrb(layoutUbuf.get(), dummy.get(), dummy.get(), dummyLut.get());
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
    outputSrb.reset();
    presentSrb.reset();
    canvas.reset(rhi->newTexture(QRhiTexture::RGBA16F, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!canvas->create()) return false;
    canvasRt.reset(rhi->newTextureRenderTarget({canvas.get()}));
    canvasRpd.reset(canvasRt->newCompatibleRenderPassDescriptor());
    canvasRt->setRenderPassDescriptor(canvasRpd.get());
    if (!canvasRt->create()) return false;
    canvasPipeline = makePipeline(canvasRpd.get(), fs, true);
    ++canvasGen;
    return canvasPipeline != nullptr;
  }

  bool ensureOutput(QSize size, int bits) {
    if (output && output->pixelSize() == size && outputBits == bits) return true;
    outputPipeline.reset();
    outputRt.reset();
    outputRpd.reset();
    outputSrb.reset();
    output.reset(rhi->newTexture(bits > 8 ? QRhiTexture::RGB10A2 : QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!output->create()) return false;
    outputRt.reset(rhi->newTextureRenderTarget({output.get()}));
    outputRpd.reset(outputRt->newCompatibleRenderPassDescriptor());
    outputRt->setRenderPassDescriptor(outputRpd.get());
    if (!outputRt->create()) return false;
    outputPipeline = makePipeline(outputRpd.get(), outFs, false);
    outputBits = bits;
    return outputPipeline != nullptr;
  }

  // (Re)builds a slot's bindings if what they point at changed
  bool bindSlot(Slot& s, QRhiTexture* t0, QRhiTexture* t1, QRhiTexture* lut, quint64 lutId, quint64 texGen) {
    if (s.srb && s.bound0 == t0 && s.bound1 == t1 && s.bound2 == lut && s.boundLutId == lutId && s.boundGen == texGen && s.boundUbufGen == ubufGen)
      return true;
    s.srb = makeSrb(ubuf.get(), t0, t1, lut);
    s.bound0 = t0;
    s.bound1 = t1;
    s.bound2 = lut;
    s.boundLutId = lutId;
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

  // The 3D texture for a baked LUT (uploaded on first use), or null when it can't be created.
  QRhiTexture* lutTexture(const Lut3D& lut, QRhiResourceUpdateBatch* updates) {
    auto it = luts.find(lut.id);
    if (it == luts.end()) {
      LutTex lt;
      lt.tex.reset(rhi->newTexture(QRhiTexture::RGBA16F, lut.size, lut.size, lut.size, 1, QRhiTexture::ThreeDimensional));
      if (!lt.tex->create()) return nullptr;
      QVarLengthArray<QRhiTextureUploadEntry, 80> slices;
      const size_t sliceValues = static_cast<size_t>(lut.size) * lut.size * 4;
      for (int z = 0; z < lut.size; ++z) {
        QRhiTextureSubresourceUploadDescription sub(lut.rgba.data() + static_cast<size_t>(z) * sliceValues, static_cast<quint32>(sliceValues * sizeof(quint16)));
        sub.setSourceSize(QSize(lut.size, lut.size));
        slices.push_back(QRhiTextureUploadEntry(z, 0, sub));
      }
      QRhiTextureUploadDescription desc;
      desc.setEntries(slices.cbegin(), slices.cend());
      updates->uploadTexture(lt.tex.get(), desc);
      it = luts.emplace(lut.id, std::move(lt)).first;
    }
    it->second.used = frameNo;
    return it->second.tex.get();
  }

  void trimLuts() {
    constexpr size_t kKeep = 12;
    while (luts.size() > kKeep) {
      auto oldest = luts.begin();
      for (auto it = luts.begin(); it != luts.end(); ++it) {
        if (it->second.used < oldest->second.used) oldest = it;
      }
      if (oldest->second.used == frameNo) break; // everything is in use
      luts.erase(oldest);
    }
  }

  void colorFailed(const QString& what, RenderStats& stats) {
    ++stats.colorErrors;
    colorError = what;
  }

  const WorkingMatrices& workingMatrices(const QString& name) {
    auto it = workingCache.find(name);
    if (it != workingCache.end()) return it->second;
    WorkingMatrices w;
    if (!manager) {
      w.error = QStringLiteral("Working space \"%1\" needs a colour manager").arg(name);
    } else {
      w.ok = manager->workingMatrix(name, &w.from, &w.to, &w.error);
    }
    return workingCache.emplace(name, std::move(w)).first->second;
  }

  // Display / delivery transform plans
  OutputPlan builtinPlan(Transfer tf, Primaries target, const ColorState& cs) {
    OutputPlan p;
    p.transfer = tf;
    p.prim = multiply(primariesMatrix(Primaries::Rec709, target), cs.toRec709);
    return p;
  }

  OutputPlan lutPlan(const DisplaySpec& spec, const ColorState& cs, RenderStats& stats) {
    OutputPlan p = builtinPlan(Transfer::Srgb, Primaries::Rec709, cs); // what we fall back to
    if (!manager) {
      colorFailed(QStringLiteral("An OpenColorIO display transform needs a colour manager"), stats);
      return p;
    }
    const GpuTransform t = manager->displayTransform(spec);
    switch (t.kind) {
    case GpuTransform::Kind::Lut:
      p.lut = t.lut;
      break;
    case GpuTransform::Kind::Passthrough: // working space is already the display's
      p.transfer = Transfer::Linear;
      p.prim = kIdentity3;
      break;
    case GpuTransform::Kind::Failed:
      colorFailed(t.error, stats);
      break;
    case GpuTransform::Kind::Default: break;
    }
    return p;
  }

  void resolveColor(const TimelineDoc& doc, RenderStats& stats) {
    ColorState cs;
    const ColorManagement cm = doc.project.colorManagement.value_or(ColorManagement{});
    cs.blend = blendOverride.value_or(cm.blendSpace);
    if (cm.workingSpace && !cm.workingSpace->isEmpty()) {
      const WorkingMatrices& w = workingMatrices(*cm.workingSpace);
      if (w.ok) {
        cs.working = *cm.workingSpace;
        cs.fromRec709 = w.from;
        cs.toRec709 = w.to;
      } else {
        colorFailed(w.error, stats);
      }
    }
    cs.delivery = deliveryOverride.value_or(deliveryFromSpace(cm.outputSpace.value_or(QString()), 8));
    cs.toneMapHdr = !isHdrDelivery(cs.delivery);

    const Delivery& dl = cs.delivery;
    if (!dl.ocioSpace.isEmpty()) {
      cs.out = lutPlan({cs.working, {}, {}, {}, dl.ocioSpace}, cs, stats);
    } else {
      switch (dl.encoding) {
      case Delivery::Encoding::Srgb: cs.out = builtinPlan(Transfer::Srgb, Primaries::Rec709, cs); break;
      case Delivery::Encoding::Rec709: cs.out = builtinPlan(sdrTransfer, Primaries::Rec709, cs); break;
      case Delivery::Encoding::Rec2100Pq: cs.out = builtinPlan(Transfer::Pq, Primaries::Rec2020, cs); break;
      case Delivery::Encoding::Rec2100Hlg: cs.out = builtinPlan(Transfer::Hlg, Primaries::Rec2020, cs); break;
      }
    }

    const QString view = cm.displayView.value_or(QString());
    if (!view.isEmpty() || !cs.working.isEmpty()) {
      // a non-default working space always needs the config's display transform to be shown right
      const auto [display, v] = splitDisplayView(view);
      cs.preview = lutPlan({cs.working, display, v, {}, {}}, cs, stats);
    } else {
      cs.preview = builtinPlan(Transfer::Srgb, Primaries::Rec709, cs);
    }
    color = cs;
  }

  GpuTransform inputTransform(const ItemColor& ic) {
    const InputSpec spec{ic.inputSpace.value_or(QString()), ic.lutPath.value_or(QString()), ic.lutIntensity, color.working};
    const QString key = QStringLiteral("%1|%2|%3|%4|%5").arg(spec.workingSpace, spec.inputSpace, spec.lutPath).arg(spec.lutIntensity);
    const auto now = std::chrono::steady_clock::now();
    const auto it = inputCache.find(key);
    if (it != inputCache.end() && now - it->second.at < std::chrono::milliseconds(500)) return it->second.t;
    if (!manager) {
      GpuTransform t;
      t.kind = GpuTransform::Kind::Failed;
      t.error = QStringLiteral("Item colour settings need a colour manager");
      return t;
    }
    GpuTransform t = manager->inputTransform(spec);
    inputCache[key] = {t, now};
    return t;
  }

  // Source values -> working space for one layer. `base` is how its stream is tagged; OCIO settings
  // on the item override it.
  LayerColor layerColor(const Item* item, Transfer base, ColorPrim prim, bool hdrTagged, RenderStats& stats) {
    LayerColor lc;
    lc.transfer = base;
    lc.prim = multiply(color.fromRec709, primariesMatrix(primariesFor(prim), Primaries::Rec709));
    lc.toneMap = hdrTagged && color.toneMapHdr;
    if (!item || !item->color) return lc;
    const ItemColor& ic = *item->color;
    if ((!ic.inputSpace || ic.inputSpace->isEmpty()) && (!ic.lutPath || ic.lutPath->isEmpty())) return lc;
    const GpuTransform t = inputTransform(ic);
    switch (t.kind) {
    case GpuTransform::Kind::Passthrough: // the source is already linear working-space light
      lc.transfer = Transfer::Linear;
      lc.prim = kIdentity3;
      lc.toneMap = false;
      break;
    case GpuTransform::Kind::Lut:
      lc.lut = t.lut;
      lc.toneMap = false;
      break;
    case GpuTransform::Kind::Failed:
      colorFailed(t.error, stats);
      break;
    case GpuTransform::Kind::Default: break;
    }
    return lc;
  }

  // The colour the canvas is cleared to: the project's background, in the canvas' encoding
  QColor background(const QString& css) const {
    QColor bg(css);
    if (!bg.isValid()) bg = Qt::black;
    if (color.blend == BlendSpace::Display) return bg;
    const Rgbd lin = mulMat3(color.fromRec709, {toLinear(Transfer::Srgb, bg.redF()), toLinear(Transfer::Srgb, bg.greenF()), toLinear(Transfer::Srgb, bg.blueF())});
    return QColor::fromRgbF(static_cast<float>(std::clamp(lin[0], 0.0, 1.0)), static_cast<float>(std::clamp(lin[1], 0.0, 1.0)),
                            static_cast<float>(std::clamp(lin[2], 0.0, 1.0)));
  }
};

Compositor::Compositor(QRhi* rhi) : d(std::make_unique<Impl>(rhi)) { d->ready = d->init(); }
Compositor::~Compositor() = default;

bool Compositor::ok() const { return d->ready; }
bool Compositor::gpuCapable() const { return runsOnSharedDevice(d->rhi); }
void Compositor::setGpuEnabled(bool enabled) { d->gpuEnabled = enabled; }
void Compositor::setColorManager(std::shared_ptr<ColorManager> manager) {
  d->manager = std::move(manager);
  d->workingCache.clear();
  d->inputCache.clear();
}
void Compositor::setBlendSpace(std::optional<BlendSpace> space) { d->blendOverride = space; }
void Compositor::setDelivery(std::optional<Delivery> delivery) { d->deliveryOverride = std::move(delivery); }
void Compositor::setSdrTransfer(Transfer transfer) { d->sdrTransfer = transfer; }
void Compositor::setDeliveryEnabled(bool enabled) { d->deliveryEnabled = enabled; }
QString Compositor::lastColorError() const { return d->colorError; }
void Compositor::invalidate() {
  for (Impl::Slot& slot : d->layerSlots) slot.releaseContent();
}
QRhiTexture* Compositor::canvasTexture() const { return d->output.get(); }
QRhiTexture* Compositor::linearCanvas() const { return d->canvas.get(); }
QSize Compositor::canvasSize() const { return d->canvas ? d->canvas->pixelSize() : QSize(); }
Delivery Compositor::delivery() const { return d->color.delivery; }

RenderStats Compositor::render(QRhiCommandBuffer* cb, const TimelineDoc& doc, Frame frame, MediaProvider& media) {
  Impl& s = *d;
  RenderStats stats;
  if (!s.ready) return stats;
  const QSize size(static_cast<int>(doc.project.width), static_cast<int>(doc.project.height));
  if (size.isEmpty() || !s.ensureCanvas(size)) return stats;
  ++s.frameNo;
  s.colorError.clear();
  s.resolveColor(doc, stats);
  const ColorState& cs = s.color;
  if (s.deliveryEnabled && !s.ensureOutput(size, cs.delivery.bits)) return stats;

  QRhiResourceUpdateBatch* updates = s.rhi->nextResourceUpdateBatch();
  if (!s.staticUploaded) {
    static const float quad[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    updates->uploadStaticBuffer(s.vbuf.get(), quad);
    const quint8 white[4] = {255, 255, 255, 255};
    QRhiTextureSubresourceUploadDescription px(white, 4);
    px.setSourceSize(QSize(1, 1));
    updates->uploadTexture(s.dummy.get(), QRhiTextureUploadDescription({QRhiTextureUploadEntry(0, 0, px)}));
    updates->uploadTexture(s.dummyLut.get(), QRhiTextureUploadDescription({QRhiTextureUploadEntry(0, 0, px)}));
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

  const auto lutFor = [&](const LayerColor& lc) -> QRhiTexture* {
    if (!lc.lut) return s.dummyLut.get();
    QRhiTexture* t = s.lutTexture(*lc.lut, updates);
    return t ? t : s.dummyLut.get();
  };

  // an uploaded RGBA layer: `changed` says the pixels differ from what the slot's texture holds
  const auto rgbaLayer = [&](const QImage& img, bool changed, Impl::Slot& slot, const QMatrix4x4& model, const UvMap& uv, const ColorMatrix& fx,
                             double opacity, Mode mode, const LayerColor& lc) {
    changed |= s.ensureRgba(slot, img.size());
    if (!slot.rgba) return false;
    slot.kind = Impl::Slot::Kind::Rgba;
    if (changed) {
      QRhiTextureSubresourceUploadDescription sub(img);
      updates->uploadTexture(slot.rgba.get(), QRhiTextureUploadDescription({QRhiTextureUploadEntry(0, 0, sub)}));
    }
    QRhiTexture* lutTex = lutFor(lc);
    if (!s.bindSlot(slot, slot.rgba.get(), s.dummy.get(), lutTex, lc.lut ? lc.lut->id : 0, slot.rgbaGen)) return false;
    Impl::Layer l;
    l.slot = static_cast<size_t>(&slot - s.layerSlots.data());
    fillLayer(l.params, proj * model, uv, kIdentityColor, fx, opacity, mode, lc, cs);
    layers.push_back(l);
    return true;
  };

  // text, shapes, captions: painted by QPainter into sRGB-encoded premultiplied pixels
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
    const LayerColor lc = s.layerColor(nullptr, Transfer::Srgb, ColorPrim::Unspecified, false, stats);
    if (rgbaLayer(r->image, changed, slot, model, UvMap{}, kIdentityColor, opacity, Premultiplied, lc)) slot.key = r->key;
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
      // still images are sRGB-encoded files
      const LayerColor lc = s.layerColor(item, Transfer::Srgb, ColorPrim::Unspecified, false, stats);
      const bool changed = slot.kind != Impl::Slot::Kind::Rgba || slot.still != v.image;
      slot.frame.reset();
      slot.converted = QImage();
      if (rgbaLayer(*v.image, changed, slot, model, UvMap{}, fx, opacity, Straight, lc)) {
        slot.still = v.image;
        ++stats.cpuLayers;
      }
      continue;
    }

    const UvMap uv = uvForRotation(v.rotation);
    const VideoFrame& vf = *v.video;
    const ColorTrc trc = vf.gpu ? vf.gpu->trc : vf.trc;
    const ColorPrim prim = vf.gpu ? vf.gpu->prim : vf.prim;
    const LayerColor lc = s.layerColor(item, transferFor(trc, s.sdrTransfer), prim, isHdr(trc), stats);
    if (vf.gpu && s.gpuEnabled) {
      const GpuFrame& g = *vf.gpu;
      if (!slot.planes) slot.planes = std::make_unique<GpuPlanes>();
      if (slot.planes->ensure(s.rhi, g.format, QSize(g.width, g.height))) {
        // the plane views are new objects whenever ensure() rebuilt them; the generation tells
        const quint64 planesGen = slot.planes->generation();
        const bool fresh = slot.kind != Impl::Slot::Kind::Planes || slot.planesGen != planesGen || slot.frame != v.video;
        slot.planesGen = planesGen;
        if (s.bindSlot(slot, slot.planes->luma(), slot.planes->chroma(), lutFor(lc), lc.lut ? lc.lut->id : 0, planesGen)) {
          slot.kind = Impl::Slot::Kind::Planes;
          if (fresh) copies.emplace_back(slot.planes.get(), &g);
          slot.frame = v.video;
          slot.still.reset();
          slot.converted = QImage();
          Impl::Layer l;
          l.slot = static_cast<size_t>(&slot - s.layerSlots.data());
          fillLayer(l.params, proj * model, uv, yuvToRgb(g.kr, g.kb, g.fullRange, g.bitDepth), fx, opacity, Yuv, lc, cs);
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
    if (rgbaLayer(*img, changed, slot, model, uv, fx, opacity, Straight, lc)) {
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
    if (slot.boundUbufGen != s.ubufGen) s.bindSlot(slot, slot.bound0, slot.bound1, slot.bound2, slot.boundLutId, slot.boundGen);
    updates->updateDynamicBuffer(s.ubuf.get(), static_cast<quint32>(l.slot) * s.stride, sizeof(Params), &l.params);
  }

  // the output pass's inputs: its LUT and its bindings
  QRhiTexture* outLut = cs.out.lut ? s.lutTexture(*cs.out.lut, updates) : nullptr;
  if (cs.preview.lut) s.lutTexture(*cs.preview.lut, updates); // keeps it alive for present()
  if (!outLut) outLut = s.dummyLut.get();
  s.trimLuts();

  if (!copies.empty()) {
    cb->beginExternal();
    for (const auto& [planes, gpu] : copies) planes->copyFrom(*gpu);
    cb->endExternal();
  }

  // the project's background, opaque (the reference fills it before drawing anything)
  cb->beginPass(s.canvasRt.get(), s.background(doc.project.styleConfig.backgroundColor), {1.0f, 0}, updates);
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

  if (s.deliveryEnabled) {
    const quint64 outLutId = cs.out.lut ? cs.out.lut->id : 0;
    if (!s.outputSrb || s.outputSrbCanvas != s.canvas.get() || s.outputSrbLut != outLut || s.outputSrbLutId != outLutId) {
      s.outputSrb = s.makeSrb(s.outputUbuf.get(), s.canvas.get(), s.dummy.get(), outLut);
      s.outputSrbCanvas = s.canvas.get();
      s.outputSrbLut = outLut;
      s.outputSrbLutId = outLutId;
    }
    QMatrix4x4 model;
    model.scale(static_cast<float>(size.width()), static_cast<float>(size.height()));
    Params params;
    fillOutput(params, proj * model, cs.out, cs);
    QRhiResourceUpdateBatch* u2 = s.rhi->nextResourceUpdateBatch();
    u2->updateDynamicBuffer(s.outputUbuf.get(), 0, sizeof params, &params);
    cb->beginPass(s.outputRt.get(), Qt::black, {1.0f, 0}, u2);
    if (s.outputPipeline && s.outputSrb) {
      cb->setGraphicsPipeline(s.outputPipeline.get());
      cb->setViewport({0, 0, static_cast<float>(size.width()), static_cast<float>(size.height())});
      cb->setVertexInput(0, 1, &vertices);
      const QRhiCommandBuffer::DynamicOffset dyn{0, 0};
      cb->setShaderResources(s.outputSrb.get(), 1, &dyn);
      cb->draw(4);
    }
    cb->endPass();
  }
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
    s.presentPipeline = s.makePipeline(s.presentRpd, s.outFs, false);
  }
  const OutputPlan& plan = s.color.preview;
  QRhiTexture* lutTex = plan.lut ? s.lutTexture(*plan.lut, updates) : nullptr;
  if (!lutTex) lutTex = s.dummyLut.get();
  const quint64 lutId = plan.lut ? plan.lut->id : 0;
  if (!s.presentSrb || s.presentSrbCanvas != s.canvas.get() || s.presentSrbLut != lutTex || s.presentSrbLutId != lutId) {
    s.presentSrb = s.makeSrb(s.presentUbuf.get(), s.canvas.get(), s.dummy.get(), lutTex);
    s.presentSrbCanvas = s.canvas.get();
    s.presentSrbLut = lutTex;
    s.presentSrbLutId = lutId;
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
  fillOutput(params, proj * model, plan, s.color);
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
