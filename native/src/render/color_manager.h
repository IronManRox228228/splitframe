#pragma once

// OpenColorIO behind a small, OCIO-free interface: loads a config, lists what it offers and turns
// colour transforms into what the compositor can sample.
//
// GPU strategy: processors are *baked* into 3D LUTs (65^3 RGBA16F) that layer.frag / output.frag
// sample, instead of generating OCIO's own HLSL/GLSL. OCIO's generated code would have to be compiled
// to a .qsb at run time (QShaderBaker + a recompile per processor and per pipeline variant) and its
// textures / uniforms bound by name into QRhi's fixed binding layout; a baked LUT is one extra
// sampler on the existing pipelines and costs the same whatever the transform is. The price is
// interpolation error, which is why the two kinds of LUT use different domains:
//   * input LUT:   domain = the source's encoded values 0..1 (camera log curves are smooth there),
//                  range = linear light in the working space (half floats hold up to 65504).
//   * display LUT: domain = scene-linear light through a log shaper (kShaperStops stops from
//                  2^-10 to 2^6, so highlights above 1.0 are covered), range = display-encoded 0..1.
// Transforms OCIO reports as no-ops never become LUTs (the shader skips them): exact.
// The CPU processors the LUTs are baked from are also exposed (apply*Cpu) as the exact reference.
//
// What a "working space" is: empty means the compositor's built-in linear Rec.709 (sRGB primaries),
// which needs no config. A name is a scene-linear OCIO colour space of the loaded config (ACEScg, ...);
// then every source and the display transform go through the config. Needs a config that has a
// "Linear Rec.709 (sRGB)"-style space (the built-in studio and CG configs do).
//
// Thread-safe: the render thread asks, the UI thread lists.

#include <QString>
#include <QStringList>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace sf::render {

// A baked cube. Texel (x,y,z) = (r,g,b) grid index, x fastest; `rgba` is RGBA16F bits, N*N*N*4 values.
struct Lut3D {
  std::uint64_t id = 0; // unique per bake; textures are cached by it
  int size = 0;
  std::vector<std::uint16_t> rgba;
};

struct GpuTransform {
  enum class Kind {
    Default,     // nothing configured: the compositor's built-in path applies
    Passthrough, // OCIO says the transform is a no-op: skip it (input: values are already working-space linear)
    Lut,         // sample `lut`
    Failed,      // `error` says why; the compositor falls back to the built-in path
  };
  Kind kind = Kind::Default;
  std::shared_ptr<const Lut3D> lut;
  QString error;
};

// Source pixels -> linear working space.
struct InputSpec {
  QString inputSpace;       // OCIO colour space of the pixels (after `lutPath`, if any); empty = built-in sRGB-style SDR
  QString lutPath;          // .cube .3dl .clf .spi3d .csp ... applied to the source values first (OCIO FileTransform)
  double lutIntensity = 1;  // 0..1 mix of the source value and the LUT's result, in the source's encoding
  QString workingSpace;     // empty = built-in linear Rec.709
};

// Linear working space -> display-encoded values.
struct DisplaySpec {
  QString workingSpace;
  QString display;      // with `view`: OCIO display / view transform (what a monitor needs)
  QString view;
  QString look;         // optional look(s) applied in the view transform ("+Name, -Other")
  QString outputSpace;  // alternative to display/view: a display-referred colour space (delivery)
};

class ColorManager {
public:
  static bool available(); // built with OpenColorIO
  static QString version(); // "2.6.0", or empty without OCIO
  // `source`: empty or "ocio://studio-config-latest" (built-in, default), "ocio://cg-config-latest",
  // or a path to a config.ocio / .ocioz. Null with *error set when it can't be loaded.
  static std::unique_ptr<ColorManager> create(const QString& source = {}, QString* error = nullptr);
  ~ColorManager();
  ColorManager(const ColorManager&) = delete;
  ColorManager& operator=(const ColorManager&) = delete;

  QString source() const;
  QString description() const;
  QStringList colorSpaces() const;
  bool hasColorSpace(const QString& name) const;
  QStringList displays() const;
  QString defaultDisplay() const;
  QStringList views(const QString& display) const;
  QString defaultView(const QString& display) const;
  QStringList looks() const;
  // the config's "Linear Rec.709 (sRGB)"-equivalent, or empty when it has none
  QString linearRec709Space() const;

  // 3x3 (row-major, linear light) from the built-in linear Rec.709 into the working space and back.
  // Identity for an empty working space; false with *error when the config can't convert.
  bool workingMatrix(const QString& workingSpace, std::array<double, 9>* fromRec709, std::array<double, 9>* toRec709, QString* error = nullptr) const;

  // GPU transforms; cached per spec (and per LUT file timestamp), failures too.
  GpuTransform inputTransform(const InputSpec& spec);
  GpuTransform displayTransform(const DisplaySpec& spec);

  // Exact CPU versions of the same transforms, in place on `pixels` RGB floats (3 per pixel).
  // Display side: working-space linear in, display-encoded out. False with *error on failure.
  bool applyInputCpu(const InputSpec& spec, float* rgb, std::size_t pixels, QString* error = nullptr);
  bool applyDisplayCpu(const DisplaySpec& spec, float* rgb, std::size_t pixels, QString* error = nullptr);

  // The log shaper the display LUT is addressed through (shaders/output.frag has the same two lines):
  // linear light -> 0..1 and back.
  static float shaperEncode(float linear);
  static float shaperDecode(float coord);

  static constexpr int kLutSize = 65;

private:
  ColorManager();
  struct Impl;
  std::unique_ptr<Impl> d;
};

} // namespace sf::render
