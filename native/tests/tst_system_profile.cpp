// The system profile: hardware queries sane on this machine (no assertions on exact hardware), the pure resource policy as a table
// (no GPU, a big GPU, little VRAM with an LLM loaded, battery, overrides), llama.cpp fit advice, and the cached smoke test.

#include "sys/system_profile.h"

#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

using namespace sf::sys;

namespace {

GpuAdapter nvidia(qint64 vram) {
  GpuAdapter g;
  g.name = QStringLiteral("Test RTX");
  g.vendorId = 0x10DE;
  g.dedicatedVramBytes = vram;
  g.luid = 0x1234;
  g.driverVersion = QStringLiteral("32.0.15.6094");
  return g;
}

LiveState live(qint64 budget, qint64 usage, bool battery = false, qint64 availRam = 16 * kGiB) {
  LiveState s;
  s.vram.valid = true;
  s.vram.localBudget = budget;
  s.vram.localUsage = usage;
  s.memory.totalPhys = 32 * kGiB;
  s.memory.availPhys = availRam;
  s.memory.commitLimit = 64 * kGiB;
  s.memory.commitTotal = 20 * kGiB;
  s.power.known = true;
  s.power.onBattery = battery;
  return s;
}

SmokeResult smokeWith(bool nvencWorks, bool d3d = true, bool compositor = true) {
  SmokeResult r;
  r.ran = true;
  r.d3d11vaDevice = d3d;
  r.compositorRan = true;
  r.compositorOk = compositor;
  EncoderProbe e;
  e.name = QStringLiteral("h264_nvenc");
  e.present = true;
  e.works = nvencWorks;
  r.encoders.push_back(e);
  return r;
}

CpuInfo cpu(int physical, int perf) {
  CpuInfo c;
  c.name = QStringLiteral("Test CPU");
  c.physicalCores = physical;
  c.logicalCores = physical * 2;
  c.performanceCores = perf;
  c.efficiencyCores = physical - perf;
  c.hybrid = physical != perf;
  return c;
}

PolicyInput input(const GpuAdapter* g, const LiveState& l, std::optional<SmokeResult> smoke = std::nullopt, CpuInfo c = cpu(8, 8)) {
  PolicyInput in;
  in.gpu = g;
  in.cpu = c;
  in.live = l;
  in.smoke = smoke;
  return in;
}

} // namespace

class TstSystemProfile : public QObject {
  Q_OBJECT
  QTemporaryDir dir_;

private slots:
  void initTestCase() {
    QVERIFY(dir_.isValid());
    qputenv("SF_SYSTEM_DIR", dir_.path().toUtf8());
  }

  void profileQuerySane() {
    const SystemProfile p = querySystem();
    QVERIFY(p.cpu.logicalCores >= 1);
    QVERIFY(p.cpu.physicalCores >= 1 && p.cpu.physicalCores <= p.cpu.logicalCores);
    QCOMPARE(p.cpu.performanceCores + p.cpu.efficiencyCores, p.cpu.physicalCores);
    QVERIFY(!p.cpu.name.isEmpty());
    QVERIFY(p.live.memory.totalPhys > 512 * kMiB);
    QVERIFY(p.live.memory.availPhys > 0 && p.live.memory.availPhys <= p.live.memory.totalPhys);
    QVERIFY(p.live.memory.commitLimit >= p.live.memory.totalPhys / 2);
    if (p.gpu()) {
      QVERIFY(!p.gpu()->name.isEmpty());
      QVERIFY(!p.gpu()->software);
      if (p.live.vram.valid) {
        QVERIFY(p.live.vram.localBudget > 0);
        QVERIFY(p.live.vram.localUsage >= 0);
      }
    }
    QVERIFY(p.toJson().contains(QStringLiteral("cacheKey")));
    qInfo().noquote() << QString::fromUtf8(QJsonDocument(p.toJson()).toJson(QJsonDocument::Compact));
  }

  void policyNoGpu() {
    LiveState l;
    l.memory.totalPhys = 16 * kGiB;
    l.memory.availPhys = 8 * kGiB;
    const Decisions d = decide(input(nullptr, l));
    QCOMPARE(d.previewScale, 0.5);
    QVERIFY(!d.hwDecode);
    QCOMPARE(d.exportEncoder, QStringLiteral("software"));
    QCOMPARE(d.mlDevice, QStringLiteral("cpu"));
    QVERIFY(d.mlThreads >= 2 && d.mlThreads <= 4);
    QCOMPARE(d.cacheMb, 2048);
    for (const char* k : {"previewScale", "hwDecode", "exportEncoder", "mlDevice", "mlThreads", "backgroundPriority", "cacheMb"}) QVERIFY2(d.reasons.contains(QLatin1String(k)), k);
  }

