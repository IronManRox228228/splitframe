#include "sys/system_profile.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dxgi1_6.h>
#include <intrin.h>
#include <wrl/client.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/log.h>
#include <libavutil/pixfmt.h>
}

namespace sf::sys {

namespace {

double round2(double v) { return std::round(v * 100.0) / 100.0; }
double mib(qint64 bytes) { return round2(static_cast<double>(bytes) / static_cast<double>(kMiB)); }
QString q(const char* s) { return QString::fromLatin1(s); }

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

} // namespace

// ---------------------------------------------------------------- JSON of the plain structs

QString GpuAdapter::vendor() const {
  switch (vendorId) {
  case 0x10DE: return q("nvidia");
  case 0x1002:
  case 0x1022: return q("amd");
  case 0x8086: return q("intel");
  case 0x1414: return q("microsoft");
  default: return q("other");
  }
}

QJsonObject GpuAdapter::toJson() const {
  return QJsonObject{{q("name"), name},
                     {q("vendor"), vendor()},
                     {q("vendorId"), static_cast<double>(vendorId)},
                     {q("deviceId"), static_cast<double>(deviceId)},
                     {q("driverVersion"), driverVersion},
                     {q("dedicatedVramMiB"), mib(dedicatedVramBytes)},
                     {q("dedicatedSystemMiB"), mib(dedicatedSystemBytes)},
                     {q("sharedSystemMiB"), mib(sharedSystemBytes)},
                     {q("luid"), QString::number(luid, 16)},
                     {q("software"), software}};
}

QJsonObject VramBudget::toJson() const {
  if (!valid) return QJsonObject{{q("valid"), false}};
  return QJsonObject{{q("valid"), true},
                     {q("localBudgetMiB"), mib(localBudget)},
                     {q("localUsageMiB"), mib(localUsage)},
                     {q("localHeadroomMiB"), mib(localHeadroom())},
                     {q("nonLocalBudgetMiB"), mib(nonLocalBudget)},
                     {q("nonLocalUsageMiB"), mib(nonLocalUsage)},
                     {q("note"), q("usage is this process only; the budget shrinks when another process takes video memory")}};
}

QJsonObject CpuInfo::toJson() const {
  return QJsonObject{{q("name"), name}, {q("physicalCores"), physicalCores}, {q("logicalCores"), logicalCores}, {q("performanceCores"), performanceCores}, {q("efficiencyCores"), efficiencyCores}, {q("hybrid"), hybrid}};
}

QJsonObject MemoryInfo::toJson() const {
  return QJsonObject{{q("totalPhysMiB"), mib(totalPhys)},      {q("availPhysMiB"), mib(availPhys)},       {q("commitLimitMiB"), mib(commitLimit)},
                     {q("commitTotalMiB"), mib(commitTotal)}, {q("commitHeadroomMiB"), mib(commitHeadroom())}};
}

QJsonObject PowerInfo::toJson() const {
  return QJsonObject{{q("known"), known},
                     {q("source"), !known ? q("unknown") : onBattery ? q("battery") : q("ac")},
                     {q("batterySaver"), batterySaver},
                     {q("batteryPercent"), batteryPercent >= 0 ? QJsonValue(batteryPercent) : QJsonValue(QJsonValue::Null)}};
}

QJsonObject LiveState::toJson() const { return QJsonObject{{q("vram"), vram.toJson()}, {q("memory"), memory.toJson()}, {q("power"), power.toJson()}}; }

QString SystemProfile::cacheKey() const {
  const GpuAdapter* g = gpu();
  return QStringLiteral("%1|%2|%3").arg(g ? QString::number(g->luid, 16) : q("nogpu"), g ? g->driverVersion : QString(), cpu.name);
}

QJsonObject SystemProfile::toJson() const {
  QJsonArray a;
  for (const GpuAdapter& g : gpus) a.append(g.toJson());
  return QJsonObject{{q("gpus"), a}, {q("primaryGpu"), primaryGpu}, {q("cpu"), cpu.toJson()}, {q("live"), live.toJson()}, {q("cacheKey"), cacheKey()}};
}

// ---------------------------------------------------------------- hardware queries (Windows)

#ifdef _WIN32
namespace {

using Microsoft::WRL::ComPtr;

GpuAdapter describe(IDXGIAdapter1* adapter) {
  GpuAdapter g;
  DXGI_ADAPTER_DESC1 d{};
  if (FAILED(adapter->GetDesc1(&d))) return g;
  g.name = QString::fromWCharArray(d.Description);
  g.vendorId = d.VendorId;
  g.deviceId = d.DeviceId;
  g.dedicatedVramBytes = static_cast<qint64>(d.DedicatedVideoMemory);
  g.dedicatedSystemBytes = static_cast<qint64>(d.DedicatedSystemMemory);
  g.sharedSystemBytes = static_cast<qint64>(d.SharedSystemMemory);
  g.luid = (static_cast<quint64>(static_cast<quint32>(d.AdapterLuid.HighPart)) << 32) | d.AdapterLuid.LowPart;
  g.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || d.VendorId == 0x1414;
  LARGE_INTEGER umd{};
  if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
    g.driverVersion = QStringLiteral("%1.%2.%3.%4").arg(HIWORD(umd.HighPart)).arg(LOWORD(umd.HighPart)).arg(HIWORD(umd.LowPart)).arg(LOWORD(umd.LowPart));
  }
  return g;
}

QString cpuBrand() {
  int regs[4] = {0, 0, 0, 0};
  __cpuid(regs, static_cast<int>(0x80000000));
  if (static_cast<unsigned>(regs[0]) < 0x80000004u) return {};
  char brand[49] = {};
  for (unsigned i = 0; i < 3; ++i) {
    __cpuid(regs, static_cast<int>(0x80000002u + i));
    std::memcpy(brand + i * 16, regs, 16);
  }
  return QString::fromLatin1(brand).simplified();
}

CpuInfo queryCpu() {
  CpuInfo c;
  c.name = cpuBrand();
  DWORD len = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
  std::vector<unsigned char> buf(len);
  if (len > 0 && GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len)) {
    std::vector<int> classes;
    for (DWORD off = 0; off < len;) {
      const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
      if (info->Relationship == RelationProcessorCore) {
        ++c.physicalCores;
        classes.push_back(info->Processor.EfficiencyClass);
        for (WORD g = 0; g < info->Processor.GroupCount; ++g) {
          KAFFINITY m = info->Processor.GroupMask[g].Mask;
          while (m) {
            c.logicalCores += static_cast<int>(m & 1);
            m >>= 1;
          }
        }
      }
      if (info->Size == 0) break;
      off += info->Size;
    }
    if (!classes.empty()) {
      const int top = *std::max_element(classes.begin(), classes.end());
      for (int k : classes) (k == top ? c.performanceCores : c.efficiencyCores)++;
      c.hybrid = c.efficiencyCores > 0;
    }
  }
  if (c.logicalCores <= 0) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    c.logicalCores = static_cast<int>(si.dwNumberOfProcessors);
  }
  if (c.physicalCores <= 0) c.physicalCores = c.logicalCores;
  if (c.performanceCores <= 0) c.performanceCores = c.physicalCores;
  return c;
}

