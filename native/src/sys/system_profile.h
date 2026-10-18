#pragma once

// What this machine is and what it can spare right now, and the resource decisions drawn from it.
//
//   querySystem()    static hardware (GPU adapters through DXGI, CPU topology, RAM, power) plus a first live sample
//   queryLive()      the cheap part only, meant to be repeated every few seconds: VRAM budget / usage, RAM, power
//   runSmokeTest()   < 3 s, tiny: which FFmpeg hardware encoders really open, whether D3D11VA exists, optionally one
//                    1080p compositor frame (the caller supplies it: this library does not link the renderer)
//   decide()         PURE: profile + live budget + smoke result + overrides -> decisions with a reason each
//   SystemService    the process-wide owner: caches the smoke test (JSON, keyed by adapter LUID + driver + CPU name),
//                    persists overrides, re-queries the live budget on a timer and re-decides when it moves
//   fitAdvice()      PURE: suggested llama.cpp -ngl / --n-cpu-moe / context for a model size and a VRAM budget
//
// VRAM headroom = the OS budget of the adapter minus what this process already uses (DXGI QueryVideoMemoryInfo).
// The budget shrinks when another process (an LLM server, a game) takes video memory, which is how a collapse shows.

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <optional>
#include <vector>

namespace sf::sys {

inline constexpr qint64 kMiB = 1024ll * 1024ll;
inline constexpr qint64 kGiB = 1024ll * kMiB;

struct GpuAdapter {
  QString name;
  quint32 vendorId = 0, deviceId = 0;
  QString driverVersion; // "a.b.c.d" as DXGI reports it (product.version.subversion.build)
  qint64 dedicatedVramBytes = 0, dedicatedSystemBytes = 0, sharedSystemBytes = 0;
  quint64 luid = 0; // high part << 32 | low part
  bool software = false; // Microsoft Basic Render Driver / WARP
  QString vendor() const; // nvidia | amd | intel | microsoft | other
  QJsonObject toJson() const;
};

struct VramBudget {
  bool valid = false;
  qint64 localBudget = 0, localUsage = 0, nonLocalBudget = 0, nonLocalUsage = 0; // usage = this process
  qint64 localHeadroom() const { return valid ? localBudget - localUsage : 0; }
  QJsonObject toJson() const;
};

struct CpuInfo {
  QString name;
  int physicalCores = 0, logicalCores = 0;
  int performanceCores = 0, efficiencyCores = 0; // EfficiencyClass: the top class is "P", the rest "E"; all P when not hybrid
  bool hybrid = false;
  QJsonObject toJson() const;
};

struct MemoryInfo {
  qint64 totalPhys = 0, availPhys = 0, commitLimit = 0, commitTotal = 0; // commitTotal = bytes committed system-wide
  qint64 commitHeadroom() const { return commitLimit - commitTotal; }
  QJsonObject toJson() const;
};

struct PowerInfo {
  bool known = false;
  bool onBattery = false;
  bool batterySaver = false;
  int batteryPercent = -1; // -1 unknown / no battery
  QJsonObject toJson() const;
};

// The part that changes while the app runs.
struct LiveState {
  VramBudget vram;
  MemoryInfo memory;
  PowerInfo power;
  QJsonObject toJson() const;
};

struct SystemProfile {
  std::vector<GpuAdapter> gpus;
  int primaryGpu = -1; // index in gpus of the adapter the renderer uses (the high-performance one), -1 when none
  CpuInfo cpu;
  LiveState live;
  const GpuAdapter* gpu() const { return primaryGpu >= 0 && primaryGpu < static_cast<int>(gpus.size()) ? &gpus[static_cast<size_t>(primaryGpu)] : nullptr; }
  QString cacheKey() const; // primary adapter LUID + driver version + CPU name
  QJsonObject toJson() const;
};

SystemProfile querySystem();
// Live state of the primary adapter (`luid` from SystemProfile::gpu(); 0 = none) plus RAM and power.
LiveState queryLive(quint64 luid);

// ---------------------------------------------------------------- smoke test

struct EncoderProbe {
  QString name;      // h264_nvenc ...
  bool present = false; // compiled into this FFmpeg
  bool works = false;   // opened and produced a packet
  double ms = 0;
  QString error;
  QJsonObject toJson() const;
};

struct SmokeResult {
  bool ran = false;
  QString at;        // ISO time
  QString key;       // what it was measured for (SystemProfile::cacheKey)
  double totalMs = 0;
  std::vector<EncoderProbe> encoders;
  bool d3d11vaDevice = false;
  QStringList d3d11vaCodecs; // decoders FFmpeg can run through D3D11VA: h264, hevc, av1
  bool compositorRan = false, compositorOk = false;
  double compositorMs = 0;
  QString compositorError;
  bool fromCache = false;
  bool encoderWorks(const QString& name) const;
  QJsonObject toJson() const;
  static SmokeResult fromJson(const QJsonObject& o);
};

struct SmokeOptions {
  int frames = 10;
  // Renders one 1080p frame and fills compositorOk / compositorMs / compositorError (the engine supplies it); optional.
  std::function<void(SmokeResult&)> compositor;
};
SmokeResult runSmokeTest(const SmokeOptions& options = {});

// ---------------------------------------------------------------- policy

struct Overrides {
  std::optional<double> previewScale;
  std::optional<bool> hwDecode;
  std::optional<QString> exportEncoder;       // software | nvenc | amf | qsv
  std::optional<QString> mlDevice;            // cpu | gpu
  std::optional<int> mlThreads;
  std::optional<QString> backgroundPriority;  // idle | below_normal | normal
  std::optional<int> cacheMb;
  QJsonObject toJson() const;
  static Overrides fromJson(const QJsonObject& o);
  // Sets (or with a null value clears) one key; false and *error when the key is unknown or the value is out of range.
  bool set(const QString& key, const QJsonValue& value, QString* error);
  static QStringList keys();
};

struct PolicyInput {
  const GpuAdapter* gpu = nullptr; // null: no usable GPU
  CpuInfo cpu;
  LiveState live;
  std::optional<SmokeResult> smoke; // nullopt: never measured, hardware is assumed to work
  Overrides overrides;
};

struct Decisions {
  double previewScale = 1.0;              // 1, 0.75, 0.5: the preview renders at this fraction of the canvas
  bool hwDecode = false;                  // D3D11VA decoding
  QString exportEncoder = QStringLiteral("software"); // software | nvenc | amf | qsv
  QString mlDevice = QStringLiteral("cpu");           // cpu | gpu: where the footage index / local models run
  int mlThreads = 2;
  QString backgroundPriority = QStringLiteral("below_normal"); // idle | below_normal | normal
  int cacheMb = 512;                      // decoded-frame cache
  QJsonObject reasons;                    // key -> why
  QJsonObject toJson() const;
  bool sameValues(const Decisions& o) const;
};

// VRAM other processes appear to hold: what an idle system grants as budget (90 % of the dedicated memory) minus the budget it grants now.
qint64 estimateOtherVramUse(const GpuAdapter& gpu, const VramBudget& budget);

Decisions decide(const PolicyInput& input);

// ---------------------------------------------------------------- llama.cpp fit advice

struct FitRequest {
  qint64 sizeBytes = 0;   // GGUF file size
  int layers = 0;         // 0: guessed from the size
  bool isMoe = false;
  qint64 budgetBytes = 0; // VRAM the model may use (headroom)
  int wantedCtx = 0;      // 0: largest of 32768..4096 that fits fully
};
struct FitAdvice {
  int ngl = 0;          // -ngl: layers on the GPU
  int nCpuMoe = 0;      // --n-cpu-moe: expert tensors of this many layers stay on the CPU (MoE only)
  int ctx = 4096;
  int layers = 0;
  qint64 estimatedVramBytes = 0;
  bool fitsFully = false; // every layer, the KV cache and the compute buffers fit
  bool gpuUseful = false; // at least one layer (or the dense part of an MoE) can live on the GPU
  QStringList notes;
  QJsonObject toJson() const;
};
FitAdvice fitAdvice(const FitRequest& request);

// ---------------------------------------------------------------- the process-wide service

class SystemService : public QObject {
  Q_OBJECT
public:
  explicit SystemService(QObject* parent = nullptr);