  void policyBigGpu() {
    const GpuAdapter g = nvidia(24 * kGiB);
    const Decisions d = decide(input(&g, live(21 * kGiB, 1 * kGiB), smokeWith(true)));
    QCOMPARE(d.previewScale, 1.0);
    QVERIFY(d.hwDecode);
    QCOMPARE(d.exportEncoder, QStringLiteral("nvenc"));
    QCOMPARE(d.mlDevice, QStringLiteral("gpu"));
    QCOMPARE(d.mlThreads, 4);
    QCOMPARE(d.backgroundPriority, QStringLiteral("below_normal"));
    // a smoke test that could not open the encoder sends export to software, the rest stays
    const Decisions bad = decide(input(&g, live(21 * kGiB, 1 * kGiB), smokeWith(false)));
    QCOMPARE(bad.exportEncoder, QStringLiteral("software"));
    QCOMPARE(bad.mlDevice, QStringLiteral("gpu"));
    // a compositor that failed in the smoke test means no usable GPU
    QCOMPARE(decide(input(&g, live(21 * kGiB, 1 * kGiB), smokeWith(true, true, false))).previewScale, 0.5);
  }

  void policyLlmLoadedCollapsesHeadroom() {
    const GpuAdapter g = nvidia(8 * kGiB);
    // idle: ~7 GB budget, we hold 0.5 GB
    const Decisions before = decide(input(&g, live(7 * kGiB, kGiB / 2), smokeWith(true)));
    QCOMPARE(before.mlDevice, QStringLiteral("gpu"));
    QCOMPARE(before.exportEncoder, QStringLiteral("nvenc"));
    QCOMPARE(before.previewScale, 1.0);
    // a 5 GB model loads in another process: the OS shrinks our budget to ~2.2 GB
    const Decisions loaded = decide(input(&g, live(2200 * kMiB, kGiB / 2), smokeWith(true)));
    QCOMPARE(loaded.mlDevice, QStringLiteral("cpu"));
    QCOMPARE(loaded.previewScale, 0.75);
    QVERIFY(loaded.reasons.value(QStringLiteral("mlDevice")).toString().contains(QStringLiteral("Another process")));
    // further down: under 1 GB of headroom, export leaves the GPU and the preview halves
    const Decisions tight = decide(input(&g, live(1400 * kMiB, 700 * kMiB), smokeWith(true)));
    QCOMPARE(tight.exportEncoder, QStringLiteral("software"));
    QCOMPARE(tight.previewScale, 0.5);
    QVERIFY(tight.hwDecode); // 0.7 GB is still over the 0.5 GB decode floor
    const Decisions starved = decide(input(&g, live(900 * kMiB, 600 * kMiB), smokeWith(true)));
    QVERIFY(!starved.hwDecode);
    QVERIFY(!before.sameValues(loaded));
    QVERIFY(loaded.sameValues(loaded));
  }

  void policyBattery() {
    const GpuAdapter g = nvidia(24 * kGiB);
    const Decisions d = decide(input(&g, live(21 * kGiB, kGiB, true), smokeWith(true)));
    QCOMPARE(d.mlDevice, QStringLiteral("cpu"));
    QCOMPARE(d.mlThreads, 2); // 4 halved
    QCOMPARE(d.backgroundPriority, QStringLiteral("idle"));
    QCOMPARE(d.previewScale, 0.75);
    // two performance cores: 2, halved to 1
    QCOMPARE(decide(input(&g, live(21 * kGiB, kGiB, true), smokeWith(true), cpu(4, 2))).mlThreads, 1);
    QCOMPARE(decide(input(&g, live(21 * kGiB, kGiB, false), smokeWith(true), cpu(4, 2))).mlThreads, 2);
  }

  void policyLowRamAndCommit() {
    const GpuAdapter g = nvidia(24 * kGiB);
    LiveState l = live(21 * kGiB, kGiB, false, kGiB);
    QCOMPARE(decide(input(&g, l, smokeWith(true))).backgroundPriority, QStringLiteral("idle"));
    QCOMPARE(decide(input(&g, l, smokeWith(true))).cacheMb, 256);
    l = live(21 * kGiB, kGiB);
    l.memory.commitTotal = l.memory.commitLimit - kGiB;
    QCOMPARE(decide(input(&g, l, smokeWith(true))).cacheMb, 512);
  }

  void overridesWin() {
    const GpuAdapter g = nvidia(24 * kGiB);
    PolicyInput in = input(&g, live(21 * kGiB, kGiB), smokeWith(true));
    QString err;
    QVERIFY(in.overrides.set(QStringLiteral("mlDevice"), QStringLiteral("cpu"), &err));
    QVERIFY(in.overrides.set(QStringLiteral("previewScale"), 0.5, &err));
    QVERIFY(in.overrides.set(QStringLiteral("exportEncoder"), QStringLiteral("software"), &err));
    const Decisions d = decide(in);
    QCOMPARE(d.mlDevice, QStringLiteral("cpu"));
    QCOMPARE(d.previewScale, 0.5);
    QCOMPARE(d.exportEncoder, QStringLiteral("software"));
    QVERIFY(d.reasons.value(QStringLiteral("mlDevice")).toString().contains(QStringLiteral("override")));
    QVERIFY(in.overrides.set(QStringLiteral("mlDevice"), QJsonValue(QJsonValue::Null), &err)); // cleared
    QCOMPARE(decide(in).mlDevice, QStringLiteral("gpu"));
    QVERIFY(!in.overrides.set(QStringLiteral("previewScale"), 3.0, &err));
    QVERIFY(!in.overrides.set(QStringLiteral("exportEncoder"), QStringLiteral("quicksync"), &err));
    QVERIFY(!in.overrides.set(QStringLiteral("nope"), 1, &err));
    QVERIFY(err.contains(QStringLiteral("Keys")));
    QCOMPARE(Overrides::fromJson(in.overrides.toJson()).toJson(), in.overrides.toJson());
  }

