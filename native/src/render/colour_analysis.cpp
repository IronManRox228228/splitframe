#include "render/colour_analysis.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace sf::render {

namespace {

constexpr double kLogFloor = 1.0 / 1024.0; // 2^-10: what a black pixel counts as in log statistics
constexpr double kContrastFloor = 0.001;
constexpr double kCrushedMax = 2.0 / 255.0 + 1e-6;
constexpr double kClippedMin = 253.0 / 255.0 - 1e-6;
constexpr double kMidGrey = 0.18;

double luma(double r, double g, double b) { return 0.2126 * r + 0.7152 * g + 0.0722 * b; }
double q8(double v) { return std::round(std::clamp(v, 0.0, 1.0) * 255.0) / 255.0; }
double lg2(double v) { return std::log2(std::max(v, kLogFloor)); }
double round4(double v) { return std::round(v * 10000.0) / 10000.0; }

float percentile(const std::vector<float>& sorted, double p) {
  if (sorted.empty()) return 0.f;
  const double idx = std::clamp(p / 100.0 * static_cast<double>(sorted.size() - 1), 0.0, static_cast<double>(sorted.size() - 1));
  return sorted[static_cast<size_t>(std::llround(idx))];
}

float median(std::vector<float>& v) {
  if (v.empty()) return 0.f;
  const size_t mid = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
  return v[mid];
}

double wrapDeg(double d) {
  while (d > 180) d -= 360;
  while (d <= -180) d += 360;
  return d;
}

QJsonObject pctJson(const ChannelPct& p) { return {{"r", round4(p.r)}, {"g", round4(p.g)}, {"b", round4(p.b)}, {"all", round4(p.all)}}; }

} // namespace

void PixelSet::append(const PixelSet& other) {
  lin.insert(lin.end(), other.lin.begin(), other.lin.end());
  disp.insert(disp.end(), other.disp.begin(), other.disp.end());
}

PixelSet pixelsFromLinear(const std::vector<float>& linearRgb) {
  PixelSet p;
  p.lin = linearRgb;
  p.disp.resize(linearRgb.size());
  for (size_t i = 0; i < linearRgb.size(); ++i) p.disp[i] = static_cast<float>(q8(fromLinear(Transfer::Srgb, std::max(static_cast<double>(linearRgb[i]), 0.0))));
  return p;
}

PixelSet pixelsFromImages(const QImage& linear, const QImage* display, int maxSide, const std::vector<unsigned char>* coverage) {
  PixelSet out;
  if (linear.isNull() || linear.format() != QImage::Format_RGBA32FPx4_Premultiplied) return out;
  const int w = linear.width(), h = linear.height();
  const int step = std::max(1, (std::max(w, h) + maxSide - 1) / maxSide);
  const bool haveDisplay = display && display->size() == linear.size() && (display->format() == QImage::Format_RGBA8888 || display->format() == QImage::Format_RGBA64);
  const bool d8 = haveDisplay && display->format() == QImage::Format_RGBA8888;
  const size_t cap = static_cast<size_t>((w + step - 1) / step) * static_cast<size_t>((h + step - 1) / step);
  out.lin.reserve(cap * 3);
  out.disp.reserve(cap * 3);
  for (int y = 0; y < h; y += step) {
    const float* row = reinterpret_cast<const float*>(linear.constScanLine(y));
    const uchar* d8row = haveDisplay && d8 ? display->constScanLine(y) : nullptr;
    const quint16* d16row = haveDisplay && !d8 ? reinterpret_cast<const quint16*>(display->constScanLine(y)) : nullptr;
    for (int x = 0; x < w; x += step) {
      if (coverage && !coverage->empty() && !(*coverage)[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)]) continue;
      const float* px = row + x * 4;
      const float a = px[3];
      if (a < 0.99f) continue;
      const float r = px[0] / a, g = px[1] / a, b = px[2] / a;
      out.lin.insert(out.lin.end(), {r, g, b});
      if (d8row) {
        out.disp.insert(out.disp.end(), {d8row[x * 4] / 255.f, d8row[x * 4 + 1] / 255.f, d8row[x * 4 + 2] / 255.f});
      } else if (d16row) {
        out.disp.insert(out.disp.end(), {d16row[x * 4] / 65535.f, d16row[x * 4 + 1] / 65535.f, d16row[x * 4 + 2] / 65535.f});
      } else {
        for (const float v : {r, g, b}) out.disp.push_back(static_cast<float>(q8(fromLinear(Transfer::Srgb, std::max(static_cast<double>(v), 0.0)))));
      }
    }
  }
  return out;
}

