#include "audio/engine.h"

#include "audio/clap_host.h"
#include "audio/effects.h"
#include "audio/envelope.h"
#include "audio/timebase.h"
#include "core/timeline_doc.h"

#include <QByteArray>
#include <QFile>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <cstring>
#include <mutex>

namespace sf::audio {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kControlGrid = 16; // automation of insert parameters is applied on this grid (absolute samples)

double bessel0(double x) {
  double sum = 1, term = 1;
  for (int k = 1; k < 40; ++k) {
    term *= (x / (2 * k)) * (x / (2 * k));
    sum += term;
  }
  return sum;
}

// Polyphase windowed-sinc interpolation table. Row r holds the taps for fractional position r / kPhases:
// tap j reads source sample floor(p) - half + 1 + j with weight cut * sinc(cut * x) * kaiser(x / half),
// x = (j - half + 1) - frac. cut < 1 when reading faster than real time so the aliasing band is cut first.
struct SincTable {
  static constexpr int kPhases = 256;
  int half = 8;
  int taps = 16;
  std::vector<float> rows; // (kPhases + 1) * taps

  explicit SincTable(double maxSpeed) {
    const double cut = maxSpeed > 1.0 ? 1.0 / maxSpeed : 1.0;
    half = std::min(64, std::max(8, static_cast<int>(std::ceil(8.0 * maxSpeed))));
    taps = half * 2;
    rows.assign(static_cast<size_t>(kPhases + 1) * static_cast<size_t>(taps), 0.0f);
    const double beta = 8.6, norm = bessel0(beta);
    for (int r = 0; r <= kPhases; ++r) {
      const double frac = static_cast<double>(r) / kPhases;
      std::vector<double> w(static_cast<size_t>(taps));
      double sum = 0;
      for (int j = 0; j < taps; ++j) {
        const double x = static_cast<double>(j - half + 1) - frac;
        const double a = kPi * cut * x;
        const double sinc = std::fabs(a) < 1e-12 ? 1.0 : std::sin(a) / a;
        const double rr = x / half;
        const double win = std::fabs(rr) >= 1 ? 0.0 : bessel0(beta * std::sqrt(1 - rr * rr)) / norm;
        w[static_cast<size_t>(j)] = cut * sinc * win;
        sum += w[static_cast<size_t>(j)];
      }
      for (int j = 0; j < taps; ++j) rows[static_cast<size_t>(r) * static_cast<size_t>(taps) + static_cast<size_t>(j)] = static_cast<float>(w[static_cast<size_t>(j)] / sum);
    }
  }
};

struct Knot {
  double local; // item-local timeline sample
  double src;   // source sample index in the stream
};

struct Clip {
  std::shared_ptr<AudioSource> src;
  int node = -1;
  Sample start = 0, end = 0; // timeline samples [start, end)
  // source position of local sample L: constant speed or through the knots
  double srcOrigin = 0;
  double speed = 1;
  std::vector<Knot> knots;
  bool identity = false; // speed 1, integer origin: plain copy
  std::shared_ptr<SincTable> table;
  // gain
  Envelope vol;
  bool hasVolEnv = false;
  double staticVol = 1;
  double fadeIn = 0, fadeOut = 0; // samples
  double ceiling = 1;

  double sourcePos(double local) const {
    if (knots.empty()) return srcOrigin + local * speed;
    if (knots.size() == 1) return knots[0].src + (local - knots[0].local);
    size_t i;
    if (local <= knots.front().local) i = 0;
    else if (local >= knots.back().local) i = knots.size() - 2;
    else {
      i = static_cast<size_t>(std::upper_bound(knots.begin(), knots.end(), local, [](double v, const Knot& k) { return v < k.local; }) - knots.begin()) - 1;
    }
    const Knot& a = knots[i];
    const Knot& b = knots[i + 1];
    const double span = std::max(1.0, b.local - a.local);
    return a.src + (local - a.local) * (b.src - a.src) / span;
  }
};

struct InsertRt {
  std::unique_ptr<Effect> fx;
  QString id;
  bool bypass = false;
  std::vector<std::pair<QString, Envelope>> automation;
  QString sidechain;
  int scNode = -1;