  // The directory of the cache and the overrides: $SF_SYSTEM_DIR when set, else AppConfigLocation.
  static QString dataDir();

  // Static hardware, queried once and kept (live fields are refreshed by `live()`).
  const SystemProfile& profile();
  // Cached smoke test when its key still matches, else (or when `force`) runs it and writes the cache.
  SmokeResult smoke(bool force, const SmokeOptions& options = {});
  // Latest live state (re-queried when older than maxAgeMs; the timer keeps it fresh once started).
  LiveState live(int maxAgeMs = 1000);
  PolicyInput input();
  Decisions decisions();

  const Overrides& overrides() const { return overrides_; }
  bool setOverride(const QString& key, const QJsonValue& value, QString* error); // persists

  // Re-query the live budget every `intervalMs` (default 3 s) and emit decisionsChanged when a decision moves.
  void startMonitoring(int intervalMs = 3000);
  void stopMonitoring();
  void pollNow(); // one monitor step (the timer calls this; tests too)

signals:
  void decisionsChanged(const sf::sys::Decisions& decisions);

private:
  void loadOverrides();
  void saveOverrides() const;
  SystemProfile profile_;
  bool haveProfile_ = false;
  std::optional<SmokeResult> smoke_;
  bool smokeLoaded_ = false;
  LiveState live_;
  qint64 liveAtMs_ = 0;
  Overrides overrides_;
  bool overridesLoaded_ = false;
  Decisions last_;
  bool haveLast_ = false;
  QTimer timer_;
};

// Process-wide owner shared across subsystems
SystemService& sharedSystemService();

} // namespace sf::sys

Q_DECLARE_METATYPE(sf::sys::Decisions)
