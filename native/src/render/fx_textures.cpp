#include "render/fx_textures.h"

#define _USE_MATH_DEFINES
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>

namespace sf::render {

namespace {

uint32_t hashCoord(int x, int y, uint32_t seed) {
  uint32_t h = seed ^ (static_cast<uint32_t>(x) * 374761393u) ^ (static_cast<uint32_t>(y) * 668265263u);
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

double valueAt(int x, int y, uint32_t seed) {
  return static_cast<double>(hashCoord(x, y, seed) & 0xffff) / 65535.0;
}

double smoothstep(double t) {
  return t * t * (3.0 - 2.0 * t);
}

double valueNoise2D(double u, double v, double freq, uint32_t seed) {
  const double x = u * freq;
  const double y = v * freq;
  const int ix = static_cast<int>(std::floor(x));
  const int iy = static_cast<int>(std::floor(y));
  const double fx = x - ix;
  const double fy = y - iy;
  const double sx = smoothstep(fx);
  const double sy = smoothstep(fy);

  const double v00 = valueAt(ix, iy, seed);
  const double v10 = valueAt(ix + 1, iy, seed);
  const double v01 = valueAt(ix, iy + 1, seed);
  const double v11 = valueAt(ix + 1, iy + 1, seed);

  const double v0 = v00 * (1.0 - sx) + v10 * sx;
  const double v1 = v01 * (1.0 - sx) + v11 * sx;
  return v0 * (1.0 - sy) + v1 * sy;
}

constexpr double kPi = 3.14159265358979323846;

} // namespace

const QStringList& proceduralTextureNames() {
  static const QStringList kNames{
    QStringLiteral("linear_lr"),
    QStringLiteral("linear_tb"),
    QStringLiteral("radial"),
    QStringLiteral("diamond"),
    QStringLiteral("spiral"),
    QStringLiteral("noise"),
    QStringLiteral("clouds"),
    QStringLiteral("bars"),
    QStringLiteral("checker"),
  };
  return kNames;
}

QImage createProceduralTexture(const QString& name) {
  if (!proceduralTextureNames().contains(name)) return {};

  constexpr int kSize = 512;
  QImage img(kSize, kSize, QImage::Format_RGBA8888);

  for (int y = 0; y < kSize; ++y) {
    const double v = static_cast<double>(y) / static_cast<double>(kSize - 1);
    uchar* scan = img.scanLine(y);
    for (int x = 0; x < kSize; ++x) {
      const double u = static_cast<double>(x) / static_cast<double>(kSize - 1);
      double val = 0;

      if (name == QLatin1String("linear_lr")) {
        val = u;
      } else if (name == QLatin1String("linear_tb")) {
        val = v;
      } else if (name == QLatin1String("radial")) {
        const double dx = u - 0.5;
        const double dy = v - 0.5;
        const double r = std::sqrt(dx * dx + dy * dy);
        val = std::clamp(r / 0.5, 0.0, 1.0);
      } else if (name == QLatin1String("diamond")) {
        const double dx = std::abs(u - 0.5);
        const double dy = std::abs(v - 0.5);
        val = std::clamp((dx + dy) / 0.5, 0.0, 1.0);
      } else if (name == QLatin1String("spiral")) {
        const double dx = u - 0.5;
        const double dy = v - 0.5;
        const double angle = (std::atan2(dy, dx) + kPi) / (2.0 * kPi);
        const double r = std::sqrt(dx * dx + dy * dy) / 0.5;
        val = std::fmod(r * 3.0 + angle, 1.0);
        if (val < 0.0) val += 1.0;
      } else if (name == QLatin1String("noise")) {
        val = valueNoise2D(u, v, 16.0, 1337);
      } else if (name == QLatin1String("clouds")) {
        const double c1 = valueNoise2D(u, v, 8.0, 1337);
        const double c2 = valueNoise2D(u, v, 16.0, 1337 + 101);
        const double c3 = valueNoise2D(u, v, 32.0, 1337 + 202);
        val = (c1 * 4.0 + c2 * 2.0 + c3 * 1.0) / 7.0;
      } else if (name == QLatin1String("bars")) {
        const int barIdx = static_cast<int>(std::floor(u * 16.0));
        val = (barIdx % 2 == 0) ? 0.0 : 1.0;
      } else if (name == QLatin1String("checker")) {
        const int cx = static_cast<int>(std::floor(u * 16.0));
        const int cy = static_cast<int>(std::floor(v * 16.0));
        val = ((cx + cy) % 2 == 0) ? 0.0 : 1.0;
      }

      const int b = std::clamp(static_cast<int>(std::round(val * 255.0)), 0, 255);
      scan[x * 4 + 0] = static_cast<uchar>(b);
      scan[x * 4 + 1] = static_cast<uchar>(b);
      scan[x * 4 + 2] = static_cast<uchar>(b);
      scan[x * 4 + 3] = 255;
    }
  }

  return img;
}

const QImage& proceduralTexture(const QString& name) {
  static const auto& cache = *new std::map<QString, QImage>{
    {QStringLiteral("linear_lr"), createProceduralTexture(QStringLiteral("linear_lr"))},
    {QStringLiteral("linear_tb"), createProceduralTexture(QStringLiteral("linear_tb"))},
    {QStringLiteral("radial"), createProceduralTexture(QStringLiteral("radial"))},
    {QStringLiteral("diamond"), createProceduralTexture(QStringLiteral("diamond"))},
    {QStringLiteral("spiral"), createProceduralTexture(QStringLiteral("spiral"))},
    {QStringLiteral("noise"), createProceduralTexture(QStringLiteral("noise"))},
    {QStringLiteral("clouds"), createProceduralTexture(QStringLiteral("clouds"))},
    {QStringLiteral("bars"), createProceduralTexture(QStringLiteral("bars"))},
    {QStringLiteral("checker"), createProceduralTexture(QStringLiteral("checker"))},
  };
  static const QImage kEmpty;
  auto it = cache.find(name);
  return it != cache.end() ? it->second : kEmpty;
}

} // namespace sf::render
