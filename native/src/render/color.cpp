#include "render/color.h"

#include <algorithm>
#include <cmath>

namespace sf::render {

namespace {

constexpr double kPqM1 = 2610.0 / 16384.0;
constexpr double kPqM2 = 2523.0 / 4096.0 * 128.0;
constexpr double kPqC1 = 3424.0 / 4096.0;
constexpr double kPqC2 = 2413.0 / 4096.0 * 32.0;
constexpr double kPqC3 = 2392.0 / 4096.0 * 32.0;
constexpr double kPqPeak = 10000.0;
constexpr double kDiffuseWhite = 203.0; // nits that map to 1.0 (BT.2408 reference white)

constexpr double kHlgA = 0.17883277;
constexpr double kHlgB = 0.28466892;
constexpr double kHlgC = 0.55991073;
constexpr double kHlgGamma = 1.2;     // system gamma for a 1000 nit display
constexpr double kHlgPeak = 1000.0;

double hlgInverseOetf(double v) { return v <= 0.5 ? v * v / 3.0 : (std::exp((v - kHlgC) / kHlgA) + kHlgB) / 12.0; }
double hlgOetf(double e) { return e <= 1.0 / 12.0 ? std::sqrt(3.0 * e) : kHlgA * std::log(12.0 * e - kHlgB) + kHlgC; }
double lumaBt2020(const Rgbd& c) { return 0.2627 * c[0] + 0.6780 * c[1] + 0.0593 * c[2]; }

double pqDecode(double v) {
  const double p = std::pow(std::clamp(v, 0.0, 1.0), 1.0 / kPqM2);
  const double num = std::max(p - kPqC1, 0.0);
  const double den = kPqC2 - kPqC3 * p;
  return std::pow(num / den, 1.0 / kPqM1) * kPqPeak / kDiffuseWhite;
}

double pqEncode(double l) {
  const double y = std::pow(std::clamp(l * kDiffuseWhite / kPqPeak, 0.0, 1.0), kPqM1);
  return std::pow((kPqC1 + kPqC2 * y) / (1.0 + kPqC3 * y), kPqM2);
}

struct Chroma {
  double rx, ry, gx, gy, bx, by;
};
constexpr double kWx = 0.3127, kWy = 0.3290; // D65

Chroma chromaOf(Primaries p) {
  switch (p) {
  case Primaries::Rec2020: return {0.708, 0.292, 0.170, 0.797, 0.131, 0.046};
  case Primaries::DisplayP3: return {0.680, 0.320, 0.265, 0.690, 0.150, 0.060};
  default: return {0.640, 0.330, 0.300, 0.600, 0.150, 0.060};
  }
}

Mat3 rgbToXyz(Primaries p) {
  const Chroma c = chromaOf(p);
  const auto col = [](double x, double y) { return Rgbd{x / y, 1.0, (1.0 - x - y) / y}; };
  const Rgbd r = col(c.rx, c.ry), g = col(c.gx, c.gy), b = col(c.bx, c.by), w = col(kWx, kWy);
  const Mat3 prim = {r[0], g[0], b[0], r[1], g[1], b[1], r[2], g[2], b[2]};
  const Rgbd s = mulMat3(inverse(prim), w);
  return {r[0] * s[0], g[0] * s[1], b[0] * s[2], r[1] * s[0], g[1] * s[1], b[1] * s[2], r[2] * s[0], g[2] * s[1], b[2] * s[2]};
}

} // namespace

double toLinear(Transfer t, double v) {
  switch (t) {
  case Transfer::Srgb: return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
  case Transfer::Bt709: return v < 0.081 ? v / 4.5 : std::pow((v + 0.099) / 1.099, 1.0 / 0.45);
  case Transfer::Bt1886: return std::pow(std::max(v, 0.0), 2.4);
  case Transfer::Gamma22: return std::pow(std::max(v, 0.0), 2.2);
  case Transfer::Gamma28: return std::pow(std::max(v, 0.0), 2.8);
  case Transfer::Pq: return pqDecode(v);
  case Transfer::Hlg: {
    const double e = hlgInverseOetf(std::max(v, 0.0));
    return e * std::pow(e, kHlgGamma - 1.0) * (kHlgPeak / kDiffuseWhite);
  }
  case Transfer::Linear: return v;
  }
  return v;
}

double fromLinear(Transfer t, double l) {
  switch (t) {
  case Transfer::Srgb: return l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
  case Transfer::Bt709: return l < 0.018 ? l * 4.5 : 1.099 * std::pow(l, 0.45) - 0.099;
  case Transfer::Bt1886: return std::pow(std::max(l, 0.0), 1.0 / 2.4);
  case Transfer::Gamma22: return std::pow(std::max(l, 0.0), 1.0 / 2.2);
  case Transfer::Gamma28: return std::pow(std::max(l, 0.0), 1.0 / 2.8);
  case Transfer::Pq: return pqEncode(l);
  case Transfer::Hlg: {
    const double fd = std::max(l, 0.0) * kDiffuseWhite / kHlgPeak;
    return hlgOetf(fd <= 0 ? 0.0 : fd * std::pow(fd, (1.0 - kHlgGamma) / kHlgGamma));
  }
  case Transfer::Linear: return l;
  }
  return l;
}

Rgbd decodeRgb(Transfer t, const Rgbd& v) {
  if (t != Transfer::Hlg) return {toLinear(t, v[0]), toLinear(t, v[1]), toLinear(t, v[2])};
  const Rgbd e = {hlgInverseOetf(std::max(v[0], 0.0)), hlgInverseOetf(std::max(v[1], 0.0)), hlgInverseOetf(std::max(v[2], 0.0))};
  const double ys = lumaBt2020(e);
  const double k = ys > 0 ? std::pow(ys, kHlgGamma - 1.0) * (kHlgPeak / kDiffuseWhite) : 0.0;
  return {e[0] * k, e[1] * k, e[2] * k};
}

Rgbd encodeRgb(Transfer t, const Rgbd& l) {
  if (t != Transfer::Hlg) return {fromLinear(t, l[0]), fromLinear(t, l[1]), fromLinear(t, l[2])};
  const double s = kDiffuseWhite / kHlgPeak;
  const Rgbd fd = {std::max(l[0], 0.0) * s, std::max(l[1], 0.0) * s, std::max(l[2], 0.0) * s};
  const double ys = lumaBt2020(fd);
  const double k = ys > 0 ? std::pow(ys, (1.0 - kHlgGamma) / kHlgGamma) : 0.0;
  return {hlgOetf(fd[0] * k), hlgOetf(fd[1] * k), hlgOetf(fd[2] * k)};
}

Rgbd mulMat3(const Mat3& m, const Rgbd& v) {
  return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2], m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}