PixelSet subsample(const PixelSet& in, size_t maxPixels) {
  const size_t n = in.size();
  if (n <= maxPixels || maxPixels == 0) return in;
  const size_t stride = (n + maxPixels - 1) / maxPixels;
  PixelSet out;
  out.lin.reserve((n / stride + 1) * 3);
  out.disp.reserve((n / stride + 1) * 3);
  for (size_t i = 0; i < n; i += stride) {
    out.lin.insert(out.lin.end(), in.lin.begin() + static_cast<std::ptrdiff_t>(i * 3), in.lin.begin() + static_cast<std::ptrdiff_t>(i * 3 + 3));
    out.disp.insert(out.disp.end(), in.disp.begin() + static_cast<std::ptrdiff_t>(i * 3), in.disp.begin() + static_cast<std::ptrdiff_t>(i * 3 + 3));
  }
  return out;
}

PixelSet gradePixels(const PixelSet& in, const PrimaryGrade& grade) {
  const GradeCoeffs c = gradeCoeffs(grade);
  std::vector<float> lin(in.lin.size());
  for (size_t i = 0; i + 2 < in.lin.size(); i += 3) {
    const Rgbd v = applyGrade(c, {in.lin[i], in.lin[i + 1], in.lin[i + 2]});
    lin[i] = static_cast<float>(v[0]);
    lin[i + 1] = static_cast<float>(v[1]);
    lin[i + 2] = static_cast<float>(v[2]);
  }
  return pixelsFromLinear(lin);
}

// ---------------------------------------------------------------- analysis