  InsertRt() = default;
  InsertRt(InsertRt&&) noexcept = default;
  InsertRt& operator=(InsertRt&&) noexcept = default;
};

struct SendRt {
  int target = -1;
  float level = 1;
  bool pre = false;
};

enum class Kind { Strip, Bus, Master };

struct Node {
  QString id;
  Kind kind = Kind::Strip;
  MixNode cfg;
  bool muted = false;
  std::vector<InsertRt> inserts;
  Envelope volEnv, panEnv;
  bool hasVolEnv = false, hasPanEnv = false;
  int output = -1;
  std::vector<SendRt> sends;
  std::vector<float> in, out;
  int order = 0;
  // meter (written by the render thread, read by meters())
  NodeMeter meter;

  Node() = default;
  Node(Node&&) noexcept = default;
  Node& operator=(Node&&) noexcept = default;
  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;
};

} // namespace

struct AudioEngine::Impl {
  std::shared_ptr<const TimelineDoc> doc;
  std::shared_ptr<SourceProvider> sources;
  RenderOptions opts;
  std::int64_t fps = 30;

  std::vector<Node> nodes;
  std::vector<int> order; // processing order
  int master = -1;
  std::vector<Clip> clips;
  std::vector<std::vector<int>> clipsOfNode; // by node
  int masterLatency = 0;
  bool anySolo = false;

  // stream position
  bool positioned = false;
  Sample nextOut = 0;
  Sample graphPos = 0;
  std::int64_t discard = 0;

  // scratch
  std::vector<float> scratchSrc, gain, tmp;
  std::vector<double> pos;

  mutable std::mutex meterMutex;
  std::map<QString, NodeMeter> meterSnap;
  mutable std::mutex loudMutex;
  std::unique_ptr<LoudnessMeter> loud;

  Impl(std::shared_ptr<const TimelineDoc> document, std::shared_ptr<SourceProvider> provider, RenderOptions o)
      : doc(std::move(document)), sources(std::move(provider)), opts(o) {
    fps = std::max<std::int64_t>(1, doc->project.fps);
    build();
  }

  // ---------------------------------------------------------------- build

  int findNode(const QString& id, Kind kind) const {
    for (size_t i = 0; i < nodes.size(); ++i)
      if (nodes[i].id == id && nodes[i].kind == kind) return static_cast<int>(i);
    return -1;
  }
  int findAny(const QString& id) const {
    if (id == QLatin1String("master")) return master;
    for (size_t i = 0; i < nodes.size(); ++i)
      if (nodes[i].id == id) return static_cast<int>(i);
    return -1;
  }