MemoryInfo queryMemory() {
  MemoryInfo m;
  MEMORYSTATUSEX s{};
  s.dwLength = sizeof(s);
  if (GlobalMemoryStatusEx(&s)) {
    m.totalPhys = static_cast<qint64>(s.ullTotalPhys);
    m.availPhys = static_cast<qint64>(s.ullAvailPhys);
    m.commitLimit = static_cast<qint64>(s.ullTotalPageFile); // the system commit limit
    m.commitTotal = static_cast<qint64>(s.ullTotalPageFile - s.ullAvailPageFile);
  }
  return m;
}

PowerInfo queryPower() {
  PowerInfo p;
  SYSTEM_POWER_STATUS s{};
  if (!GetSystemPowerStatus(&s)) return p;
  p.known = s.ACLineStatus != 255;
  p.onBattery = s.ACLineStatus == 0;
  p.batterySaver = (s.SystemStatusFlag & 1) != 0;
  p.batteryPercent = s.BatteryLifePercent == 255 ? -1 : static_cast<int>(s.BatteryLifePercent);
  return p;
}

} // namespace

SystemProfile querySystem() {
  SystemProfile p;
  ComPtr<IDXGIFactory1> factory;
  if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    for (UINT i = 0;; ++i) {
      ComPtr<IDXGIAdapter1> a;
      if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
      p.gpus.push_back(describe(a.Get()));
    }
    // the adapter the renderer uses: the high-performance one (same choice as media/hwdevice), never the software rasteriser
    ComPtr<IDXGIFactory6> f6;
    if (SUCCEEDED(factory.As(&f6))) {
      ComPtr<IDXGIAdapter1> a;
      if (SUCCEEDED(f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)))) {
        const quint64 luid = describe(a.Get()).luid;
        for (size_t i = 0; i < p.gpus.size(); ++i)
          if (p.gpus[i].luid == luid && !p.gpus[i].software) p.primaryGpu = static_cast<int>(i);
      }
    }
    if (p.primaryGpu < 0)
      for (size_t i = 0; i < p.gpus.size(); ++i)
        if (!p.gpus[i].software) {
          p.primaryGpu = static_cast<int>(i);
          break;
        }
  }
  p.cpu = queryCpu();
  p.live = queryLive(p.gpu() ? p.gpu()->luid : 0);
  return p;
}