Mat3 multiply(const Mat3& a, const Mat3& b) {
  Mat3 o{};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      for (int k = 0; k < 3; ++k) o[static_cast<size_t>(r * 3 + c)] += a[static_cast<size_t>(r * 3 + k)] * b[static_cast<size_t>(k * 3 + c)];
    }
  }
  return o;
}

Mat3 inverse(const Mat3& m) {
  const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
  const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  if (det == 0) return kIdentity3;
  const double s = 1.0 / det;
  return {(e * i - f * h) * s, (c * h - b * i) * s, (b * f - c * e) * s, (f * g - d * i) * s, (a * i - c * g) * s,
          (c * d - a * f) * s, (d * h - e * g) * s, (b * g - a * h) * s, (a * e - b * d) * s};
}

Mat3 primariesMatrix(Primaries from, Primaries to) {
  if (from == to) return kIdentity3;
  return multiply(inverse(rgbToXyz(to)), rgbToXyz(from));
}

Rgbd toneMapHighlights(const Rgbd& l) {
  const double m = std::max({l[0], l[1], l[2]});
  if (m <= kToneMapKnee) return l;
  const double range = 1.0 - kToneMapKnee;
  const double mapped = kToneMapKnee + range * (1.0 - std::exp(-(m - kToneMapKnee) / range));
  const double s = mapped / m;
  return {l[0] * s, l[1] * s, l[2] * s};
}

Transfer transferFor(ColorTrc trc, Transfer sdr) {
  switch (trc) {
  case ColorTrc::Srgb: return Transfer::Srgb;
  case ColorTrc::Linear: return Transfer::Linear;
  case ColorTrc::Gamma22: return Transfer::Gamma22;
  case ColorTrc::Gamma28: return Transfer::Gamma28;
  case ColorTrc::Pq: return Transfer::Pq;
  case ColorTrc::Hlg: return Transfer::Hlg;
  case ColorTrc::Bt709:
  case ColorTrc::Unspecified: break;
  }
  return sdr;
}

Primaries primariesFor(ColorPrim prim) {
  switch (prim) {
  case ColorPrim::Bt2020: return Primaries::Rec2020;
  case ColorPrim::DisplayP3: return Primaries::DisplayP3;
  default: return Primaries::Rec709;
  }
}

Delivery deliveryFromSpace(const QString& space, int bits) {
  Delivery d;
  d.bits = bits;
  const QString s = space.trimmed().toLower();
  if (s.isEmpty() || s == QLatin1String("srgb")) return d;
  if (s == QLatin1String("rec709")) d.encoding = Delivery::Encoding::Rec709;
  else if (s == QLatin1String("rec2100pq")) d.encoding = Delivery::Encoding::Rec2100Pq;
  else if (s == QLatin1String("rec2100hlg")) d.encoding = Delivery::Encoding::Rec2100Hlg;
  else d.ocioSpace = space.trimmed();
  return d;
}

bool isHdrDelivery(const Delivery& d) {
  return d.ocioSpace.isEmpty() && (d.encoding == Delivery::Encoding::Rec2100Pq || d.encoding == Delivery::Encoding::Rec2100Hlg);
}

} // namespace sf::render