ColourMetrics analyze(const PixelSet& px) {
  ColourMetrics m;
  const size_t n = px.size();
  if (n == 0) return m;
  m.valid = true;
  m.pixels = static_cast<qint64>(n);

  std::vector<float> lumas, metered, chromas, castT, castTint;
  lumas.reserve(n);
  metered.reserve(n);
  chromas.reserve(n);
  double lumaSum = 0, dispSum = 0, dispSq = 0;
  qint64 crushed[4] = {0, 0, 0, 0}, clipped[4] = {0, 0, 0, 0}, gamut = 0, skin = 0;
  double logSum[3] = {0, 0, 0}, logSq[3] = {0, 0, 0}, lumaLogSum = 0, lumaLogSq = 0;
  double skinX = 0, skinY = 0;
  qint64 neutral = 0;
  std::array<qint64, kHistogramBins> bins{};

  for (size_t i = 0; i < n; ++i) {
    const double lr = px.lin[i * 3], lgn = px.lin[i * 3 + 1], lb = px.lin[i * 3 + 2];
    const double dr = px.disp[i * 3], dg = px.disp[i * 3 + 1], db = px.disp[i * 3 + 2];
    const double ll = luma(std::max(lr, 0.0), std::max(lgn, 0.0), std::max(lb, 0.0));
    const double ld = luma(dr, dg, db);
    lumas.push_back(static_cast<float>(ll));
    if (ld > kCrushedMax) metered.push_back(static_cast<float>(ll)); // pure black carries no exposure information
    lumaSum += ll;
    dispSum += ld;
    dispSq += ld * ld;
    bins[static_cast<size_t>(std::clamp(static_cast<int>(ld * kHistogramBins), 0, kHistogramBins - 1))]++;

    const double dc[3] = {dr, dg, db};
    for (int c = 0; c < 3; ++c) {
      if (dc[c] <= kCrushedMax) crushed[c]++;
      if (dc[c] >= kClippedMin) clipped[c]++;
    }
    if (ld <= kCrushedMax) crushed[3]++;
    if (ld >= kClippedMin) clipped[3]++;

    const double dmax = std::max({dr, dg, db}), dmin = std::min({dr, dg, db});
    chromas.push_back(static_cast<float>(dmax - dmin));
    if (dmax >= 0.1 && (dmax - dmin) / dmax >= 0.95) gamut++;

    const double lin[3] = {std::max(lr, 0.0), std::max(lgn, 0.0), std::max(lb, 0.0)};
    for (int c = 0; c < 3; ++c) {
      const double l = lg2(lin[c]);
      logSum[c] += l;
      logSq[c] += l * l;
    }
    const double ml = lg2(ll);
    lumaLogSum += ml;
    lumaLogSq += ml * ml;

    // near-neutral pixel: mid display luma, low linear saturation, all channels above the noise floor
    const double lmax = std::max({lin[0], lin[1], lin[2]}), lmin = std::min({lin[0], lin[1], lin[2]});
    if (ld >= 0.15 && ld <= 0.85 && lmin > 1e-4 && (lmax - lmin) / lmax <= 0.55) {
      ++neutral;
      castT.push_back(static_cast<float>(std::log2(lin[0] / lin[2])));
      castTint.push_back(static_cast<float>(-2.0 * std::log2(lin[1] / std::sqrt(lin[0] * lin[2]))));
    }

    // skin
    const double cb = (db - ld) / 1.8556, cr = (dr - ld) / 1.5748;
    const double mag = std::hypot(cb, cr);
    if (mag >= 0.04 && mag <= 0.20 && ld >= 0.20 && ld <= 0.92 && dr > dg && dg > db) {
      double hue = std::atan2(cr, cb) * 180.0 / std::numbers::pi;
      if (hue < 0) hue += 360.0;
      if (hue >= 108.0 && hue <= 146.0) {
        ++skin;
        skinX += std::cos(hue * std::numbers::pi / 180.0);
        skinY += std::sin(hue * std::numbers::pi / 180.0);
      }
    }
  }

  const double dn = static_cast<double>(n);
  const double pct = 100.0 / dn;
  std::sort(lumas.begin(), lumas.end());
  m.lumaMean = lumaSum / dn;
  m.p1 = percentile(lumas, 1);
  m.p5 = percentile(lumas, 5);
  m.p50 = m.lumaMedian = percentile(lumas, 50);
  m.p95 = percentile(lumas, 95);
  m.p99 = percentile(lumas, 99);
  for (int i = 0; i < kHistogramBins; ++i) m.histogram[static_cast<size_t>(i)] = static_cast<double>(bins[static_cast<size_t>(i)]) / dn;
  // Meter like a camera: ignore crushed pixels, which no exposure change can move. Fall back to every
  // pixel when almost the whole picture is crushed.
  double meteredMedian = m.lumaMedian;
  if (metered.size() * 100 >= n) {
    std::sort(metered.begin(), metered.end());
    meteredMedian = percentile(metered, 50);
  }
  m.exposureStops = std::log2(std::max(meteredMedian, 1e-6) / kMidGrey);
  m.contrastStops = std::log2(std::max(m.p99, kContrastFloor) / std::max(m.p1, kContrastFloor));
  const double dMean = dispSum / dn;
  m.rmsContrast = std::sqrt(std::max(dispSq / dn - dMean * dMean, 0.0));
  m.crushedPct = {crushed[0] * pct, crushed[1] * pct, crushed[2] * pct, crushed[3] * pct};
  m.clippedPct = {clipped[0] * pct, clipped[1] * pct, clipped[2] * pct, clipped[3] * pct};

  m.neutralPct = static_cast<double>(neutral) * pct;
  if (neutral >= 8 && m.neutralPct >= 0.5) {
    m.castTemperature = median(castT);
    m.castTint = median(castTint);
  }

  double chromaSum = 0;
  for (float c : chromas) chromaSum += c;
  std::sort(chromas.begin(), chromas.end());
  m.chromaMean = chromaSum / dn;
  m.chromaP95 = percentile(chromas, 95);
  m.gamutLimitPct = static_cast<double>(gamut) * pct;

  m.skinPct = static_cast<double>(skin) * pct;
  if (skin > 0) {
    m.skinHueDeg = std::atan2(skinY, skinX) * 180.0 / std::numbers::pi;
    if (m.skinHueDeg < 0) m.skinHueDeg += 360.0;
    m.skinOffsetDeg = wrapDeg(m.skinHueDeg - kSkinLineDeg);
  }

  for (int c = 0; c < 3; ++c) {
    m.logMean[static_cast<size_t>(c)] = logSum[c] / dn;
    m.logStd[static_cast<size_t>(c)] = std::sqrt(std::max(logSq[c] / dn - (logSum[c] / dn) * (logSum[c] / dn), 0.0));
  }
  m.lumaLogMean = lumaLogSum / dn;
  m.lumaLogStd = std::sqrt(std::max(lumaLogSq / dn - m.lumaLogMean * m.lumaLogMean, 0.0));

  m.exposureMin = m.exposureMax = m.exposureStops;
  m.crushedMin = m.crushedMax = m.crushedPct.all;
  m.clippedMin = m.clippedMax = m.clippedPct.all;
  return m;
}