LiveState queryLive(quint64 luid) {
  LiveState s;
  s.memory = queryMemory();
  s.power = queryPower();
  if (luid != 0) {
    ComPtr<IDXGIFactory4> f4;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f4)))) {
      LUID l{};
      l.LowPart = static_cast<DWORD>(luid & 0xFFFFFFFFu);
      l.HighPart = static_cast<LONG>(luid >> 32);
      ComPtr<IDXGIAdapter3> a3;
      DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonLocal{};
      if (SUCCEEDED(f4->EnumAdapterByLuid(l, IID_PPV_ARGS(&a3))) && SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
          SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal))) {
        s.vram.valid = true;
        s.vram.localBudget = static_cast<qint64>(local.Budget);
        s.vram.localUsage = static_cast<qint64>(local.CurrentUsage);
        s.vram.nonLocalBudget = static_cast<qint64>(nonLocal.Budget);
        s.vram.nonLocalUsage = static_cast<qint64>(nonLocal.CurrentUsage);
      }
    }
  }
  return s;
}

#else // not Windows: nothing to ask

SystemProfile querySystem() {
  SystemProfile p;
  p.cpu.logicalCores = p.cpu.physicalCores = p.cpu.performanceCores = 1;
  return p;
}
LiveState queryLive(quint64) { return {}; }

#endif

// ---------------------------------------------------------------- smoke test

QJsonObject EncoderProbe::toJson() const {
  QJsonObject o{{q("name"), name}, {q("present"), present}, {q("works"), works}, {q("ms"), round2(ms)}};
  if (!error.isEmpty()) o.insert(q("error"), error);
  return o;
}

bool SmokeResult::encoderWorks(const QString& name) const {
  for (const EncoderProbe& e : encoders)
    if (e.name == name) return e.works;
  return false;
}

QJsonObject SmokeResult::toJson() const {
  QJsonArray enc;
  for (const EncoderProbe& e : encoders) enc.append(e.toJson());
  QJsonObject comp{{q("ran"), compositorRan}, {q("ok"), compositorOk}, {q("ms"), round2(compositorMs)}};
  if (!compositorError.isEmpty()) comp.insert(q("error"), compositorError);
  return QJsonObject{{q("ran"), ran},
                     {q("at"), at},
                     {q("key"), key},
                     {q("fromCache"), fromCache},
                     {q("totalMs"), round2(totalMs)},
                     {q("encoders"), enc},
                     {q("d3d11va"), QJsonObject{{q("device"), d3d11vaDevice}, {q("codecs"), QJsonArray::fromStringList(d3d11vaCodecs)}}},
                     {q("compositor"), comp}};
}

SmokeResult SmokeResult::fromJson(const QJsonObject& o) {
  SmokeResult r;
  r.ran = o.value(q("ran")).toBool();
  r.at = o.value(q("at")).toString();
  r.key = o.value(q("key")).toString();
  r.totalMs = o.value(q("totalMs")).toDouble();
  for (const QJsonValue& v : o.value(q("encoders")).toArray()) {
    const QJsonObject e = v.toObject();
    EncoderProbe p;
    p.name = e.value(q("name")).toString();
    p.present = e.value(q("present")).toBool();
    p.works = e.value(q("works")).toBool();
    p.ms = e.value(q("ms")).toDouble();
    p.error = e.value(q("error")).toString();
    r.encoders.push_back(p);
  }
  const QJsonObject d3 = o.value(q("d3d11va")).toObject();
  r.d3d11vaDevice = d3.value(q("device")).toBool();
  for (const QJsonValue& v : d3.value(q("codecs")).toArray()) r.d3d11vaCodecs << v.toString();
  const QJsonObject c = o.value(q("compositor")).toObject();
  r.compositorRan = c.value(q("ran")).toBool();
  r.compositorOk = c.value(q("ok")).toBool();
  r.compositorMs = c.value(q("ms")).toDouble();
  r.compositorError = c.value(q("error")).toString();
  return r;
}

namespace {

QString avError(int r) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(r, buf, sizeof(buf));
  return QString::fromLocal8Bit(buf);
}