  void build() {
    const TimelineDoc& tdoc = *doc;
    const Mixer* mx = tdoc.mixer ? &*tdoc.mixer : nullptr;

    // clips first: which tracks carry audio
    struct Pending {
      const Item* item;
      const Track* track;
      std::shared_ptr<AudioSource> src;
    };
    std::vector<Pending> pending;
    for (const Item& it : tdoc.items) {
      if (it.type() != ItemType::Video && it.type() != ItemType::Audio) continue;
      if (it.muted || !it.assetId) continue;
      const Track* tr = getTrack(tdoc, it.trackId);
      if (!tr || tr->muted) continue;
      if (tr->hidden && opts.hiddenTracksSilent) continue;
      auto src = sources ? sources->source(*it.assetId) : nullptr;
      if (!src) continue;
      pending.push_back({&it, tr, std::move(src)});
    }

    // nodes: strips for tracks with clips or a configured strip, then buses, then master
    nodes.reserve(tdoc.tracks.size() + (mx ? mx->buses.size() : 0) + 1);
    for (const Track& tr : tdoc.tracks) {
      const MixNode* cfg = nullptr;
      if (mx)
        for (const MixNode& n : mx->strips)
          if (n.id == tr.id) cfg = &n;
      const bool hasClips = std::any_of(pending.begin(), pending.end(), [&](const Pending& p) { return p.track->id == tr.id; });
      if (!cfg && !hasClips) continue;
      Node n;
      n.id = tr.id;
      n.kind = Kind::Strip;
      if (cfg) n.cfg = *cfg;
      n.cfg.id = tr.id;
      n.muted = n.cfg.muted || tr.muted;
      nodes.push_back(std::move(n));
    }
    if (mx)
      for (const MixNode& b : mx->buses) {
        Node n;
        n.id = b.id;
        n.kind = Kind::Bus;
        n.cfg = b;
        n.muted = b.muted;
        nodes.push_back(std::move(n));
      }
    {
      Node n;
      n.id = QStringLiteral("master");
      n.kind = Kind::Master;
      if (mx) n.cfg = mx->master;
      n.cfg.id = n.id;
      n.muted = false;
      nodes.push_back(std::move(n));
      master = static_cast<int>(nodes.size()) - 1;
    }
    for (const Node& n : nodes)
      if (n.kind == Kind::Strip && n.cfg.solo) anySolo = true;

    // routing, inserts, automation
    for (size_t ni = 0; ni < nodes.size(); ++ni) {
      Node& n = nodes[ni];
      if (n.kind != Kind::Master) {
        n.output = master;
        if (n.cfg.output) {
          const int t = findNode(*n.cfg.output, Kind::Bus);
          if (t >= 0 && t != static_cast<int>(ni)) n.output = t;
        }
      }
      for (const MixSend& s : n.cfg.sends) {
        const int t = findNode(s.target, Kind::Bus);
        if (t >= 0 && t != static_cast<int>(ni)) n.sends.push_back({t, static_cast<float>(s.level), s.preFader});
      }
      auto kf = [&](const char* key) -> const KeyframeList* {
        const auto it = n.cfg.automation.find(QLatin1String(key));
        return it == n.cfg.automation.end() || it->second.empty() ? nullptr : &it->second;
      };
      if (const auto* v = kf("volume")) {
        n.volEnv = Envelope::fromKeyframes(*v, fps);
        n.hasVolEnv = true;
      }
      if (const auto* p = kf("pan")) {
        n.panEnv = Envelope::fromKeyframes(*p, fps);
        n.hasPanEnv = true;
      }
      for (const MixInsert& mi : n.cfg.inserts) {
        InsertRt rt;
        rt.id = mi.id;
        rt.bypass = mi.bypass;
        if (mi.type == QLatin1String("clap")) {
          rt.fx = makeClapEffect(mi, kRate);
        } else {
          rt.fx = makeEffect(mi.type, kRate, mi.params);
        }
        for (const auto& [param, kfs] : mi.automation)
          if (!kfs.empty()) rt.automation.emplace_back(param, Envelope::fromKeyframes(kfs, fps));
        if (mi.sidechain) rt.sidechain = *mi.sidechain;
        n.inserts.push_back(std::move(rt));
      }
    }
    for (Node& n : nodes)
      for (InsertRt& rt : n.inserts)
        if (!rt.sidechain.isEmpty() && rt.fx && rt.fx->usesSidechain()) rt.scNode = findAny(rt.sidechain);

    computeOrder();

    for (const Node& n : nodes)
      if (n.kind == Kind::Master)
        for (const InsertRt& rt : n.inserts)
          if (rt.fx && !rt.bypass) masterLatency += rt.fx->latencySamples();

    // clips
    clipsOfNode.assign(nodes.size(), {});
    for (const Pending& p : pending) {
      const int node = findNode(p.track->id, Kind::Strip);
      if (node < 0) continue;
      Clip c = makeClip(*p.item, p.src);
      c.node = node;
      clipsOfNode[static_cast<size_t>(node)].push_back(static_cast<int>(clips.size()));
      clips.push_back(std::move(c));
    }
    for (auto& v : clipsOfNode)
      std::stable_sort(v.begin(), v.end(), [&](int a, int b) { return clips[static_cast<size_t>(a)].start < clips[static_cast<size_t>(b)].start; });
  }