ColourMetrics analyzeFrames(const std::vector<PixelSet>& frames) {
  if (frames.empty()) return {};
  if (frames.size() == 1) return analyze(frames.front());
  PixelSet pooled;
  for (const PixelSet& f : frames) pooled.append(f);
  ColourMetrics m = analyze(pooled);
  m.frames = static_cast<int>(frames.size());
  bool first = true;
  for (const PixelSet& f : frames) {
    const ColourMetrics fm = analyze(f);
    if (!fm.valid) continue;
    if (first) {
      m.exposureMin = m.exposureMax = fm.exposureStops;
      m.crushedMin = m.crushedMax = fm.crushedPct.all;
      m.clippedMin = m.clippedMax = fm.clippedPct.all;
      first = false;
    } else {
      m.exposureMin = std::min(m.exposureMin, fm.exposureStops);
      m.exposureMax = std::max(m.exposureMax, fm.exposureStops);
      m.crushedMin = std::min(m.crushedMin, fm.crushedPct.all);
      m.crushedMax = std::max(m.crushedMax, fm.crushedPct.all);
      m.clippedMin = std::min(m.clippedMin, fm.clippedPct.all);
      m.clippedMax = std::max(m.clippedMax, fm.clippedPct.all);
    }
  }
  return m;
}

QJsonObject ColourMetrics::toJson() const {
  QJsonArray hist;
  for (double v : histogram) hist.append(round4(v));
  const auto label = [](double v, double eps, const char* neg, const char* pos) { return std::fabs(v) < eps ? QStringLiteral("neutral") : QString::fromLatin1(v > 0 ? pos : neg); };
  QJsonObject o;
  o.insert("valid", valid);
  o.insert("frames", frames);
  o.insert("pixels", static_cast<double>(pixels));
  o.insert("luma", QJsonObject{{"mean", round4(lumaMean)}, {"median", round4(lumaMedian)}, {"p1", round4(p1)}, {"p5", round4(p5)}, {"p50", round4(p50)}, {"p95", round4(p95)},
                                {"p99", round4(p99)}, {"stopsVs18Grey", round4(exposureStops)}});
  o.insert("histogram", QJsonObject{{"bins", kHistogramBins}, {"space", "display luma 0..1, bin 0 = black"}, {"fraction", hist}});
  o.insert("contrast", QJsonObject{{"stops", round4(contrastStops)}, {"rms", round4(rmsContrast)}});
  o.insert("crushedPct", pctJson(crushedPct));
  o.insert("clippedPct", pctJson(clippedPct));
  o.insert("cast", QJsonObject{{"temperature", round4(castTemperature)},
                               {"tint", round4(castTint)},
                               {"neutralPixelsPct", round4(neutralPct)},
                               {"warmCool", label(castTemperature, 0.02, "cool", "warm")},
                               {"greenMagenta", label(castTint, 0.02, "green", "magenta")},
                               {"proposedCorrection", QJsonObject{{"temperature", round4(-castTemperature)}, {"tint", round4(-castTint)}}}});
  o.insert("saturation", QJsonObject{{"chromaMean", round4(chromaMean)}, {"chromaP95", round4(chromaP95)}, {"nearGamutLimitPct", round4(gamutLimitPct)}});
  o.insert("skin", QJsonObject{{"pct", round4(skinPct)}, {"hueDeg", round4(skinHueDeg)}, {"offsetFromSkinLineDeg", round4(skinOffsetDeg)}, {"skinLineDeg", kSkinLineDeg}});
  o.insert("stats", QJsonObject{{"logMean", QJsonArray{round4(logMean[0]), round4(logMean[1]), round4(logMean[2])}},
                                {"logStd", QJsonArray{round4(logStd[0]), round4(logStd[1]), round4(logStd[2])}},
                                {"lumaLogMean", round4(lumaLogMean)},
                                {"lumaLogStd", round4(lumaLogStd)}});
  o.insert("range", QJsonObject{{"exposureStopsMin", round4(exposureMin)}, {"exposureStopsMax", round4(exposureMax)}, {"crushedAllMin", round4(crushedMin)}, {"crushedAllMax", round4(crushedMax)},
                                {"clippedAllMin", round4(clippedMin)}, {"clippedAllMax", round4(clippedMax)}});
  return o;
}

