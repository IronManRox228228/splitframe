#include "jobs/builtin_analyzers.h"

#include "jobs/hands_tracker.h"
#include "media/waveform.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>

namespace sf::jobs {

namespace {

QString q(const char* s) { return QString::fromLatin1(s); }
double round3(double v) { return std::round(v * 1000.0) / 1000.0; }

class WaveformAnalyzer final : public Analyzer {
public:
  QString id() const override { return q("waveform"); }
  int version() const override { return 1; }
  QString description() const override { return q("Min / max audio peaks of a file reduced to `buckets` values in -1..1, plus the overall peak."); }
  QJsonObject defaultParams() const override { return {{q("buckets"), 200}}; }
  QJsonObject normalizeParams(const QJsonObject& p) const override { return {{q("buckets"), std::clamp(p.value(q("buckets")).toInt(200), 8, 4000)}}; }

  AnalyzerResult run(JobContext& ctx) override {
    QString err;
    const WaveformPeaks peaks = extractWaveformPeaks(ctx.path, 480, [&ctx] { return ctx.stop(); }, &err);
    if (ctx.stop()) return AnalyzerResult::failure(q("cancelled"));
    if (peaks.bucketCount() == 0) return AnalyzerResult::failure(err.isEmpty() ? QStringLiteral("No audio in %1.").arg(ctx.path) : err);
    const int want = ctx.params.value(q("buckets")).toInt(200);
    const int factor = std::max<int>(1, static_cast<int>((peaks.bucketCount() + want - 1) / want));
    const WaveformPeaks r = factor > 1 ? peaks.reduced(factor) : peaks;
    QJsonArray mins, maxs;
    double peak = 0;
    for (qint64 i = 0; i < r.bucketCount(); ++i) {
      mins.append(round3(static_cast<double>(r.minAt(i))));
      maxs.append(round3(static_cast<double>(r.maxAt(i))));
      peak = std::max({peak, std::abs(static_cast<double>(r.minAt(i))), std::abs(static_cast<double>(r.maxAt(i)))});
    }
    ctx.report(1.0);
    return AnalyzerResult::success(QJsonObject{{q("sampleRate"), r.sampleRate},
                                               {q("totalSamples"), static_cast<double>(r.totalSamples)},
                                               {q("durationSec"), round3(static_cast<double>(r.totalSamples) / r.sampleRate)},
                                               {q("buckets"), static_cast<double>(r.bucketCount())},
                                               {q("peak"), round3(peak)},
                                               {q("min"), mins},
                                               {q("max"), maxs}});
  }
};

} // namespace

std::shared_ptr<Analyzer> makeWaveformAnalyzer() { return std::make_shared<WaveformAnalyzer>(); }

void registerBuiltinAnalyzers(JobScheduler& scheduler) {
  scheduler.registerAnalyzer(makeHandsAnalyzer());
  scheduler.registerAnalyzer(makeWaveformAnalyzer());
}

} // namespace sf::jobs
