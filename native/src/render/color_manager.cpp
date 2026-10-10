#include "render/color_manager.h"

#include "render/color.h"

#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QtGlobal>
#include <qfloat16.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>

#ifdef SF_HAVE_OCIO
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <OpenColorIO/OpenColorIO.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
namespace OCIO = OCIO_NAMESPACE;
#endif

namespace sf::render {

namespace {

// Display LUT addressing: coord = (log2(v + 2^-10) + 10) / 16, i.e. 16 stops from 2^-10 to 2^6 SDR whites.
constexpr float kShaperFloor = 1.0f / 1024.0f;
constexpr float kShaperStops = 16.0f;
constexpr float kShaperBias = 10.0f;

std::uint64_t nextLutId() {
  static std::atomic<std::uint64_t> counter{0};
  return ++counter;
}

std::uint16_t toHalf(float v) {
  if (!(v == v)) v = 0; // NaN
  v = std::clamp(v, -65504.0f, 65504.0f);
  const qfloat16 h(v);
  std::uint16_t bits;
  std::memcpy(&bits, &h, sizeof bits);
  return bits;
}

} // namespace

float ColorManager::shaperEncode(float v) { return std::clamp((std::log2(std::max(v, 0.0f) + kShaperFloor) + kShaperBias) / kShaperStops, 0.0f, 1.0f); }
float ColorManager::shaperDecode(float c) { return std::max(std::exp2(c * kShaperStops - kShaperBias) - kShaperFloor, 0.0f); }

#ifdef SF_HAVE_OCIO

namespace {

QString exceptionText(const std::exception& e) { return QString::fromUtf8(e.what()); }

// An input transform as CPU processors: optional LUT, then either an OCIO space -> working
// conversion or the built-in sRGB decode (+ gamut matrix when the working space isn't Rec.709).
struct InputChain {
  OCIO::ConstCPUProcessorRcPtr lut;
  OCIO::ConstCPUProcessorRcPtr space;
  bool spaceNoOp = false;
  Mat3 toWorking = kIdentity3; // built-in path only
  double mix = 1;

  void apply(float* rgb, std::size_t n) const {
    if (lut) {
      std::vector<float> lutted(rgb, rgb + n * 3);
      OCIO::PackedImageDesc img(lutted.data(), static_cast<long>(n), 1, 3);
      lut->apply(img);
      if (mix >= 1.0) {
        std::copy(lutted.begin(), lutted.end(), rgb);
      } else {
        const float t = static_cast<float>(mix);
        for (std::size_t i = 0; i < n * 3; ++i) rgb[i] += (lutted[i] - rgb[i]) * t;
      }
    }
    if (space) {
      if (!spaceNoOp) {
        OCIO::PackedImageDesc img(rgb, static_cast<long>(n), 1, 3);
        space->apply(img);
      }
      return;
    }
    for (std::size_t i = 0; i < n; ++i) {
      Rgbd c = {toLinear(Transfer::Srgb, rgb[i * 3]), toLinear(Transfer::Srgb, rgb[i * 3 + 1]), toLinear(Transfer::Srgb, rgb[i * 3 + 2])};
      c = mulMat3(toWorking, c);
      for (std::size_t k = 0; k < 3; ++k) rgb[i * 3 + k] = static_cast<float>(c[k]);
    }
  }
};

} // namespace

struct ColorManager::Impl {
  OCIO::ConstConfigRcPtr config;
  QString source;
  mutable QMutex m;
  std::map<QString, GpuTransform> inputCache;
  std::map<QString, GpuTransform> displayCache;

  QString linearRec709() const {
    static const char* const names[] = {"Linear Rec.709 (sRGB)", "lin_rec709", "Linear Rec.709", "lin_srgb", "Linear sRGB", "linear"};
    for (const char* n : names) {
      if (config->getColorSpace(n)) return QString::fromUtf8(n);
    }
    return {};
  }