  // dependencies: a node is processed after everything that feeds it (routing, sends, sidechains);
  // an edge that would close a loop is dropped at run time because its source comes later in the order
  void computeOrder() {
    const size_t n = nodes.size();
    std::vector<std::vector<int>> deps(n);
    for (size_t i = 0; i < n; ++i) {
      const Node& nd = nodes[i];
      if (nd.output >= 0) deps[static_cast<size_t>(nd.output)].push_back(static_cast<int>(i));
      for (const SendRt& s : nd.sends) deps[static_cast<size_t>(s.target)].push_back(static_cast<int>(i));
      for (const InsertRt& rt : nd.inserts)
        if (rt.scNode >= 0 && rt.scNode != static_cast<int>(i)) deps[i].push_back(rt.scNode);
    }
    std::vector<int> state(n, 0); // 0 new, 1 visiting, 2 done
    order.clear();
    std::function<void(int)> visit = [&](int i) {
      if (state[static_cast<size_t>(i)] != 0) return;
      state[static_cast<size_t>(i)] = 1;
      for (int dep : deps[static_cast<size_t>(i)]) visit(dep);
      state[static_cast<size_t>(i)] = 2;
      nodes[static_cast<size_t>(i)].order = static_cast<int>(order.size());
      order.push_back(i);
    };
    for (size_t i = 0; i < n; ++i)
      if (static_cast<int>(i) != master) visit(static_cast<int>(i));
    visit(master);
  }