// Open the encoder on 256 x 256 NV12 and push `frames` flat frames through it: works when a packet comes out.
EncoderProbe probeEncoder(const char* name, int frames) {
  EncoderProbe p;
  p.name = QString::fromLatin1(name);
  const auto t0 = std::chrono::steady_clock::now();
  const auto done = [&]() {
    p.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return p;
  };
  const AVCodec* enc = avcodec_find_encoder_by_name(name);
  if (!enc) {
    p.error = q("not in this FFmpeg build");
    return done();
  }
  p.present = true;
  AVCodecContext* ctx = avcodec_alloc_context3(enc);
  if (!ctx) {
    p.error = q("out of memory");
    return done();
  }
  ctx->width = 256;
  ctx->height = 256;
  ctx->time_base = {1, 30};
  ctx->framerate = {30, 1};
  ctx->pix_fmt = AV_PIX_FMT_NV12;
  ctx->gop_size = 5;
  ctx->max_b_frames = 0;
  ctx->bit_rate = 2'000'000;
  if (const int r = avcodec_open2(ctx, enc, nullptr); r < 0) {
    p.error = avError(r);
    avcodec_free_context(&ctx);
    return done();
  }
  AVFrame* f = av_frame_alloc();
  AVPacket* pkt = av_packet_alloc();
  f->format = AV_PIX_FMT_NV12;
  f->width = ctx->width;
  f->height = ctx->height;
  int packets = 0;
  bool failed = false;
  if (av_frame_get_buffer(f, 0) < 0) {
    p.error = q("out of memory");
    failed = true;
  }
  const auto drain = [&]() {
    for (;;) {
      const int r = avcodec_receive_packet(ctx, pkt);
      if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return;
      if (r < 0) {
        p.error = avError(r);
        failed = true;
        return;
      }
      ++packets;
      av_packet_unref(pkt);
    }
  };
  for (int i = 0; i < frames && !failed; ++i) {
    av_frame_make_writable(f);
    for (int y = 0; y < f->height; ++y) std::memset(f->data[0] + static_cast<ptrdiff_t>(y) * f->linesize[0], 100 + (i * 7) % 100, static_cast<size_t>(f->width));
    for (int y = 0; y < f->height / 2; ++y) std::memset(f->data[1] + static_cast<ptrdiff_t>(y) * f->linesize[1], 128, static_cast<size_t>(f->width));
    f->pts = i;
    if (const int r = avcodec_send_frame(ctx, f); r < 0) {
      p.error = avError(r);
      failed = true;
      break;
    }
    drain();
  }
  if (!failed) {
    avcodec_send_frame(ctx, nullptr);
    drain();
  }
  p.works = !failed && packets > 0;
  if (!p.works && p.error.isEmpty()) p.error = q("no packet came out");
  av_packet_free(&pkt);
  av_frame_free(&f);
  avcodec_free_context(&ctx);
  return done();
}

bool decoderHasD3d11(AVCodecID id) {
  const AVCodec* dec = avcodec_find_decoder(id);
  if (!dec) return false;
  for (int i = 0;; ++i) {
    const AVCodecHWConfig* cfg = avcodec_get_hw_config(dec, i);
    if (!cfg) return false;
    if (cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA && (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) return true;
  }
}

} // namespace

SmokeResult runSmokeTest(const SmokeOptions& options) {
  SmokeResult r;
  r.ran = true;
  r.at = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  const auto t0 = std::chrono::steady_clock::now();
  const int frames = std::clamp(options.frames, 1, 60);
  // an absent vendor's encoder logs loudly while failing: keep FFmpeg quiet for the duration (restored below)
  const int logLevel = av_log_get_level();
  av_log_set_level(AV_LOG_QUIET);
  // the encoders in parallel: an absent vendor's encoder fails fast, but a driver that loads slowly must not add up
  std::vector<std::future<EncoderProbe>> jobs;
  for (const char* name : {"h264_nvenc", "hevc_nvenc", "h264_amf", "h264_qsv"}) jobs.push_back(std::async(std::launch::async, [name, frames] { return probeEncoder(name, frames); }));

  if (options.compositor) {
    r.compositorRan = true;
    options.compositor(r);
  }

#ifdef _WIN32
  {
    AVBufferRef* dev = nullptr;
    if (av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) >= 0) {
      r.d3d11vaDevice = true;
      av_buffer_unref(&dev);
    }
    if (r.d3d11vaDevice) {
      if (decoderHasD3d11(AV_CODEC_ID_H264)) r.d3d11vaCodecs << q("h264");
      if (decoderHasD3d11(AV_CODEC_ID_HEVC)) r.d3d11vaCodecs << q("hevc");
      if (decoderHasD3d11(AV_CODEC_ID_AV1)) r.d3d11vaCodecs << q("av1");
    }
  }
#endif
  for (auto& j : jobs) r.encoders.push_back(j.get());
  av_log_set_level(logLevel);
  r.totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return r;
}

// ---------------------------------------------------------------- overrides

QStringList Overrides::keys() { return {q("previewScale"), q("hwDecode"), q("exportEncoder"), q("mlDevice"), q("mlThreads"), q("backgroundPriority"), q("cacheMb")}; }

QJsonObject Overrides::toJson() const {
  QJsonObject o;
  if (previewScale) o.insert(q("previewScale"), *previewScale);
  if (hwDecode) o.insert(q("hwDecode"), *hwDecode);
  if (exportEncoder) o.insert(q("exportEncoder"), *exportEncoder);
  if (mlDevice) o.insert(q("mlDevice"), *mlDevice);
  if (mlThreads) o.insert(q("mlThreads"), *mlThreads);
  if (backgroundPriority) o.insert(q("backgroundPriority"), *backgroundPriority);
  if (cacheMb) o.insert(q("cacheMb"), *cacheMb);
  return o;
}