ColourMetrics ColourMetrics::fromJson(const QJsonObject& o) {
  ColourMetrics m;
  m.valid = o.value("valid").toBool(true);
  m.frames = o.value("frames").toInt(1);
  m.pixels = static_cast<qint64>(o.value("pixels").toDouble());
  const QJsonObject l = o.value("luma").toObject();
  m.lumaMean = l.value("mean").toDouble();
  m.lumaMedian = m.p50 = l.value("median").toDouble();
  m.p1 = l.value("p1").toDouble();
  m.p5 = l.value("p5").toDouble();
  m.p95 = l.value("p95").toDouble();
  m.p99 = l.value("p99").toDouble();
  m.exposureStops = l.value("stopsVs18Grey").toDouble();
  const QJsonObject c = o.value("contrast").toObject();
  m.contrastStops = c.value("stops").toDouble();
  m.rmsContrast = c.value("rms").toDouble();
  const auto pct = [](const QJsonObject& p) { return ChannelPct{p.value("r").toDouble(), p.value("g").toDouble(), p.value("b").toDouble(), p.value("all").toDouble()}; };
  m.crushedPct = pct(o.value("crushedPct").toObject());
  m.clippedPct = pct(o.value("clippedPct").toObject());
  const QJsonObject cast = o.value("cast").toObject();
  m.castTemperature = cast.value("temperature").toDouble();
  m.castTint = cast.value("tint").toDouble();
  m.neutralPct = cast.value("neutralPixelsPct").toDouble();
  const QJsonObject s = o.value("saturation").toObject();
  m.chromaMean = s.value("chromaMean").toDouble();
  m.chromaP95 = s.value("chromaP95").toDouble();
  m.gamutLimitPct = s.value("nearGamutLimitPct").toDouble();
  const QJsonObject sk = o.value("skin").toObject();
  m.skinPct = sk.value("pct").toDouble();
  m.skinHueDeg = sk.value("hueDeg").toDouble();
  m.skinOffsetDeg = sk.value("offsetFromSkinLineDeg").toDouble();
  const QJsonObject st = o.value("stats").toObject();
  const QJsonArray lm = st.value("logMean").toArray(), ls = st.value("logStd").toArray();
  for (int i = 0; i < 3 && i < lm.size() && i < ls.size(); ++i) {
    m.logMean[static_cast<size_t>(i)] = lm.at(i).toDouble();
    m.logStd[static_cast<size_t>(i)] = ls.at(i).toDouble();
  }
  m.lumaLogMean = st.value("lumaLogMean").toDouble();
  m.lumaLogStd = st.value("lumaLogStd").toDouble();
  const QJsonArray hist = o.value("histogram").toObject().value("fraction").toArray();
  for (int i = 0; i < kHistogramBins && i < hist.size(); ++i) m.histogram[static_cast<size_t>(i)] = hist.at(i).toDouble();
  m.exposureMin = m.exposureMax = m.exposureStops;
  m.crushedMin = m.crushedMax = m.crushedPct.all;
  m.clippedMin = m.clippedMax = m.clippedPct.all;
  return m;
}

// ---------------------------------------------------------------- proposals