  Clip makeClip(const Item& it, std::shared_ptr<AudioSource> src) {
    Clip c;
    c.start = frameToSample(it.startFrame, fps);
    c.end = frameToSample(it.startFrame + it.durationFrames, fps);
    const double streamOffset = std::llround(src->info().startSec * kRate); // source frames are container-relative
    const auto srcSampleOfFrame = [&](Frame f) { return static_cast<double>(frameToSample(f, fps)) - streamOffset; };
    double maxSpeed = 1;
    if (!it.timeRemap.empty()) {
      std::vector<TimeRemapPoint> pts = it.timeRemap;
      std::stable_sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a.frame < b.frame; });
      const Sample base = frameToSample(it.startFrame, fps);
      for (const TimeRemapPoint& p : pts)
        c.knots.push_back({static_cast<double>(frameToSample(it.startFrame + p.frame, fps) - base), srcSampleOfFrame(p.sourceFrame)});
      for (size_t i = 0; i + 1 < c.knots.size(); ++i) {
        const double span = std::max(1.0, c.knots[i + 1].local - c.knots[i].local);
        maxSpeed = std::max(maxSpeed, std::fabs((c.knots[i + 1].src - c.knots[i].src) / span));
      }
    } else {
      c.speed = it.speed;
      c.srcOrigin = srcSampleOfFrame(it.sourceInFrame.value_or(0));
      maxSpeed = std::max(1.0, std::fabs(it.speed));
    }
    c.identity = c.knots.empty() && c.speed == 1.0 && c.srcOrigin == std::floor(c.srcOrigin);
    if (!c.identity) c.table = std::make_shared<SincTable>(maxSpeed);
    c.src = std::move(src);

    c.ceiling = opts.volumeCeiling;
    c.staticVol = it.volume;
    const auto kf = it.keyframes.find(QStringLiteral("volume"));
    if (kf != it.keyframes.end() && !kf->second.empty()) {
      c.vol = Envelope::fromKeyframes(kf->second, fps, it.startFrame);
      c.hasVolEnv = true;
    }
    std::visit(
        [&](const auto& props) {
          if constexpr (std::is_base_of_v<FadeProps, std::decay_t<decltype(props)>>) {
            c.fadeIn = framesToSamplesF(props.fadeInFrames, fps);
            c.fadeOut = framesToSamplesF(props.fadeOutFrames, fps);
          }
        },
        it.props);
    return c;
  }

  // ---------------------------------------------------------------- render

  void resetState() {
    for (Node& n : nodes)
      for (InsertRt& rt : n.inserts)
        if (rt.fx) rt.fx->reset();
    {
      const std::lock_guard lock(meterMutex);
      for (Node& n : nodes) n.meter = NodeMeter{};
    }
    {
      const std::lock_guard lock(loudMutex);
      if (loud) loud->reset();
    }
    positioned = false;
  }

  void renderClip(const Clip& c, Sample blockStart, int n, float* dst) {
    const Sample a = std::max(blockStart, c.start), b = std::min(blockStart + n, c.end);
    if (a >= b) return;
    const int m = static_cast<int>(b - a);
    const Sample local0 = a - c.start;
    float* out = dst + (a - blockStart) * 2;
    const Sample dur = c.end - c.start;

    // gain per sample
    gain.resize(static_cast<size_t>(m));
    for (int i = 0; i < m; ++i) {
      const Sample L = local0 + i;
      double g = c.hasVolEnv ? c.vol.at(static_cast<double>(a + i)) : c.staticVol;
      if (c.fadeIn > 0 && static_cast<double>(L) < c.fadeIn) g *= std::max(0.0, static_cast<double>(L) / c.fadeIn);
      const double fromEnd = static_cast<double>(dur - L);
      if (c.fadeOut > 0 && fromEnd < c.fadeOut) g *= std::max(0.0, fromEnd / c.fadeOut);
      gain[static_cast<size_t>(i)] = static_cast<float>(std::min(c.ceiling, std::max(0.0, g)));
    }

    if (c.identity) {
      const std::int64_t s0 = static_cast<std::int64_t>(c.srcOrigin) + local0;
      scratchSrc.resize(static_cast<size_t>(m) * 2);
      c.src->read(s0, m, scratchSrc.data());
      for (int i = 0; i < m; ++i) {
        out[i * 2] += scratchSrc[static_cast<size_t>(i) * 2] * gain[static_cast<size_t>(i)];
        out[i * 2 + 1] += scratchSrc[static_cast<size_t>(i) * 2 + 1] * gain[static_cast<size_t>(i)];
      }
      return;
    }

    const SincTable& t = *c.table;
    pos.resize(static_cast<size_t>(m));
    double lo = 1e300, hi = -1e300;
    for (int i = 0; i < m; ++i) {
      const double p = c.sourcePos(static_cast<double>(local0 + i));
      pos[static_cast<size_t>(i)] = p;
      lo = std::min(lo, p);
      hi = std::max(hi, p);
    }
    const std::int64_t first = static_cast<std::int64_t>(std::floor(lo)) - t.half + 1;
    const std::int64_t last = static_cast<std::int64_t>(std::floor(hi)) + t.half;
    const std::int64_t span = last - first + 1;
    if (span > 4'000'000) return; // absurd remap segment; nothing sensible to read
    scratchSrc.resize(static_cast<size_t>(span) * 2);
    c.src->read(first, span, scratchSrc.data());
    for (int i = 0; i < m; ++i) {
      const double p = pos[static_cast<size_t>(i)];
      const double fl = std::floor(p);
      const double frac = p - fl;
      const double fr = frac * SincTable::kPhases;
      const int r0 = std::min(SincTable::kPhases - 1, static_cast<int>(fr));
      const float w1 = static_cast<float>(fr - r0), w0 = 1.0f - w1;
      const float* row0 = &t.rows[static_cast<size_t>(r0) * static_cast<size_t>(t.taps)];
      const float* row1 = row0 + t.taps;
      const float* s = &scratchSrc[static_cast<size_t>(static_cast<std::int64_t>(fl) - t.half + 1 - first) * 2];
      float accL = 0, accR = 0;
      for (int j = 0; j < t.taps; ++j) {
        const float w = row0[j] * w0 + row1[j] * w1;
        accL += w * s[j * 2];
        accR += w * s[j * 2 + 1];
      }
      out[i * 2] += accL * gain[static_cast<size_t>(i)];
      out[i * 2 + 1] += accR * gain[static_cast<size_t>(i)];
    }
  }

  void runInserts(Node& nd, Sample abs, int n) {
    for (InsertRt& rt : nd.inserts) {
      if (!rt.fx || rt.bypass) continue;
      const float* sc = nullptr;
      if (rt.scNode >= 0) {
        const Node& src = nodes[static_cast<size_t>(rt.scNode)];
        if (src.order < nd.order && src.out.size() >= static_cast<size_t>(n) * 2) sc = src.out.data();
      }
      float* io = nd.in.data();
      if (rt.automation.empty()) {
        rt.fx->process(io, n, sc);
        continue;
      }
      // segments between grid points, absolute-sample aligned so block size never matters
      int done = 0;
      while (done < n) {
        const Sample at = abs + done;
        const int len = std::min<Sample>(n - done, kControlGrid - (at % kControlGrid + kControlGrid) % kControlGrid);
        // evaluated at the grid point at or before this segment, so a block boundary inside a grid cell changes nothing
        const Sample gridAt = at - (at % kControlGrid + kControlGrid) % kControlGrid;
        for (const auto& [param, env] : rt.automation) rt.fx->setParam(param, env.at(static_cast<double>(gridAt)));
        rt.fx->process(io + done * 2, len, sc ? sc + done * 2 : nullptr);
        done += len;
      }
    }
  }

  void measure(Node& nd, int n) {
    float pk[2] = {0, 0};
    double sq[2] = {0, 0};
    const float* o = nd.out.data();
    for (int i = 0; i < n; ++i)
      for (int ch = 0; ch < 2; ++ch) {
        const float v = o[i * 2 + ch];
        pk[ch] = std::max(pk[ch], std::fabs(v));
        sq[ch] += static_cast<double>(v) * v;
      }
    const float fall = static_cast<float>(std::pow(10.0, -static_cast<double>(n) / kRate));
    float gr = 0;
    for (const InsertRt& rt : nd.inserts)
      if (rt.fx && !rt.bypass) gr = std::max(gr, static_cast<float>(rt.fx->gainReductionDb()));
    const std::lock_guard lock(meterMutex);
    for (int ch = 0; ch < 2; ++ch) {
      nd.meter.peak[ch] = pk[ch];
      nd.meter.peakHold[ch] = std::max(pk[ch], nd.meter.peakHold[ch] * fall);
      nd.meter.rms[ch] = n > 0 ? static_cast<float>(std::sqrt(sq[ch] / n)) : 0.0f;
    }
    nd.meter.gainReductionDb = gr;
  }

  // One block of the graph at timeline position `abs`; the master's (latency-delayed) output lands in `outBlock`.
  void processBlock(Sample abs, int n, float* outBlock) {
    const size_t len = static_cast<size_t>(n) * 2;
    for (Node& nd : nodes) {
      nd.in.assign(len, 0.0f);
      if (nd.out.size() < len) nd.out.resize(len);
    }
    for (Node& nd : nodes) {
      if (nd.kind != Kind::Strip) continue;
      for (int ci : clipsOfNode[static_cast<size_t>(&nd - nodes.data())]) {
        const Clip& c = clips[static_cast<size_t>(ci)];
        if (c.start >= abs + n) break; // sorted by start
        renderClip(c, abs, n, nd.in.data());
      }
    }

    for (int idx : order) {
      Node& nd = nodes[static_cast<size_t>(idx)];
      runInserts(nd, abs, n);
      const bool silent = nd.kind == Kind::Strip ? (nd.muted || (anySolo && !nd.cfg.solo)) : nd.muted;

      if (!silent)
        for (const SendRt& s : nd.sends)
          if (s.pre && nodes[static_cast<size_t>(s.target)].order > nd.order) addScaled(nodes[static_cast<size_t>(s.target)].in.data(), nd.in.data(), n, s.level);

      // fader + pan, in place into out
      float* out = nd.out.data();
      const float* in = nd.in.data();
      if (silent) {
        std::memset(out, 0, len * sizeof(float));
      } else {
        const bool volStatic = !nd.hasVolEnv, panStatic = !nd.hasPanEnv;
        if (volStatic && panStatic) {
          const float v = static_cast<float>(nd.cfg.volume);
          if (nd.cfg.pan == 0.0) {
            if (v == 1.0f) std::memcpy(out, in, len * sizeof(float));
            else
              for (size_t i = 0; i < len; ++i) out[i] = in[i] * v;
          } else {
            const PanGains pg = panGains(nd.cfg.pan);
            for (int i = 0; i < n; ++i) {
              out[i * 2] = in[i * 2] * v * pg.l;
              out[i * 2 + 1] = in[i * 2 + 1] * v * pg.r;
            }
          }
        } else {
          for (int i = 0; i < n; ++i) {
            const double at = static_cast<double>(abs + i);
            const float v = static_cast<float>(volStatic ? nd.cfg.volume : std::max(0.0, nd.volEnv.at(at)));
            const PanGains pg = panGains(panStatic ? nd.cfg.pan : nd.panEnv.at(at));
            out[i * 2] = in[i * 2] * v * pg.l;
            out[i * 2 + 1] = in[i * 2 + 1] * v * pg.r;
          }
        }
        for (const SendRt& s : nd.sends)
          if (!s.pre && nodes[static_cast<size_t>(s.target)].order > nd.order) addScaled(nodes[static_cast<size_t>(s.target)].in.data(), out, n, s.level);
      }
      measure(nd, n);
      if (nd.output >= 0 && !silent && nodes[static_cast<size_t>(nd.output)].order > nd.order) addScaled(nodes[static_cast<size_t>(nd.output)].in.data(), out, n, 1.0f);
    }
    std::memcpy(outBlock, nodes[static_cast<size_t>(master)].out.data(), len * sizeof(float));
    const std::lock_guard lock(loudMutex);
    if (loud) loud->process(outBlock, n);
  }

  static void addScaled(float* dst, const float* src, int n, float level) {
    const size_t len = static_cast<size_t>(n) * 2;
    if (level == 1.0f)
      for (size_t i = 0; i < len; ++i) dst[i] += src[i];
    else
      for (size_t i = 0; i < len; ++i) dst[i] += src[i] * level;
  }

  void render(Sample start, std::int64_t count, float* out) {
    if (count <= 0) return;
    if (!positioned || start != nextOut) {
      resetState();
      positioned = true;
      graphPos = start;
      discard = masterLatency;
    }
    std::int64_t got = 0;
    const int maxBlock = std::max(16, opts.blockSize);
    tmp.resize(static_cast<size_t>(maxBlock) * 2);
    while (got < count) {
      const std::int64_t remaining = discard + (count - got);
      const int n = static_cast<int>(std::min<std::int64_t>(maxBlock, remaining));
      processBlock(graphPos, n, tmp.data());
      graphPos += n;
      int skip = static_cast<int>(std::min<std::int64_t>(discard, n));
      discard -= skip;
      const int take = n - skip;
      if (take > 0) {
        std::memcpy(out + got * 2, tmp.data() + static_cast<size_t>(skip) * 2, static_cast<size_t>(take) * 2 * sizeof(float));
        got += take;
      }
    }
    nextOut = start + count;
  }
};

