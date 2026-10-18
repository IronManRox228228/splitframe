// This machine and what it can spare: system.profile, system.overrides.set, llm.fitAdvice.
// The numbers come from sys/system_profile (DXGI, CPU topology, RAM, power, a cached < 3 s hardware smoke test); the decisions are a pure
// function of them (sys::decide) that is re-evaluated against the live VRAM budget on every call and, once monitoring runs, every few seconds.

#include "engine/cmd_util.h"
#include "render/media_provider.h"
#include "render/offscreen.h"
#include "sys/system_profile.h"

#include <QFileInfo>

#include <chrono>

namespace sf::engine {

// One process, one machine: the service (profile, smoke cache, overrides, live monitor) is shared by every Engine.
sys::SystemService& sharedSystemService() {
  return sys::sharedSystemService();
}

namespace {

sys::SystemService& service() { return sharedSystemService(); }

class NullMedia final : public render::MediaProvider {
public:
  render::Visual visual(const Item&, Frame, double) override { return {}; }
};

// One 1080p frame through the compositor (an empty timeline: clear, blend target, output transform): the cost of the GPU path itself.
void compositorProbe(Engine& engine, sys::SmokeResult& r) {
  QString err;
  render::OffscreenRenderer* off = engine.renderer(&err);
  if (!off) {
    r.compositorOk = false;
    r.compositorError = err.isEmpty() ? QStringLiteral("no D3D11 device") : err;
    return;
  }
  TimelineDoc doc;
  doc.project.width = 1920;
  doc.project.height = 1080;
  doc.project.fps = 30;
  NullMedia media;
  off->compositor().setDelivery(std::nullopt);
  off->renderAndWait(doc, 0, media); // warm-up: pipelines, targets
  const auto t0 = std::chrono::steady_clock::now();
  off->renderAndWait(doc, 0, media);
  r.compositorMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  r.compositorOk = true;
}

QJsonObject wiringNote() {
  return QJsonObject{
    {q("applied"), QJsonArray{q("exportEncoder"), q("hwDecode"), q("cacheMb"), q("mlThreads"), q("backgroundPriority")}},
    {q("previewScale"), q("not wired: no preview-scale setting yet")},
    {q("note"), q("exportEncoder is wired to export.start; hwDecode and cacheMb are wired to preview FrameService; mlThreads and backgroundPriority to jobs. previewScale: not wired: no preview-scale setting yet.")}
  };
}

Result profileCmd(CallContext& c, const QJsonObject& a) {
  sys::SystemService& s = service();
  const bool refresh = a.value(q("refresh")).toBool(false);
  const bool wantSmoke = a.value(q("smoke")).toBool(true);
  s.input(); // loads the persisted overrides
  s.startMonitoring(); // idempotent: the live budget is re-read every few seconds from now on (needs the event loop)
  s.pollNow();
  QJsonObject o;
  Engine* engine = c.engine;
  sys::SmokeOptions opt;
  opt.compositor = [engine](sys::SmokeResult& r) { compositorProbe(*engine, r); };
  QJsonValue smoke = QJsonValue(QJsonValue::Null);
  if (wantSmoke) smoke = s.smoke(refresh, opt).toJson();
  const sys::SystemProfile& p = s.profile();
  QJsonObject prof = p.toJson();
  prof.insert(q("live"), s.live(0).toJson());
  o.insert(q("profile"), prof);
  o.insert(q("smoke"), smoke);
  const sys::Decisions d = s.decisions();
  o.insert(q("decisions"), d.toJson());
  o.insert(q("overrides"), s.overrides().toJson());
  o.insert(q("overrideKeys"), QJsonArray::fromStringList(sys::Overrides::keys()));
  o.insert(q("wiring"), wiringNote());
  o.insert(q("dataDir"), sys::SystemService::dataDir());
  return Result::success(o);
}

Result overridesCmd(CallContext&, const QJsonObject& a) {
  sys::SystemService& s = service();
  s.input(); // load what is persisted
  const QString key = a.value(q("key")).toString();
  QString err;
  if (!s.setOverride(key, a.value(q("value")), &err)) return invalid(err);
  s.pollNow();
  return Result::success(QJsonObject{{q("key"), key}, {q("overrides"), s.overrides().toJson()}, {q("decisions"), s.decisions().toJson()}});
}

Result fitCmd(CallContext&, const QJsonObject& a) {
  sys::FitRequest r;
  QString source;
  if (a.contains(q("ggufPath"))) {
    const QFileInfo fi(a.value(q("ggufPath")).toString());
    if (!fi.exists() || !fi.isFile()) return notFound(QStringLiteral("No file at %1.").arg(fi.filePath()), QStringLiteral("Pass sizeBytes instead, or a valid ggufPath."));
    r.sizeBytes = fi.size();
    source = q("ggufPath file size");
  } else if (a.contains(q("sizeBytes"))) {
    r.sizeBytes = static_cast<qint64>(a.value(q("sizeBytes")).toDouble());
    source = q("sizeBytes");
  }
  if (r.sizeBytes <= 0) return invalid(QStringLiteral("Pass ggufPath or sizeBytes (> 0)."));
  r.layers = a.value(q("layers")).toInt(0);
  r.isMoe = a.value(q("isMoe")).toBool(false);
  r.wantedCtx = a.value(q("ctx")).toInt(0);
  QString budgetSource;
  if (a.contains(q("vramBudgetBytes"))) {
    r.budgetBytes = static_cast<qint64>(a.value(q("vramBudgetBytes")).toDouble());
    budgetSource = q("vramBudgetBytes (given)");
  } else {
    sys::SystemService& s = service();
    const sys::LiveState live = s.live(0);
    r.budgetBytes = live.vram.valid ? live.vram.localHeadroom() : 0;
    budgetSource = live.vram.valid ? q("live DXGI headroom (budget minus this process's usage)") : q("no GPU budget available");
  }
  const sys::FitAdvice f = sys::fitAdvice(r);
  QJsonObject o = f.toJson();
  o.insert(q("modelBytes"), static_cast<double>(r.sizeBytes));
  o.insert(q("budgetBytes"), static_cast<double>(r.budgetBytes));
  o.insert(q("sizeSource"), source);
  o.insert(q("budgetSource"), budgetSource);
  o.insert(q("estimate"), q("A pure estimate (no model is loaded): KV cache 4 KB per token per layer, 600 MB compute buffers, MoE experts 85 % of a layer."));
  return Result::success(o);
}

} // namespace

void registerSystemCommands(Engine& e) {
  e.add(spec("system.profile", "", "engine", false,
             QStringLiteral("This machine and what it can spare. profile: gpus[] (name, vendor, vendorId, driverVersion, dedicated VRAM; primaryGpu is the one the renderer uses), cpu (physical / logical cores, performance vs efficiency cores), "
                            "live {vram: DXGI budget, this process's usage and headroom for local and non-local memory; memory: RAM and commit limit; power: AC or battery}. smoke: a cached < 3 s test (JSON in the app config dir, keyed by "
                            "adapter LUID + driver version + CPU name; re-run only when the key changes or with refresh=true): which FFmpeg hardware encoders (h264_nvenc, hevc_nvenc, h264_amf, h264_qsv) open and encode 10 tiny frames, "
                            "D3D11VA decode, one 1080p compositor frame (ms). decisions: previewScale, hwDecode, exportEncoder (software when VRAM headroom is under 1 GB), mlDevice (cpu unless headroom is over 3 GB with no other heavy VRAM user, AC), "
                            "mlThreads, backgroundPriority, cacheMb, each with a reason in `reasons`; they are recomputed from the live budget on every call, so they change when another process (an LLM) takes VRAM. "
                            "Decisions are advice: see `wiring`."),
             js::obj({{q("refresh"), js::boolean(QStringLiteral("Re-run the smoke test even when the cache is valid"))}, {q("smoke"), js::boolean(QStringLiteral("false: skip the smoke test (default true: cached or run)"))}}), profileCmd, false));
  e.add(spec("system.overrides.set", "", "engine", false,
             QStringLiteral("Pin one resource decision (persisted in the app config dir). key: previewScale (0.25..1), hwDecode (bool), exportEncoder (software | nvenc | amf | qsv), mlDevice (cpu | gpu), mlThreads (1..64), "
                            "backgroundPriority (idle | below_normal | normal), cacheMb (64..65536). value null removes the override and gives the decision back to the policy. Returns the overrides and the decisions."),
             js::obj({{q("key"), js::choice(sys::Overrides::keys())}, {q("value"), js::anything(QStringLiteral("The pinned value, or null to clear"))}}, {q("key")}), overridesCmd, false));
  e.add(spec("llm.fitAdvice", "", "engine", false,
             QStringLiteral("Suggest llama.cpp offload flags for a model without loading it: -ngl (layers on the GPU), --n-cpu-moe (MoE: experts of that many layers stay on the CPU) and a context size, from the model size (`ggufPath` or `sizeBytes`), "
                            "`layers` (guessed from the size when missing), `isMoe`, `ctx` (default: the largest of 32768..4096 that fits fully) and the VRAM budget (`vramBudgetBytes`, default the live headroom). Pure estimate: KV cache 4 KB per token "
                            "per layer, 600 MB of compute buffers. Returns {ngl, nCpuMoe, ctx, flags, fitsFully, gpuUseful, estimatedVramMiB, notes}."),
             js::obj({{q("ggufPath"), js::str()}, {q("sizeBytes"), js::number(QString(), 1)}, {q("layers"), js::integer(QString(), 1, 512)}, {q("isMoe"), js::boolean()}, {q("ctx"), js::integer(QString(), 256, 1048576)},
                      {q("vramBudgetBytes"), js::number(QString(), 0)}}),
             fitCmd, false));
}

} // namespace sf::engine