namespace {

constexpr size_t kProposalPixels = 40000; // CPU twin evaluations per iteration

double clampD(double v, double lo, double hi) { return std::clamp(v, lo, hi); }
double round3(double v) { return std::round(v * 1000.0) / 1000.0; }

void roundGrade(PrimaryGrade& g) {
  g.exposure = round3(g.exposure);
  g.temperature = round3(g.temperature);
  g.tint = round3(g.tint);
  g.contrast = round3(g.contrast);
  g.saturation = round3(g.saturation);
  g.gain = {round3(g.gain.r), round3(g.gain.g), round3(g.gain.b)};
}

// per-channel log mean relative to the mean of the three: the "opponent" description of colour balance
std::array<double, 3> opponent(const ColourMetrics& m) {
  const double avg = (m.logMean[0] + m.logMean[1] + m.logMean[2]) / 3.0;
  return {m.logMean[0] - avg, m.logMean[1] - avg, m.logMean[2] - avg};
}

} // namespace

Proposal proposeAutoBalance(const PixelSet& source, const AutoBalanceOptions& opt) {
  Proposal p;
  const PixelSet src = subsample(source, kProposalPixels);
  p.before = analyze(src);
  if (!p.before.valid) {
    p.predicted = p.before;
    p.notes << QStringLiteral("No pixels to measure.");
    return p;
  }
  const double target = std::max(opt.exposureTarget, 1e-3);
  PrimaryGrade g;
  const bool haveCast = opt.whiteBalance && p.before.neutralPct >= 0.5;
  if (opt.whiteBalance && !haveCast) p.notes << QStringLiteral("Too few near-neutral pixels to estimate a colour cast: white balance left alone.");

  ColourMetrics m = p.before;
  for (int it = 0; it < 5; ++it) {
    ++p.iterations;
    bool changed = false;
    if (haveCast) {
      const double dt = -m.castTemperature, dtint = -m.castTint;
      if (std::fabs(dt) > 0.002 || std::fabs(dtint) > 0.002) {
        g.temperature = clampD(g.temperature + dt, -1, 1);
        g.tint = clampD(g.tint + dtint, -1, 1);
        changed = true;
      }
    }
    m = analyze(gradePixels(src, g));
    const double off = std::log2(target / std::max(m.lumaMedian, 1e-6));
    if (std::fabs(off) > opt.toleranceStops || g.exposure != 0) {
      const double e = clampD(g.exposure + off, -opt.maxExposureStops, opt.maxExposureStops);
      // inside the tolerance band only an exposure we already set is kept converging
      if (std::fabs(e - g.exposure) > 0.002) {
        g.exposure = e;
        changed = true;
        m = analyze(gradePixels(src, g));
      }
    }
    if (!changed) break;
  }

  // never trade the picture for a number: back exposure, then white balance, off while crushed / clipped grow
  const auto bad = [&](const ColourMetrics& x) {
    return x.crushedPct.all - p.before.crushedPct.all > opt.maxIncreasePct || x.clippedPct.all - p.before.clippedPct.all > opt.maxIncreasePct;
  };
  m = analyze(gradePixels(src, g));
  if (bad(m) && g.exposure != 0) {
    const double wanted = g.exposure;
    for (int k = 0; k < 8 && bad(m) && std::fabs(g.exposure) > 0.01; ++k) {
      g.exposure *= 0.5;
      m = analyze(gradePixels(src, g));
    }
    if (std::fabs(g.exposure) <= 0.01) g.exposure = 0;
    p.notes << QStringLiteral("Exposure limited from %1 to %2 stops to keep crushed / clipped pixels from growing by more than %3 points.").arg(wanted, 0, 'f', 2).arg(g.exposure, 0, 'f', 2).arg(opt.maxIncreasePct);
    m = analyze(gradePixels(src, g));
  }
  if (bad(m) && (g.temperature != 0 || g.tint != 0)) {
    for (int k = 0; k < 3 && bad(m); ++k) {
      g.temperature *= 0.5;
      g.tint *= 0.5;
      m = analyze(gradePixels(src, g));
    }
    p.notes << QStringLiteral("White balance halved to keep crushed / clipped pixels in check.");
  }
  if (bad(m)) p.notes << QStringLiteral("The picture already has crushed or clipped regions that no primary correction here removes.");
  roundGrade(g);
  p.primaries = g;
  p.predicted = analyze(gradePixels(src, g));
  return p;
}