AudioEngine::AudioEngine(std::shared_ptr<const TimelineDoc> doc, std::shared_ptr<SourceProvider> sources, RenderOptions opts)
    : d(std::make_unique<Impl>(std::move(doc), std::move(sources), opts)) {}
AudioEngine::~AudioEngine() = default;

void AudioEngine::render(std::int64_t start, std::int64_t count, float* out) { d->render(start, count, out); }
void AudioEngine::reset() { d->resetState(); }

MeterSnapshot AudioEngine::meters() const {
  MeterSnapshot s;
  const std::lock_guard lock(d->meterMutex);
  for (const Node& n : d->nodes) s[n.id] = n.meter;
  return s;
}

void AudioEngine::enableLoudness(bool on) {
  const std::lock_guard lock(d->loudMutex);
  if (on && !d->loud) d->loud = std::make_unique<LoudnessMeter>();
  if (!on) d->loud.reset();
}

AudioEngine::Loudness AudioEngine::loudness() const {
  const std::lock_guard lock(d->loudMutex);
  if (!d->loud) return {};
  return {d->loud->momentary(), d->loud->shortTerm(), d->loud->integrated(), d->loud->truePeakDb()};
}

std::map<QString, QString> AudioEngine::pluginStates() const {
  std::map<QString, QString> out;
  for (Node& n : d->nodes)
    for (InsertRt& rt : n.inserts) {
      if (!rt.fx) continue;
      const QByteArray st = rt.fx->saveState();
      if (!st.isEmpty()) out[rt.id] = QString::fromLatin1(st.toBase64());
    }
  return out;
}

