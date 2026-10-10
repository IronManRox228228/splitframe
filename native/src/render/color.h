#pragma once

// Colour maths shared by the compositor, its shaders' CPU reference and the tests: transfer
// functions, primaries matrices and the delivery targets.
//
// The compositor works in scene-linear light with premultiplied alpha (RGBA16F canvas). "Linear"
// here means 1.0 = SDR reference white; PQ and HLG sources are scaled so their 203 nit diffuse white
// lands on 1.0 (ITU-R BT.2408), anything brighter stays above 1.0 in the float canvas.
//
// The functions below are the reference the GLSL in shaders/layer.frag and shaders/output.frag is
// tested against (tst_color); change them together.

#include "media/gpu_frame.h"

#include <QString>

#include <array>
#include <optional>

namespace sf::render {

// Numbering is shared with the shaders (params.z / flags): keep in step with layer.frag.
enum class Transfer {
  Srgb = 0,   // IEC 61966-2-1 piecewise; also what untagged / BT.709 SDR video is treated as (see below)
  Bt709 = 1,  // BT.709 OETF (scene-referred)
  Bt1886 = 2, // pure 2.4 power: the BT.709 *display* EOTF
  Gamma22 = 3,
  Pq = 4,     // SMPTE ST 2084, 10000 nit absolute; 203 nit -> 1.0
  Hlg = 5,    // ARIB STD-B67 / BT.2100 with the 1000 nit system gamma
  Linear = 6,
  Gamma28 = 7,
};

enum class Primaries { Rec709, Rec2020, DisplayP3 };

// encoded value -> linear light, and back. Per channel; for HLG that is only exact for neutral
// colours (its OOTF works on luminance), use decodeRgb / encodeRgb for colour.
double toLinear(Transfer t, double encoded);
double fromLinear(Transfer t, double linear);
using Rgbd = std::array<double, 3>;
Rgbd decodeRgb(Transfer t, const Rgbd& encoded);
Rgbd encodeRgb(Transfer t, const Rgbd& linear);

// 3x3 row-major linear-light primaries conversion
using Mat3 = std::array<double, 9>;
Mat3 primariesMatrix(Primaries from, Primaries to);
Rgbd mulMat3(const Mat3& m, const Rgbd& v);
inline constexpr Mat3 kIdentity3 = {1, 0, 0, 0, 1, 0, 0, 0, 1};
Mat3 multiply(const Mat3& a, const Mat3& b); // a * b
Mat3 inverse(const Mat3& m);

// Soft highlight roll-off the compositor applies to HDR (PQ / HLG) layers when the output is SDR:
// identity up to `kToneMapKnee`, then asymptotic to 1 on the largest channel (hue preserving).
inline constexpr double kToneMapKnee = 0.75;
Rgbd toneMapHighlights(const Rgbd& linear);

// What the SDR "reference" of the Electron app means in linear light: sRGB-encoded values, which is
// also how untagged and BT.709-tagged video is shown by browsers. Streams tagged BT.709 are
// therefore decoded with the sRGB EOTF by default so an opaque layer reproduces its source bit for
// bit; ColorSettings::sdrTransfer switches every SDR source (and Rec.709 delivery) to BT.1886.
Transfer transferFor(ColorTrc trc, Transfer sdr);
// BT.2020 / P3 tagged streams carry their own gamut; everything else is taken as Rec.709.
Primaries primariesFor(ColorPrim prim);
inline bool isHdr(ColorTrc trc) { return trc == ColorTrc::Pq || trc == ColorTrc::Hlg; }

// Delivery (what the readback / the encoder gets). 8-bit sRGB and Rec.709 are the supported
// readback today; 10-bit Rec.709 and Rec.2100 PQ / HLG are in the API for the export milestone.
struct Delivery {
  enum class Encoding { Srgb, Rec709, Rec2100Pq, Rec2100Hlg };
  Encoding encoding = Encoding::Srgb;
  int bits = 8; // 8 or 10
  // A display-referred OpenColorIO colour space (e.g. "Rec.1886 Rec.709 - Display") instead of the
  // built-in encodings; `encoding` then only says how the baked result is quantised (it is ignored).
  QString ocioSpace;
  bool operator==(const Delivery&) const = default;
};
// "srgb" | "rec709" | "rec2100pq" | "rec2100hlg" (case-insensitive) -> the built-in; anything else
// is taken as an OCIO colour space name. Empty -> sRGB.
Delivery deliveryFromSpace(const QString& outputSpace, int bits = 8);
bool isHdrDelivery(const Delivery& d);

} // namespace sf::render