Proposal proposeMatch(const PixelSet& target, const ColourMetrics& ref) {
  Proposal p;
  const PixelSet src = subsample(target, kProposalPixels);
  p.before = analyze(src);
  p.predicted = p.before;
  if (!p.before.valid || !ref.valid) {
    p.notes << QStringLiteral("No pixels to measure.");
    return p;
  }
  PrimaryGrade g;
  const std::array<double, 3> refOpp = opponent(ref);
  const double pivotLog = std::log2(g.pivot);

  const auto residual = [&](const ColourMetrics& m) {
    const std::array<double, 3> o = opponent(m);
    return std::array<double, 5>{ref.lumaLogMean - m.lumaLogMean, ref.lumaLogStd - m.lumaLogStd, refOpp[0] - o[0], refOpp[1] - o[1], refOpp[2] - o[2]};
  };
  const auto rms = [](const std::array<double, 5>& r) {
    double s = 0;
    for (double v : r) s += v * v;
    return std::sqrt(s / 5.0);
  };

  ColourMetrics m = p.before;
  for (int it = 0; it < 12; ++it) {
    ++p.iterations;
    const auto r = residual(m);
    if (rms(r) < 0.01 && std::fabs(ref.chromaMean - m.chromaMean) < 0.005) break;
    const double oldContrast = g.contrast;
    // contrast: log spread scales linearly with the exponent
    if (m.lumaLogStd > 1e-3 && ref.lumaLogStd > 1e-3) g.contrast = clampD(g.contrast * (ref.lumaLogStd / m.lumaLogStd), 0.2, 2.0);
    const double f = g.contrast / oldContrast;
    // exposure acts before the power curve, so one stop in is `contrast` stops out; first account for the mean the new contrast moves
    const double predictedMean = pivotLog + f * (m.lumaLogMean - pivotLog);
    g.exposure = clampD(g.exposure + (ref.lumaLogMean - predictedMean) / g.contrast, -4, 4);
    // balance: opponent channel means, again scaled by the exponent
    g.temperature = clampD(g.temperature + (r[2] - r[4]) / g.contrast, -1, 1);
    g.tint = clampD(g.tint - 2.0 * (r[3] - (r[2] + r[4]) / 2.0) / g.contrast, -1, 1);
    if (m.chromaMean > 1e-4) g.saturation = clampD(g.saturation * std::sqrt(std::max(ref.chromaMean, 1e-4) / m.chromaMean) * 1.0, 0.0, 2.0);
    m = analyze(gradePixels(src, g));
  }
  // what the clamps (or the saturation interplay) left over in colour balance goes into per-channel gain, last
  for (int it = 0; it < 4; ++it) {
    const auto r = residual(m);
    if (std::max({std::fabs(r[2]), std::fabs(r[3]), std::fabs(r[4])}) < 0.03) break;
    ++p.iterations;
    Rgb3* gain = &g.gain;
    gain->r = clampD(gain->r * std::exp2(r[2] / g.contrast), 0.0, 4.0);
    gain->g = clampD(gain->g * std::exp2(r[3] / g.contrast), 0.0, 4.0);
    gain->b = clampD(gain->b * std::exp2(r[4] / g.contrast), 0.0, 4.0);
    if (it == 0) p.notes << QStringLiteral("Residual colour balance moved into per-channel gain.");
    m = analyze(gradePixels(src, g));
  }
  roundGrade(g);
  m = analyze(gradePixels(src, g));
  const auto r = residual(m);
  QStringList extreme;
  if (std::fabs(g.exposure) > 2.0) extreme << QStringLiteral("exposure %1").arg(g.exposure);
  if (g.contrast < 0.6 || g.contrast > 1.6) extreme << QStringLiteral("contrast %1").arg(g.contrast);
  if (std::fabs(g.temperature) >= 1.0) extreme << QStringLiteral("temperature %1").arg(g.temperature);
  if (std::fabs(g.tint) >= 1.0) extreme << QStringLiteral("tint %1").arg(g.tint);
  for (const auto& [name, v] : {std::pair{"red", g.gain.r}, std::pair{"green", g.gain.g}, std::pair{"blue", g.gain.b}})
    if (v < 0.5 || v > 2.0) extreme << QStringLiteral("%1 gain %2").arg(QLatin1String(name)).arg(v);
  if (!extreme.isEmpty()) {
    p.plausible = false;
    p.notes << QStringLiteral("Not a natural match: reaching the reference needs %1. The shots are probably different scenes; pick a closer reference or match exposure and white balance only.")
                   .arg(extreme.join(QStringLiteral(", ")));
  }
  p.primaries = g;
  p.predicted = m;
  p.error = rms(r);
  p.residual = QJsonObject{{"rmsStops", round4(p.error)},
                           {"lumaLogMeanStops", round4(r[0])},
                           {"lumaLogStdStops", round4(r[1])},
                           {"channelBalanceStops", QJsonArray{round4(r[2]), round4(r[3]), round4(r[4])}},
                           {"chromaMean", round4(ref.chromaMean - m.chromaMean)},
                           {"exposureStopsVsReference", round4(m.exposureStops - ref.exposureStops)}};
  return p;
}