bool Overrides::set(const QString& key, const QJsonValue& value, QString* error) {
  const auto fail = [&](const QString& m) {
    if (error) *error = m;
    return false;
  };
  const bool clear = value.isNull() || value.isUndefined();
  const auto oneOf = [&](const QStringList& allowed) {
    return value.isString() && allowed.contains(value.toString());
  };
  if (key == q("previewScale")) {
    if (clear) return previewScale.reset(), true;
    if (!value.isDouble() || value.toDouble() < 0.25 || value.toDouble() > 1.0) return fail(q("previewScale must be a number from 0.25 to 1."));
    previewScale = value.toDouble();
  } else if (key == q("hwDecode")) {
    if (clear) return hwDecode.reset(), true;
    if (!value.isBool()) return fail(q("hwDecode must be true or false."));
    hwDecode = value.toBool();
  } else if (key == q("exportEncoder")) {
    if (clear) return exportEncoder.reset(), true;
    if (!oneOf({q("software"), q("nvenc"), q("amf"), q("qsv")})) return fail(q("exportEncoder must be software, nvenc, amf or qsv."));
    exportEncoder = value.toString();
  } else if (key == q("mlDevice")) {
    if (clear) return mlDevice.reset(), true;
    if (!oneOf({q("cpu"), q("gpu")})) return fail(q("mlDevice must be cpu or gpu."));
    mlDevice = value.toString();
  } else if (key == q("mlThreads")) {
    if (clear) return mlThreads.reset(), true;
    if (!value.isDouble() || value.toDouble() < 1 || value.toDouble() > 64) return fail(q("mlThreads must be an integer from 1 to 64."));
    mlThreads = static_cast<int>(std::lround(value.toDouble()));
  } else if (key == q("backgroundPriority")) {
    if (clear) return backgroundPriority.reset(), true;
    if (!oneOf({q("idle"), q("below_normal"), q("normal")})) return fail(q("backgroundPriority must be idle, below_normal or normal."));
    backgroundPriority = value.toString();
  } else if (key == q("cacheMb")) {
    if (clear) return cacheMb.reset(), true;
    if (!value.isDouble() || value.toDouble() < 64 || value.toDouble() > 65536) return fail(q("cacheMb must be an integer from 64 to 65536."));
    cacheMb = static_cast<int>(std::lround(value.toDouble()));
  } else {
    return fail(QStringLiteral("Unknown key \"%1\". Keys: %2.").arg(key, keys().join(QStringLiteral(", "))));
  }
  return true;
}

Overrides Overrides::fromJson(const QJsonObject& o) {
  Overrides r;
  for (auto it = o.begin(); it != o.end(); ++it) r.set(it.key(), it.value(), nullptr);
  return r;
}

// ---------------------------------------------------------------- policy

QJsonObject Decisions::toJson() const {
  return QJsonObject{{q("previewScale"), previewScale}, {q("hwDecode"), hwDecode}, {q("exportEncoder"), exportEncoder}, {q("mlDevice"), mlDevice},
                     {q("mlThreads"), mlThreads},       {q("backgroundPriority"), backgroundPriority}, {q("cacheMb"), cacheMb}, {q("reasons"), reasons}};
}

bool Decisions::sameValues(const Decisions& o) const {
  return previewScale == o.previewScale && hwDecode == o.hwDecode && exportEncoder == o.exportEncoder && mlDevice == o.mlDevice && mlThreads == o.mlThreads &&
         backgroundPriority == o.backgroundPriority && cacheMb == o.cacheMb;
}

qint64 estimateOtherVramUse(const GpuAdapter& gpu, const VramBudget& budget) {
  if (!budget.valid || gpu.dedicatedVramBytes <= 0) return 0;
  const qint64 healthy = static_cast<qint64>(static_cast<double>(gpu.dedicatedVramBytes) * 0.9);
  return std::max<qint64>(0, healthy - budget.localBudget);
}

namespace {

QString gb(qint64 bytes) { return QString::number(static_cast<double>(bytes) / static_cast<double>(kGiB), 'f', 1) + QStringLiteral(" GB"); }

} // namespace