  bool buildInput(const InputSpec& s, InputChain& chain, QString* error) const {
    try {
      const bool hasLut = !s.lutPath.isEmpty();
      QString working = s.workingSpace;
      if (!working.isEmpty() && !config->getColorSpace(working.toUtf8().constData())) {
        *error = QStringLiteral("Working space \"%1\" is not in the colour config").arg(working);
        return false;
      }
      if (!s.inputSpace.isEmpty() && !config->getColorSpace(s.inputSpace.toUtf8().constData())) {
        *error = QStringLiteral("Input colour space \"%1\" is not in the colour config").arg(s.inputSpace);
        return false;
      }
      if (working.isEmpty()) working = linearRec709();
      if (working.isEmpty() && !s.inputSpace.isEmpty()) {
        *error = QStringLiteral("The colour config has no linear Rec.709 space to relate sources to the built-in working space");
        return false;
      }
      if (hasLut) {
        const QFileInfo fi(s.lutPath);
        if (!fi.exists() || !fi.isFile()) {
          *error = QStringLiteral("LUT file not found: %1").arg(s.lutPath);
          return false;
        }
        OCIO::FileTransformRcPtr ft = OCIO::FileTransform::Create();
        ft->setSrc(fi.absoluteFilePath().toUtf8().constData());
        ft->setInterpolation(OCIO::INTERP_BEST);
        chain.lut = config->getProcessor(ft)->getDefaultCPUProcessor();
        chain.mix = std::clamp(s.lutIntensity, 0.0, 1.0);
      }
      if (!s.inputSpace.isEmpty()) {
        const OCIO::ConstProcessorRcPtr p = config->getProcessor(s.inputSpace.toUtf8().constData(), working.toUtf8().constData());
        chain.space = p->getDefaultCPUProcessor();
        chain.spaceNoOp = p->isNoOp();
      } else if (!s.workingSpace.isEmpty()) {
        std::array<double, 9> from{}, to{};
        if (!workingMatrix(s.workingSpace, &from, &to, error)) return false;
        chain.toWorking = from;
      }
      return true;
    } catch (const std::exception& e) {
      *error = exceptionText(e);
      return false;
    }
  }

  bool workingMatrix(const QString& workingSpace, std::array<double, 9>* from, std::array<double, 9>* to, QString* error) const {
    *from = *to = kIdentity3;
    if (workingSpace.isEmpty()) return true;
    try {
      const QString lin = linearRec709();
      if (lin.isEmpty()) {
        if (error) *error = QStringLiteral("The colour config has no linear Rec.709 space to relate the working space to");
        return false;
      }
      if (!config->getColorSpace(workingSpace.toUtf8().constData())) {
        if (error) *error = QStringLiteral("Working space \"%1\" is not in the colour config").arg(workingSpace);
        return false;
      }
      const auto p = config->getProcessor(lin.toUtf8().constData(), workingSpace.toUtf8().constData())->getDefaultCPUProcessor();
      // the basis vectors through the conversion give its columns (it is a matrix for any scene-linear pair)
      float basis[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
      OCIO::PackedImageDesc img(basis, 3, 1, 3);
      p->apply(img);
      for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) (*from)[static_cast<size_t>(r * 3 + c)] = basis[c * 3 + r];
      }
      *to = inverse(*from);
      return true;
    } catch (const std::exception& e) {
      if (error) *error = exceptionText(e);
      return false;
    }
  }