std::vector<float> renderRange(std::shared_ptr<const TimelineDoc> doc, std::shared_ptr<SourceProvider> sources, std::int64_t start,
                               std::int64_t count, const RenderOptions& opts) {
  std::vector<float> out(static_cast<size_t>(std::max<std::int64_t>(0, count)) * 2);
  AudioEngine engine(std::move(doc), std::move(sources), opts);
  const std::int64_t pre = std::min(std::max<std::int64_t>(0, opts.prerollSamples), std::max<std::int64_t>(0, start));
  if (pre > 0) {
    std::vector<float> warm(static_cast<size_t>(pre) * 2);
    engine.render(start - pre, pre, warm.data());
  }
  engine.render(start, count, out.data());
  return out;
}

std::int64_t docDurationSamples(const TimelineDoc& doc) {
  return frameToSample(docDurationFrames(doc), std::max<std::int64_t>(1, doc.project.fps));
}

bool writeWav(const QString& path, const float* data, std::int64_t frames, bool float32) {
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly)) return false;
  const int bytes = float32 ? 4 : 2;
  const std::uint32_t dataSize = static_cast<std::uint32_t>(frames * 2 * bytes);
  auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
  f.write("RIFF", 4);
  u32(36 + dataSize);
  f.write("WAVEfmt ", 8);
  u32(16);
  u16(float32 ? 3 : 1);
  u16(2);
  u32(kRate);
  u32(static_cast<std::uint32_t>(kRate * 2 * bytes));
  u16(static_cast<std::uint16_t>(2 * bytes));
  u16(static_cast<std::uint16_t>(bytes * 8));
  f.write("data", 4);
  u32(dataSize);
  if (float32) {
    f.write(reinterpret_cast<const char*>(data), static_cast<qint64>(frames) * 2 * 4);
  } else {
    std::vector<std::int16_t> pcm(static_cast<size_t>(frames) * 2);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<std::int16_t>(std::lround(std::min(1.0f, std::max(-1.0f, data[i])) * 32767.0f));
    f.write(reinterpret_cast<const char*>(pcm.data()), static_cast<qint64>(pcm.size() * 2));
  }
  return f.error() == QFileDevice::NoError;
}

} // namespace sf::audio
