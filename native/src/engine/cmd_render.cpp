// Render commands: frame.render, audio.analyze, waveform.get, thumbnail.get.

#include "audio/engine.h"
#include "audio/loudness.h"
#include "audio/source.h"
#include "audio/timebase.h"
#include "engine/cmd_util.h"
#include "media/frame_service.h"
#include "media/probe.h"
#include "media/waveform.h"
#include "render/media_provider.h"

#include <QFileInfo>

#include <cmath>

namespace sf::engine {

namespace {

QJsonValue finiteOrNull(double v) { return std::isfinite(v) ? QJsonValue(std::round(v * 100.0) / 100.0) : QJsonValue(QJsonValue::Null); }

Result renderFrame(CallContext& c, const QJsonObject& a) {
  const TimelineDoc& doc = c.session->project->doc();
  Frame frame = 0;
  if (a.contains(q("frame"))) frame = a.value(q("frame")).toInteger();
  else if (a.contains(q("timeSec"))) frame = toFrames(a.value(q("timeSec")).toDouble(), doc);
  if (frame < 0) return invalid(QStringLiteral("frame must be >= 0."));

  QString err;
  render::OffscreenRenderer* off = c.engine->renderer(&err);
  if (!off) return Result::failure(q("unavailable"), QStringLiteral("The renderer is not available: %1").arg(err));
  off->compositor().setDelivery(std::nullopt);

  FrameService::Options o;
  o.cacheBytes = 128ll << 20;
  o.workerThreads = 2;
  o.gpuFrames = false;
  o.hw = HwMode::Off; // software decode: the GPU is shared with other work
  FrameService service(o);
  render::FrameServiceProvider provider(service, assetTable(*c.session), render::FrameServiceProvider::Mode::Blocking);
  provider.openAll();
  provider.prepare(doc, frame, 0);
  render::RenderStats stats;
  const QImage img = off->render(doc, frame, provider, &stats);
  if (img.isNull()) return Result::failure(q("unavailable"), QStringLiteral("The renderer returned no picture for frame %1.").arg(frame));
  return imageResult(img, imageOutFrom(a), QStringLiteral("frame_%1_%2").arg(c.session->id).arg(frame),
                     {{q("frame"), static_cast<double>(frame)}, {q("timeSec"), framesToSeconds(frame, static_cast<double>(doc.project.fps))},
                      {q("canvasWidth"), static_cast<double>(doc.project.width)}, {q("canvasHeight"), static_cast<double>(doc.project.height)},
                      {q("missingLayers"), static_cast<double>(stats.missing)}});
}

Result analyzeAudio(CallContext& c, const QJsonObject& a) {
  const TimelineDoc& doc = c.session->project->doc();
  const std::int64_t fps = doc.project.fps;
  const Frame in = a.value(q("startFrame")).toInteger(0);
  const Frame out = a.contains(q("endFrame")) ? a.value(q("endFrame")).toInteger() : docDurationFrames(doc);
  if (out <= in) return invalid(QStringLiteral("The range is empty (startFrame %1, endFrame %2).").arg(in).arg(out));

  auto sources = std::make_shared<audio::FileProvider>();
  std::map<QString, audio::FileProvider::File> files;
  for (const auto& [id, ref] : assetTable(*c.session)) files[id] = {ref.path, ref.kind != AssetKind::Image};
  sources->setFiles(std::move(files));
  audio::AudioEngine engine(std::make_shared<const TimelineDoc>(doc), sources);
  const std::int64_t base = audio::frameToSample(in, fps);
  const std::int64_t total = audio::frameToSample(out, fps) - base;
  audio::LoudnessMeter meter;
  constexpr std::int64_t kChunk = 48000;
  std::vector<float> buf(static_cast<size_t>(kChunk) * 2);
  for (std::int64_t done = 0; done < total; done += kChunk) {
    const std::int64_t n = std::min(kChunk, total - done);
    engine.render(base + done, n, buf.data());
    meter.process(buf.data(), n);
  }
  audio::LoudnessResult r;
  r.integrated = meter.integrated();
  r.range = meter.loudnessRange();
  r.truePeakDb = meter.truePeakDb();
  r.samplePeakDb = meter.samplePeakDb();
  QJsonObject o{{q("startFrame"), static_cast<double>(in)}, {q("endFrame"), static_cast<double>(out)}, {q("seconds"), static_cast<double>(total) / 48000.0},
                {q("integratedLufs"), finiteOrNull(r.integrated)}, {q("loudnessRangeLu"), finiteOrNull(r.range)}, {q("truePeakDbtp"), finiteOrNull(r.truePeakDb)},
                {q("samplePeakDb"), finiteOrNull(r.samplePeakDb)}, {q("silent"), !std::isfinite(r.integrated)}};
  if (a.contains(q("targetLufs"))) {
    std::optional<double> ceiling;
    if (a.contains(q("truePeakCeilingDb"))) ceiling = a.value(q("truePeakCeilingDb")).toDouble();
    o.insert(q("suggestedGainDb"), finiteOrNull(audio::normalizeGainDb(r, a.value(q("targetLufs")).toDouble(), ceiling)));
  }
  return Result::success(o);
}

Result waveformGet(CallContext& c, const QJsonObject& a) {
  const Asset* as = c.session->project->asset(a.value(q("assetId")).toString());
  if (!as) return notFound(QStringLiteral("Asset %1 not found.").arg(a.value(q("assetId")).toString()), QStringLiteral("Call assets.list."));
  if (as->kind == AssetKind::Image) return invalid(QStringLiteral("Asset %1 is an image: no audio.").arg(as->id));
  WaveformPeaks peaks;
  if (const auto pv = c.session->pool->preview(as->id); pv && pv->peaks.bucketCount() > 0) {
    peaks = pv->peaks;
  } else {
    QString err;
    peaks = extractWaveformPeaks(as->path, 480, {}, &err);
    if (peaks.bucketCount() == 0) return Result::failure(q("unavailable"), err.isEmpty() ? QStringLiteral("No audio in %1.").arg(as->path) : err);
  }
  const int want = a.value(q("buckets")).toInt(200);
  const int factor = std::max<int>(1, static_cast<int>((peaks.bucketCount() + want - 1) / want));
  const WaveformPeaks r = factor > 1 ? peaks.reduced(factor) : peaks;
  QJsonArray mins, maxs;
  for (qint64 i = 0; i < r.bucketCount(); ++i) {
    mins.append(std::round(static_cast<double>(r.minAt(i)) * 1000.0) / 1000.0);
    maxs.append(std::round(static_cast<double>(r.maxAt(i)) * 1000.0) / 1000.0);
  }
  return Result::success(QJsonObject{{q("assetId"), as->id}, {q("durationMs"), static_cast<double>(as->durationMs)}, {q("buckets"), static_cast<double>(r.bucketCount())},
                                     {q("samplesPerBucket"), r.samplesPerPeak}, {q("min"), mins}, {q("max"), maxs}});
}

Result thumbnailGet(CallContext& c, const QJsonObject& a) {
  const Asset* as = c.session->project->asset(a.value(q("assetId")).toString());
  if (!as) return notFound(QStringLiteral("Asset %1 not found.").arg(a.value(q("assetId")).toString()), QStringLiteral("Call assets.list."));
  if (as->kind == AssetKind::Audio) return invalid(QStringLiteral("Asset %1 is audio: use waveform.get.").arg(as->id));
  QString err;
  const QImage img = decodeFrame(as->path, static_cast<qint64>(a.value(q("atSec")).toDouble(0) * 1000.0), &err);
  if (img.isNull()) return Result::failure(q("unavailable"), err.isEmpty() ? QStringLiteral("Can't decode %1.").arg(as->path) : err);
  ImageOut out = imageOutFrom(a);
  if (!a.contains(q("maxWidth"))) out.maxWidth = 320;
  if (!a.contains(q("maxHeight"))) out.maxHeight = 320;
  return imageResult(img, out, QStringLiteral("thumb_%1").arg(as->id), {{q("assetId"), as->id}, {q("atSec"), a.value(q("atSec")).toDouble(0)}});
}

} // namespace

void registerRenderCommands(Engine& e) {
  {
    QJsonObject schema = js::obj({{q("frame"), js::integer(QStringLiteral("Timeline frame (default 0)"), 0)}, {q("timeSec"), js::number(QStringLiteral("Alternative to frame: seconds"), 0)}});
    addImageOutProps(schema);
    e.add(spec("frame.render", "captureFrame", "engine", false,
               QStringLiteral("Render one timeline frame exactly as export would (software decode, offscreen compositor) to a PNG: `path` (a temp file unless you pass path) and optionally base64 "
                              "(inline=true). Downscaled to maxWidth/maxHeight (default 1280)."),
               schema, renderFrame));
  }
  e.add(spec("audio.analyze", "", "audio", false,
             QStringLiteral("Mix the timeline audio (mixer, effects and fades included) and measure it per ITU-R BS.1770 / EBU R128: integrated LUFS, loudness range, true peak, sample peak. "
                            "With targetLufs also the static gain that would hit it. null values mean silence."),
             js::obj({{q("startFrame"), js::integer(QString(), 0)}, {q("endFrame"), js::integer(QString(), 0)}, {q("targetLufs"), js::number(QStringLiteral("e.g. -14 streaming, -16 podcast, -23 EBU R128"), -70, 0)},
                      {q("truePeakCeilingDb"), js::number(QStringLiteral("dBTP ceiling used for the suggested gain"), -20, 3)}}),
             analyzeAudio));
  e.add(spec("waveform.get", "", "audio", false, QStringLiteral("Min/max waveform peaks of an asset's audio, reduced to about `buckets` (default 200) values in -1..1."),
             js::obj({{q("assetId"), js::str()}, {q("buckets"), js::integer(QString(), 8, 4000)}}, {q("assetId")}), waveformGet));
  {
    QJsonObject schema = js::obj({{q("assetId"), js::str()}, {q("atSec"), js::number(QStringLiteral("Position in the source, seconds (default 0)"), 0)}}, {q("assetId")});
    addImageOutProps(schema);
    e.add(spec("thumbnail.get", "", "engine", false, QStringLiteral("A still from an asset (video frame at atSec, or the image) as PNG, default at most 320 px."), schema, thumbnailGet));
  }
}

} // namespace sf::engine