  OCIO::ConstProcessorRcPtr displayProcessor(const DisplaySpec& s, QString* error) const {
    QString working = s.workingSpace.isEmpty() ? linearRec709() : s.workingSpace;
    if (working.isEmpty()) {
      *error = QStringLiteral("The colour config has no linear Rec.709 space to relate the built-in working space to");
      return nullptr;
    }
    if (!s.outputSpace.isEmpty()) {
      if (!config->getColorSpace(s.outputSpace.toUtf8().constData())) {
        *error = QStringLiteral("Output colour space \"%1\" is not in the colour config").arg(s.outputSpace);
        return nullptr;
      }
      return config->getProcessor(working.toUtf8().constData(), s.outputSpace.toUtf8().constData());
    }
    QString display = s.display.isEmpty() ? QString::fromUtf8(config->getDefaultDisplay()) : s.display;
    QString view = s.view.isEmpty() ? QString::fromUtf8(config->getDefaultView(display.toUtf8().constData())) : s.view;
    OCIO::DisplayViewTransformRcPtr dvt = OCIO::DisplayViewTransform::Create();
    dvt->setSrc(working.toUtf8().constData());
    dvt->setDisplay(display.toUtf8().constData());
    dvt->setView(view.toUtf8().constData());
    if (s.look.isEmpty()) return config->getProcessor(dvt);
    OCIO::LegacyViewingPipelineRcPtr pipe = OCIO::LegacyViewingPipeline::Create();
    pipe->setDisplayViewTransform(dvt);
    pipe->setLooksOverrideEnabled(true);
    pipe->setLooksOverride(s.look.toUtf8().constData());
    return pipe->getProcessor(config);
  }
};

namespace {
// Rows of `n` pixels, r fastest
std::vector<float> gridRgb(int size, bool shaped) {
  std::vector<float> v(static_cast<size_t>(size) * size * size * 3);
  size_t i = 0;
  for (int b = 0; b < size; ++b) {
    for (int g = 0; g < size; ++g) {
      for (int r = 0; r < size; ++r) {
        const float c[3] = {float(r) / float(size - 1), float(g) / float(size - 1), float(b) / float(size - 1)};
        for (float x : c) v[i++] = shaped ? ColorManager::shaperDecode(x) : x;
      }
    }
  }
  return v;
}

std::shared_ptr<Lut3D> packLut(const std::vector<float>& rgb, int size, bool clamp01) {
  auto lut = std::make_shared<Lut3D>();
  lut->id = nextLutId();
  lut->size = size;
  const size_t n = static_cast<size_t>(size) * size * size;
  lut->rgba.resize(n * 4);
  for (size_t i = 0; i < n; ++i) {
    for (size_t k = 0; k < 3; ++k) {
      const float v = rgb[i * 3 + k];
      lut->rgba[i * 4 + k] = toHalf(clamp01 ? std::clamp(v, 0.0f, 1.0f) : v);
    }
    lut->rgba[i * 4 + 3] = toHalf(1.0f);
  }
  return lut;
}
} // namespace

ColorManager::ColorManager() : d(std::make_unique<Impl>()) {}
ColorManager::~ColorManager() = default;

bool ColorManager::available() { return true; }
QString ColorManager::version() { return QString::fromUtf8(OCIO::GetVersion()); }

std::unique_ptr<ColorManager> ColorManager::create(const QString& source, QString* error) {
  std::unique_ptr<ColorManager> cm(new ColorManager());
  try {
    const QString src = source.isEmpty() ? QStringLiteral("ocio://studio-config-latest") : source;
    cm->d->config = OCIO::Config::CreateFromFile(src.toUtf8().constData());
    cm->d->source = src;
    cm->d->config->validate();
  } catch (const std::exception& e) {
    if (error) *error = QStringLiteral("Can't load colour config \"%1\": %2").arg(source, exceptionText(e));
    return nullptr;
  }
  return cm;
}

QString ColorManager::source() const { return d->source; }
QString ColorManager::description() const { return QString::fromUtf8(d->config->getDescription()); }