Decisions decide(const PolicyInput& in) {
  Decisions d;
  const auto why = [&](const char* key, const QString& text) { d.reasons.insert(q(key), text); };
  const bool smokeRan = in.smoke && in.smoke->ran;
  const bool gpuOk = in.gpu && !in.gpu->software && !(smokeRan && in.smoke->compositorRan && !in.smoke->compositorOk);
  const bool battery = in.live.power.known && (in.live.power.onBattery || in.live.power.batterySaver);
  // no budget reading: assume half of the dedicated memory is free
  const qint64 headroom = !gpuOk ? 0 : in.live.vram.valid ? in.live.vram.localHeadroom() : in.gpu->dedicatedVramBytes / 2;
  const qint64 other = gpuOk ? estimateOtherVramUse(*in.gpu, in.live.vram) : 0;
  const bool otherHeavy = other > 3 * kGiB / 2;

  // preview
  if (!gpuOk) {
    d.previewScale = 0.5;
    why("previewScale", q("No usable GPU: compositing falls back to a slow path, so the preview renders at half size."));
  } else if (headroom < kGiB) {
    d.previewScale = 0.5;
    why("previewScale", QStringLiteral("Only %1 of VRAM headroom: preview at half size to stay clear of the budget.").arg(gb(headroom)));
  } else if (headroom < 2 * kGiB) {
    d.previewScale = 0.75;
    why("previewScale", QStringLiteral("%1 of VRAM headroom: preview at 75 %.").arg(gb(headroom)));
  } else if (battery) {
    d.previewScale = 0.75;
    why("previewScale", q("On battery: preview at 75 % to save power."));
  } else {
    d.previewScale = 1.0;
    why("previewScale", QStringLiteral("%1 of VRAM headroom on AC: full-size preview.").arg(gb(headroom)));
  }

  // decode
  const bool d3d = !smokeRan || in.smoke->d3d11vaDevice;
  d.hwDecode = gpuOk && d3d && headroom >= kGiB / 2;
  why("hwDecode", !gpuOk ? q("No usable GPU.") : !d3d ? q("The smoke test could not create a D3D11VA device.") : headroom < kGiB / 2 ? QStringLiteral("Only %1 of VRAM headroom: decode in software.").arg(gb(headroom))
                                                                                                                                    : q("D3D11VA is available and VRAM has room."));

  // export
  const QString vendor = gpuOk ? in.gpu->vendor() : QString();
  const QString family = vendor == q("nvidia") ? q("nvenc") : vendor == q("amd") ? q("amf") : vendor == q("intel") ? q("qsv") : QString();
  d.exportEncoder = q("software");
  if (!gpuOk) {
    why("exportEncoder", q("No usable GPU: software encode."));
  } else if (family.isEmpty()) {
    why("exportEncoder", QStringLiteral("No hardware encoder known for vendor %1.").arg(vendor));
  } else if (headroom < kGiB) {
    why("exportEncoder", QStringLiteral("VRAM headroom is %1 (under 1 GB): the hardware encoder would compete with the compositor, so software encode.").arg(gb(headroom)));
  } else if (smokeRan && !in.smoke->encoderWorks(QStringLiteral("h264_") + family)) {
    why("exportEncoder", QStringLiteral("The smoke test could not open h264_%1: software encode.").arg(family));
  } else {
    d.exportEncoder = family;
    why("exportEncoder", QStringLiteral("%1 %2 headroom, %3.").arg(gb(headroom), q("of VRAM"), smokeRan ? QStringLiteral("h264_%1 opened in the smoke test").arg(family) : q("hardware encoder assumed to work (no smoke test yet)")));
  }

  // machine learning (footage index, local models)
  if (gpuOk && headroom > 3 * kGiB && !otherHeavy && !battery) {
    d.mlDevice = q("gpu");
    why("mlDevice", QStringLiteral("%1 of VRAM headroom and no other heavy VRAM user: models may use the GPU.").arg(gb(headroom)));
  } else {
    d.mlDevice = q("cpu");
    why("mlDevice", !gpuOk ? q("No usable GPU.")
                    : battery ? q("On battery: models stay on the CPU.")
                    : otherHeavy ? QStringLiteral("Another process holds about %1 of VRAM: models stay on the CPU.").arg(gb(other))
                                 : QStringLiteral("Only %1 of VRAM headroom (needs more than 3 GB): models stay on the CPU.").arg(gb(headroom)));
  }
  const int perfCores = in.cpu.performanceCores > 0 ? in.cpu.performanceCores : std::max(in.cpu.physicalCores, 1);
  d.mlThreads = std::clamp(perfCores / 2, 2, 4);
  if (battery) d.mlThreads = std::max(1, d.mlThreads / 2);
  why("mlThreads", QStringLiteral("%1 performance cores: half of them capped to 2..4%2.").arg(perfCores).arg(battery ? q(", halved on battery") : QString()));

  // background work
  const bool lowRam = in.live.memory.totalPhys > 0 && in.live.memory.availPhys < 2 * kGiB;
  d.backgroundPriority = battery || lowRam ? q("idle") : q("below_normal");
  why("backgroundPriority", battery ? q("On battery: background work runs at idle priority.") : lowRam ? QStringLiteral("Only %1 of RAM free: background work runs at idle priority.").arg(gb(in.live.memory.availPhys))
                                                                                                    : q("Background work yields to the editor."));

  // frame cache
  const qint64 availMb = in.live.memory.availPhys / kMiB;
  d.cacheMb = static_cast<int>(std::clamp<qint64>(availMb / 4, 256, 4096));
  QString cacheWhy = QStringLiteral("A quarter of the %1 MB of free RAM, 256..4096 MB.").arg(availMb);
  if (in.live.memory.commitLimit > 0 && in.live.memory.commitHeadroom() < 2 * kGiB) {
    d.cacheMb = std::min(d.cacheMb, 512);
    cacheWhy = QStringLiteral("Only %1 of commit headroom: cache capped at 512 MB.").arg(gb(in.live.memory.commitHeadroom()));
  }
  why("cacheMb", cacheWhy);

  // the user's word beats the policy
  const Overrides& o = in.overrides;
  const auto over = [&](const char* key) { d.reasons.insert(q(key), q("override (system.overrides.set)")); };
  if (o.previewScale) { d.previewScale = *o.previewScale; over("previewScale"); }
  if (o.hwDecode) { d.hwDecode = *o.hwDecode; over("hwDecode"); }
  if (o.exportEncoder) { d.exportEncoder = *o.exportEncoder; over("exportEncoder"); }
  if (o.mlDevice) { d.mlDevice = *o.mlDevice; over("mlDevice"); }
  if (o.mlThreads) { d.mlThreads = *o.mlThreads; over("mlThreads"); }
  if (o.backgroundPriority) { d.backgroundPriority = *o.backgroundPriority; over("backgroundPriority"); }
  if (o.cacheMb) { d.cacheMb = *o.cacheMb; over("cacheMb"); }
  return d;
}