  void fitAdviceTable() {
    FitRequest r;
    r.sizeBytes = 4 * kGiB;
    r.layers = 32;
    r.budgetBytes = 12 * kGiB;
    FitAdvice a = fitAdvice(r);
    QVERIFY(a.fitsFully);
    QCOMPARE(a.ngl, 32);
    QCOMPARE(a.ctx, 32768);
    QVERIFY(a.estimatedVramBytes <= r.budgetBytes);
    r.budgetBytes = 6 * kGiB; // the model fits, a long context does not
    a = fitAdvice(r);
    QVERIFY(a.fitsFully && a.ctx < 32768 && a.ctx >= 4096);
    r.budgetBytes = 3 * kGiB; // partial offload
    a = fitAdvice(r);
    QVERIFY(!a.fitsFully);
    QVERIFY(a.ngl > 0 && a.ngl < 32);
    QVERIFY(a.estimatedVramBytes <= r.budgetBytes);
    r.budgetBytes = 400 * kMiB;
    a = fitAdvice(r);
    QCOMPARE(a.ngl, 0);
    QVERIFY(!a.gpuUseful);
    // MoE: experts of the first layers stay on the CPU
    FitRequest m;
    m.sizeBytes = 18 * kGiB;
    m.layers = 48;
    m.isMoe = true;
    m.budgetBytes = 10 * kGiB;
    const FitAdvice ma = fitAdvice(m);
    QVERIFY(!ma.fitsFully);
    QCOMPARE(ma.ngl, 48);
    QVERIFY(ma.nCpuMoe > 0 && ma.nCpuMoe < 48);
    QVERIFY(ma.estimatedVramBytes <= m.budgetBytes);
    QVERIFY(ma.toJson().value(QStringLiteral("flags")).toString().contains(QStringLiteral("--n-cpu-moe")));
    m.budgetBytes = 40 * kGiB;
    QVERIFY(fitAdvice(m).fitsFully);
    // layers guessed from the size
    FitRequest g;
    g.sizeBytes = 8 * kGiB;
    g.budgetBytes = 2 * kGiB;
    QVERIFY(fitAdvice(g).layers >= 16);
  }

  void smokeRunsFastAndCaches() {
    SystemService s;
    const SmokeResult r = s.smoke(true);
    QVERIFY(r.ran);
    QCOMPARE(r.encoders.size(), size_t(4));
    for (const EncoderProbe& e : r.encoders) QVERIFY(!e.name.isEmpty());
    QVERIFY2(r.totalMs < 3000.0, qPrintable(QString::number(r.totalMs)));
    qInfo().noquote() << QString::fromUtf8(QJsonDocument(r.toJson()).toJson(QJsonDocument::Compact));
    // the cache answers without running again, also from a fresh service
    SystemService again;
    const SmokeResult c = again.smoke(false);
    QVERIFY(c.fromCache);
    QCOMPARE(c.at, r.at);
    QCOMPARE(c.encoders.size(), r.encoders.size());
    QCOMPARE(c.d3d11vaDevice, r.d3d11vaDevice);
    for (size_t i = 0; i < r.encoders.size(); ++i) QCOMPARE(c.encoders[i].works, r.encoders[i].works);
    QVERIFY(!again.smoke(true).fromCache);
    QCOMPARE(SmokeResult::fromJson(r.toJson()).toJson(), r.toJson());
  }

  void serviceOverridesPersistAndLiveUpdates() {
    {
      SystemService s;
      QString err;
      QVERIFY(s.setOverride(QStringLiteral("cacheMb"), 777, &err));
      QCOMPARE(s.decisions().cacheMb, 777);
    }
    SystemService s2;
    QCOMPARE(s2.decisions().cacheMb, 777);
    QString err;
    QVERIFY(s2.setOverride(QStringLiteral("cacheMb"), QJsonValue(QJsonValue::Null), &err));
    QVERIFY(s2.decisions().cacheMb != 777);
    s2.pollNow(); // one monitor step works and stays quiet when nothing moved
    s2.pollNow();
  }
};

QTEST_GUILESS_MAIN(TstSystemProfile)
#include "tst_system_profile.moc"