QStringList ColorManager::colorSpaces() const {
  QStringList out;
  for (int i = 0; i < d->config->getNumColorSpaces(); ++i) out << QString::fromUtf8(d->config->getColorSpaceNameByIndex(i));
  return out;
}
bool ColorManager::hasColorSpace(const QString& name) const { return d->config->getColorSpace(name.toUtf8().constData()) != nullptr; }
QStringList ColorManager::displays() const {
  QStringList out;
  for (int i = 0; i < d->config->getNumDisplays(); ++i) out << QString::fromUtf8(d->config->getDisplay(i));
  return out;
}
QString ColorManager::defaultDisplay() const { return QString::fromUtf8(d->config->getDefaultDisplay()); }
QStringList ColorManager::views(const QString& display) const {
  QStringList out;
  const QByteArray disp = display.toUtf8();
  for (int i = 0; i < d->config->getNumViews(disp.constData()); ++i) out << QString::fromUtf8(d->config->getView(disp.constData(), i));
  return out;
}
QString ColorManager::defaultView(const QString& display) const { return QString::fromUtf8(d->config->getDefaultView(display.toUtf8().constData())); }
QStringList ColorManager::looks() const {
  QStringList out;
  for (int i = 0; i < d->config->getNumLooks(); ++i) out << QString::fromUtf8(d->config->getLookNameByIndex(i));
  return out;
}
QString ColorManager::linearRec709Space() const { return d->linearRec709(); }

bool ColorManager::workingMatrix(const QString& ws, std::array<double, 9>* from, std::array<double, 9>* to, QString* error) const {
  return d->workingMatrix(ws, from, to, error);
}

GpuTransform ColorManager::inputTransform(const InputSpec& s) {
  GpuTransform out;
  if (s.inputSpace.isEmpty() && s.lutPath.isEmpty()) return out; // Default: the compositor decodes by the stream tags
  QString key = QStringLiteral("%1|%2|%3|%4").arg(s.workingSpace, s.inputSpace, s.lutPath).arg(qRound(s.lutIntensity * 1000));
  if (!s.lutPath.isEmpty()) {
    const QFileInfo fi(s.lutPath);
    key += QStringLiteral("|%1|%2").arg(fi.lastModified().toMSecsSinceEpoch()).arg(fi.size()); // exists() false -> "-1|0" bakes a failure
  }
  {
    const QMutexLocker lock(&d->m);
    const auto it = d->inputCache.find(key);
    if (it != d->inputCache.end()) return it->second;
  }
  InputChain chain;
  QString err;
  if (!d->buildInput(s, chain, &err)) {
    out.kind = GpuTransform::Kind::Failed;
    out.error = err;
  } else if (s.lutPath.isEmpty() && chain.space && chain.spaceNoOp) {
    out.kind = GpuTransform::Kind::Passthrough;
  } else if (s.lutPath.isEmpty() && !chain.space && chain.toWorking == kIdentity3) {
    out.kind = GpuTransform::Kind::Default; // only a Rec.709 working space: nothing to bake
  } else {
    try {
      std::vector<float> v = gridRgb(kLutSize, false);
      chain.apply(v.data(), v.size() / 3);
      out.kind = GpuTransform::Kind::Lut;
      out.lut = packLut(v, kLutSize, false);
    } catch (const std::exception& e) {
      out.kind = GpuTransform::Kind::Failed;
      out.error = exceptionText(e);
    }
  }
  const QMutexLocker lock(&d->m);
  return d->inputCache.emplace(key, out).first->second;
}

GpuTransform ColorManager::displayTransform(const DisplaySpec& s) {
  const QString key = QStringLiteral("%1|%2|%3|%4|%5").arg(s.workingSpace, s.display, s.view, s.look, s.outputSpace);
  {
    const QMutexLocker lock(&d->m);
    const auto it = d->displayCache.find(key);
    if (it != d->displayCache.end()) return it->second;
  }
  GpuTransform out;
  QString err;
  try {
    const OCIO::ConstProcessorRcPtr p = d->displayProcessor(s, &err);
    if (!p) {
      out.kind = GpuTransform::Kind::Failed;
      out.error = err;
    } else if (p->isNoOp()) {
      out.kind = GpuTransform::Kind::Passthrough;
    } else {
      std::vector<float> v = gridRgb(kLutSize, true);
      OCIO::PackedImageDesc img(v.data(), static_cast<long>(v.size() / 3), 1, 3);
      p->getDefaultCPUProcessor()->apply(img);
      out.kind = GpuTransform::Kind::Lut;
      out.lut = packLut(v, kLutSize, true);
    }
  } catch (const std::exception& e) {
    out.kind = GpuTransform::Kind::Failed;
    out.error = exceptionText(e);
  }
  const QMutexLocker lock(&d->m);
  return d->displayCache.emplace(key, out).first->second;
}