// ---------------------------------------------------------------- llama.cpp fit advice

QJsonObject FitAdvice::toJson() const {
  return QJsonObject{{q("ngl"), ngl},
                     {q("nCpuMoe"), nCpuMoe},
                     {q("ctx"), ctx},
                     {q("layers"), layers},
                     {q("estimatedVramMiB"), mib(estimatedVramBytes)},
                     {q("fitsFully"), fitsFully},
                     {q("gpuUseful"), gpuUseful},
                     {q("flags"), QStringLiteral("-ngl %1 -c %2%3").arg(ngl).arg(ctx).arg(nCpuMoe > 0 ? QStringLiteral(" --n-cpu-moe %1").arg(nCpuMoe) : QString())},
                     {q("notes"), QJsonArray::fromStringList(notes)}};
}

FitAdvice fitAdvice(const FitRequest& req) {
  FitAdvice a;
  const double size = static_cast<double>(std::max<qint64>(req.sizeBytes, 1));
  int layers = req.layers;
  if (layers <= 0) {
    layers = std::clamp(static_cast<int>(16 + 11 * std::log2(1.0 + size / static_cast<double>(kGiB))), 16, 96);
    a.notes << QStringLiteral("Layer count not given: guessed %1 from the file size.").arg(layers);
  }
  a.layers = layers;
  const double fixedShare = 0.06, expertShare = req.isMoe ? 0.85 : 0.0;
  const double fixed = size * fixedShare, perLayer = size * (1.0 - fixedShare) / layers;
  const double kvPerToken = 4096.0; // bytes per token per layer: 8 KV heads x 128 x (K + V) x f16
  const double reserve = 600.0 * static_cast<double>(kMiB); // compute buffers
  const double budget = static_cast<double>(std::max<qint64>(req.budgetBytes, 0));
  const auto kv = [&](int ctx) { return static_cast<double>(ctx) * kvPerToken; };
  const auto bytesFor = [&](int ngl, int ctx, int moeOnGpuLayers) {
    if (ngl <= 0) return 0.0;
    const double dense = perLayer * (1.0 - expertShare);
    double b = reserve + ngl * (dense + kv(ctx)) + (ngl >= layers ? fixed : 0.0);
    if (req.isMoe) b += perLayer * expertShare * std::min(moeOnGpuLayers, ngl);
    return b;
  };

  std::vector<int> ctxs = req.wantedCtx > 0 ? std::vector<int>{req.wantedCtx} : std::vector<int>{32768, 16384, 8192, 4096};
  a.ctx = req.wantedCtx > 0 ? req.wantedCtx : 8192;
  for (int ctx : ctxs) {
    if (bytesFor(layers, ctx, layers) <= budget) {
      a.ngl = layers;
      a.ctx = ctx;
      a.fitsFully = true;
      a.estimatedVramBytes = static_cast<qint64>(bytesFor(layers, ctx, layers));
      a.gpuUseful = true;
      a.notes << QStringLiteral("Everything fits: all %1 layers, a %2 token context and the compute buffers.").arg(layers).arg(ctx);
      return a;
    }
  }
  if (req.isMoe) {
    // attention and dense tensors of every layer on the GPU, then as many layers' experts as still fit
    const int ctx = a.ctx;
    const double base = bytesFor(layers, ctx, 0);
    if (base <= budget) {
      const int k = std::clamp(static_cast<int>((budget - base) / (perLayer * expertShare)), 0, layers);
      a.ngl = layers;
      a.nCpuMoe = layers - k;
      a.estimatedVramBytes = static_cast<qint64>(bytesFor(layers, ctx, k));
      a.gpuUseful = true;
      a.notes << QStringLiteral("MoE: attention and dense weights of all layers on the GPU, experts of %1 layers on the CPU (--n-cpu-moe %2).").arg(a.nCpuMoe).arg(a.nCpuMoe);
      return a;
    }
    const double each = perLayer * (1.0 - expertShare) + kv(ctx);
    a.ngl = std::clamp(static_cast<int>((budget - reserve) / each), 0, layers);
    a.nCpuMoe = layers; // every offloaded layer keeps its experts on the CPU
    a.estimatedVramBytes = static_cast<qint64>(bytesFor(a.ngl, ctx, 0));
    a.gpuUseful = a.ngl > 0;
    a.notes << QStringLiteral("MoE with little VRAM: %1 of %2 layers' dense weights on the GPU, all experts on the CPU.").arg(a.ngl).arg(layers);
    return a;
  }
  const int ctx = a.ctx;
  const double each = perLayer + kv(ctx);
  a.ngl = std::clamp(static_cast<int>((budget - reserve) / each), 0, layers - 1);
  a.estimatedVramBytes = static_cast<qint64>(bytesFor(a.ngl, ctx, 0));
  a.gpuUseful = a.ngl > 0;
  a.notes << (a.ngl > 0 ? QStringLiteral("Partial offload: %1 of %2 layers on the GPU with a %3 token context.").arg(a.ngl).arg(layers).arg(ctx) : QStringLiteral("Not enough VRAM for even one layer plus the compute buffers: run on the CPU (-ngl 0)."));
  return a;
}