// ---------------------------------------------------------------- check

QJsonObject Violation::toJson() const { return {{"code", code}, {"message", message}, {"value", round4(value)}, {"limit", round4(limit)}}; }

std::vector<Violation> checkColour(const ColourMetrics& before, const ColourMetrics& after, const CheckThresholds& t) {
  std::vector<Violation> v;
  const double dCrush = after.crushedPct.all - before.crushedPct.all;
  if (dCrush > t.blacksCrushedIncrease)
    v.push_back({QStringLiteral("blacksCrushedIncrease"), QStringLiteral("Crushed blacks (display luma <= 2/255) grew from %1% to %2%: shadow detail was lost.").arg(before.crushedPct.all, 0, 'f', 1).arg(after.crushedPct.all, 0, 'f', 1), dCrush,
                 t.blacksCrushedIncrease});
  const double dClip = after.clippedPct.all - before.clippedPct.all;
  if (dClip > t.highlightsClippedIncrease)
    v.push_back({QStringLiteral("highlightsClippedIncrease"), QStringLiteral("Clipped highlights (display luma >= 253/255) grew from %1% to %2%: highlight detail was lost.").arg(before.clippedPct.all, 0, 'f', 1).arg(after.clippedPct.all, 0, 'f', 1),
                 dClip, t.highlightsClippedIncrease});
  if (std::max(before.skinPct, after.skinPct) > t.skinMinPct && before.skinPct > 0 && after.skinPct > 0) {
    const double shift = std::fabs(wrapDeg(after.skinHueDeg - before.skinHueDeg));
    if (shift > t.skinHueShiftDeg)
      v.push_back({QStringLiteral("skinHueShiftDeg"), QStringLiteral("Skin tones moved %1 degrees on the vectorscope (limit %2).").arg(shift, 0, 'f', 1).arg(t.skinHueShiftDeg, 0, 'f', 1), shift, t.skinHueShiftDeg});
  }
  const double shiftStops = std::fabs(after.exposureStops - before.exposureStops);
  if (shiftStops > t.exposureShiftStops)
    v.push_back({QStringLiteral("exposureShiftStops"), QStringLiteral("Median exposure moved %1 stops (limit %2).").arg(shiftStops, 0, 'f', 2).arg(t.exposureShiftStops, 0, 'f', 2), shiftStops, t.exposureShiftStops});
  return v;
}

CheckThresholds thresholdsFromJson(const QJsonObject& o) {
  CheckThresholds t;
  const auto get = [&](const char* k, double& dst) {
    if (o.contains(QLatin1String(k))) dst = o.value(QLatin1String(k)).toDouble(dst);
  };
  get("blacksCrushedIncrease", t.blacksCrushedIncrease);
  get("highlightsClippedIncrease", t.highlightsClippedIncrease);
  get("skinHueShiftDeg", t.skinHueShiftDeg);
  get("skinMinPct", t.skinMinPct);
  get("exposureShiftStops", t.exposureShiftStops);
  return t;
}

QJsonArray violationsJson(const std::vector<Violation>& v) {
  QJsonArray a;
  for (const Violation& x : v) a.append(x.toJson());
  return a;
}

} // namespace sf::render