bool ColorManager::applyInputCpu(const InputSpec& s, float* rgb, std::size_t pixels, QString* error) {
  InputChain chain;
  QString err;
  if (!d->buildInput(s, chain, &err)) {
    if (error) *error = err;
    return false;
  }
  try {
    chain.apply(rgb, pixels);
  } catch (const std::exception& e) {
    if (error) *error = exceptionText(e);
    return false;
  }
  return true;
}

bool ColorManager::applyDisplayCpu(const DisplaySpec& s, float* rgb, std::size_t pixels, QString* error) {
  QString err;
  try {
    const OCIO::ConstProcessorRcPtr p = d->displayProcessor(s, &err);
    if (!p) {
      if (error) *error = err;
      return false;
    }
    OCIO::PackedImageDesc img(rgb, static_cast<long>(pixels), 1, 3);
    p->getDefaultCPUProcessor()->apply(img);
  } catch (const std::exception& e) {
    if (error) *error = exceptionText(e);
    return false;
  }
  return true;
}

#else // no OpenColorIO: the built-in colour path only

struct ColorManager::Impl {};
ColorManager::ColorManager() : d(std::make_unique<Impl>()) {}
ColorManager::~ColorManager() = default;
bool ColorManager::available() { return false; }
QString ColorManager::version() { return {}; }
std::unique_ptr<ColorManager> ColorManager::create(const QString&, QString* error) {
  if (error) *error = QStringLiteral("SplitFrame was built without OpenColorIO");
  return nullptr;
}
QString ColorManager::source() const { return {}; }
QString ColorManager::description() const { return {}; }
QStringList ColorManager::colorSpaces() const { return {}; }
bool ColorManager::hasColorSpace(const QString&) const { return false; }
QStringList ColorManager::displays() const { return {}; }
QString ColorManager::defaultDisplay() const { return {}; }
QStringList ColorManager::views(const QString&) const { return {}; }
QString ColorManager::defaultView(const QString&) const { return {}; }
QStringList ColorManager::looks() const { return {}; }
QString ColorManager::linearRec709Space() const { return {}; }
bool ColorManager::workingMatrix(const QString& ws, std::array<double, 9>* from, std::array<double, 9>* to, QString* error) const {
  *from = *to = kIdentity3;
  if (ws.isEmpty()) return true;
  if (error) *error = QStringLiteral("SplitFrame was built without OpenColorIO");
  return false;
}
GpuTransform ColorManager::inputTransform(const InputSpec&) { return {GpuTransform::Kind::Failed, nullptr, QStringLiteral("SplitFrame was built without OpenColorIO")}; }
GpuTransform ColorManager::displayTransform(const DisplaySpec&) { return {GpuTransform::Kind::Failed, nullptr, QStringLiteral("SplitFrame was built without OpenColorIO")}; }
bool ColorManager::applyInputCpu(const InputSpec&, float*, std::size_t, QString* error) {
  if (error) *error = QStringLiteral("SplitFrame was built without OpenColorIO");
  return false;
}
bool ColorManager::applyDisplayCpu(const DisplaySpec&, float*, std::size_t, QString* error) {
  if (error) *error = QStringLiteral("SplitFrame was built without OpenColorIO");
  return false;
}

#endif

} // namespace sf::render