// ---------------------------------------------------------------- the service

SystemService::SystemService(QObject* parent) : QObject(parent) {
  qRegisterMetaType<sf::sys::Decisions>();
  connect(&timer_, &QTimer::timeout, this, &SystemService::pollNow);
}

QString SystemService::dataDir() {
  const QString env = qEnvironmentVariable("SF_SYSTEM_DIR");
  if (!env.isEmpty()) return env;
  return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

const SystemProfile& SystemService::profile() {
  if (!haveProfile_) {
    profile_ = querySystem();
    haveProfile_ = true;
    live_ = profile_.live;
    liveAtMs_ = nowMs();
  }
  return profile_;
}

LiveState SystemService::live(int maxAgeMs) {
  const SystemProfile& p = profile();
  if (nowMs() - liveAtMs_ > maxAgeMs) {
    live_ = queryLive(p.gpu() ? p.gpu()->luid : 0);
    liveAtMs_ = nowMs();
  }
  return live_;
}

SmokeResult SystemService::smoke(bool force, const SmokeOptions& options) {
  const QString key = profile().cacheKey();
  const QString path = QDir(dataDir()).filePath(QStringLiteral("system_profile.json"));
  if (!force) {
    if (smokeLoaded_ && smoke_ && smoke_->key == key) {
      SmokeResult r = *smoke_;
      r.fromCache = true;
      return r;
    }
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
      const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
      SmokeResult r = SmokeResult::fromJson(o.value(q("smoke")).toObject());
      if (o.value(q("key")).toString() == key && r.ran) {
        r.key = key;
        smoke_ = r;
        smokeLoaded_ = true;
        r.fromCache = true;
        return r;
      }
    }
  }
  SmokeResult r = runSmokeTest(options);
  r.key = key;
  smoke_ = r;
  smokeLoaded_ = true;
  if (QDir().mkpath(dataDir())) {
    QSaveFile out(path);
    if (out.open(QIODevice::WriteOnly)) {
      out.write(QJsonDocument(QJsonObject{{q("version"), 1}, {q("key"), key}, {q("smoke"), r.toJson()}}).toJson(QJsonDocument::Indented));
      out.commit();
    }
  }
  return r;
}

void SystemService::loadOverrides() {
  if (overridesLoaded_) return;
  overridesLoaded_ = true;
  QFile f(QDir(dataDir()).filePath(QStringLiteral("system_overrides.json")));
  if (f.open(QIODevice::ReadOnly)) overrides_ = Overrides::fromJson(QJsonDocument::fromJson(f.readAll()).object());
}

void SystemService::saveOverrides() const {
  if (!QDir().mkpath(dataDir())) return;
  QSaveFile out(QDir(dataDir()).filePath(QStringLiteral("system_overrides.json")));
  if (!out.open(QIODevice::WriteOnly)) return;
  out.write(QJsonDocument(overrides_.toJson()).toJson(QJsonDocument::Indented));
  out.commit();
}

bool SystemService::setOverride(const QString& key, const QJsonValue& value, QString* error) {
  loadOverrides();
  if (!overrides_.set(key, value, error)) return false;
  saveOverrides();
  return true;
}

PolicyInput SystemService::input() {
  loadOverrides();
  PolicyInput in;
  const SystemProfile& p = profile();
  in.gpu = p.gpu();
  in.cpu = p.cpu;
  in.live = live(1000);
  if (smoke_) in.smoke = smoke_;
  in.overrides = overrides_;
  return in;
}

Decisions SystemService::decisions() { return decide(input()); }

void SystemService::startMonitoring(int intervalMs) {
  timer_.start(std::max(intervalMs, 250));
  pollNow();
}

void SystemService::stopMonitoring() { timer_.stop(); }

void SystemService::pollNow() {
  const SystemProfile& p = profile();
  live_ = queryLive(p.gpu() ? p.gpu()->luid : 0);
  liveAtMs_ = nowMs();
  const Decisions d = decisions();
  const bool changed = haveLast_ && !d.sameValues(last_);
  last_ = d;
  haveLast_ = true;
  if (changed) emit decisionsChanged(d);
}

SystemService& sharedSystemService() {
  static auto* s = new SystemService; // never destroyed: its timer must not outlive the event loop
  return *s;
}

} // namespace sf::sys
