// Export commands: export.start (async job) / status / cancel, presets.list.

#include "engine/cmd_util.h"
#include "export/export_cli.h"
#include "export/export_settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTimer>

namespace sf::engine {

namespace {

QJsonObject recordJson(const ExportRecord& r) {
  QJsonObject o{{q("jobId"), r.id}, {q("session"), r.sessionId}, {q("state"), r.state}, {q("outputPath"), QDir::toNativeSeparators(r.outputPath)}, {q("frame"), static_cast<double>(r.frame)},
                {q("totalFrames"), static_cast<double>(r.totalFrames)}, {q("fraction"), r.fraction}, {q("fps"), r.fps}, {q("etaSec"), r.etaSec}, {q("stage"), r.stage}};
  if (!r.error.isEmpty()) o.insert(q("error"), r.error);
  if (!r.result.isEmpty()) o.insert(q("result"), r.result);
  if (!r.warnings.isEmpty()) o.insert(q("warnings"), r.warnings);
  return o;
}

QJsonObject planJson(const xport::ExportPlan& p) {
  return QJsonObject{{q("container"), xport::containerName(p.container)}, {q("video"), p.video == xport::VideoCodec::None ? QStringLiteral("none") : p.videoEncoder},
                     {q("width"), p.width}, {q("height"), p.height}, {q("fps"), p.fps}, {q("frames"), static_cast<double>(p.frames)}, {q("audioSamples"), static_cast<double>(p.audioSamples)},
                     {q("bits"), p.bits}, {q("hdr"), p.hdr}, {q("warnings"), QJsonArray::fromStringList(p.warnings)}};
}

Result exportStart(CallContext& c, const QJsonObject& a) {
  Engine& eng = *c.engine;
  Session& s = *c.session;
  const TimelineDoc& doc = s.project->doc();
  QJsonObject opts = a;
  opts.remove(q("session"));
  opts.remove(q("origin"));
  bool autoOut = false;
  if (!opts.contains(q("out"))) {
    const QString dir = s.project->path().isEmpty() ? QDir::temp().filePath(QStringLiteral("splitframe-exports")) : QFileInfo(s.project->path()).absolutePath();
    QDir().mkpath(dir);
    const QString preset = opts.value(q("preset")).toString();
    const bool audioOnly = opts.value(q("audioOnly")).toBool();
    opts.insert(q("out"), QDir(dir).filePath(QStringLiteral("%1%2.%3").arg(doc.project.name, preset.isEmpty() ? QString() : QStringLiteral("-") + preset, audioOnly ? QStringLiteral("wav") : QStringLiteral("mp4"))));
    opts.insert(q("overwrite"), true);
    autoOut = true;
  }
  xport::ExportSettings settings;
  QString err;
  if (!xport::exportSettingsFromJson(opts, doc, &settings, &err)) return invalid(err);
  const auto plan = xport::planExport(doc, settings, &err);
  if (!plan) return invalid(err);
  if (QFileInfo::exists(settings.outputPath) && !settings.overwrite)
    return Result::failure(q("invalid_params"), QStringLiteral("%1 exists: pass overwrite=true or another `out`.").arg(settings.outputPath));
  for (const auto& [id, rec] : eng.exports())
    if (rec.state == QLatin1String("running") && QFileInfo(rec.outputPath) == QFileInfo(settings.outputPath))
      return Result::failure(q("busy"), QStringLiteral("Export %1 is already writing %2.").arg(id, settings.outputPath));

  const QString id = newId(q("exp"));
  ExportRecord& rec = eng.exports()[id];
  rec.id = id;
  rec.sessionId = s.id;
  rec.outputPath = QFileInfo(settings.outputPath).absoluteFilePath();
  rec.totalFrames = plan->frames;
  rec.stage = QStringLiteral("queued");
  rec.job = std::make_unique<xport::ExportJob>(doc, assetTable(s), settings, &eng);
  Engine* engine = &eng;
  QObject::connect(rec.job.get(), &xport::ExportJob::progress, engine, [engine, id](const xport::ExportProgress& p) {
    auto it = engine->exports().find(id);
    if (it == engine->exports().end()) return;
    ExportRecord& r = it->second;
    r.frame = p.frame;
    r.totalFrames = p.totalFrames;
    r.fraction = p.fraction;
    r.fps = p.fps;
    r.etaSec = p.etaSec;
    r.stage = p.stage;
    engine->emitEvent(q("export.progress"), {{q("session"), r.sessionId}, {q("jobId"), id}, {q("frame"), static_cast<double>(p.frame)}, {q("totalFrames"), static_cast<double>(p.totalFrames)},
                                             {q("fraction"), p.fraction}, {q("fps"), p.fps}, {q("etaSec"), p.etaSec}, {q("stage"), p.stage}});
  });
  QObject::connect(rec.job.get(), &xport::ExportJob::finished, engine, [engine, id](const xport::ExportResult& res) {
    auto it = engine->exports().find(id);
    if (it == engine->exports().end()) return;
    ExportRecord& r = it->second;
    r.state = res.cancelled ? QStringLiteral("cancelled") : res.ok ? QStringLiteral("done") : QStringLiteral("failed");
    r.error = res.error;
    r.stage = r.state;
    if (res.ok) r.fraction = 1;
    r.warnings = QJsonArray::fromStringList(res.warnings);
    r.result = QJsonObject{{q("path"), QDir::toNativeSeparators(res.path)}, {q("videoFrames"), static_cast<double>(res.videoFrames)}, {q("audioSamples"), static_cast<double>(res.audioSamples)},
                           {q("seconds"), res.seconds}, {q("averageFps"), res.averageFps}, {q("videoEncoder"), res.videoEncoder}, {q("appliedGainDb"), res.appliedGainDb},
                           {q("missingFrames"), static_cast<double>(res.missingFrames)}};
    engine->emitEvent(q("export.finished"), recordJson(r));
    // the worker thread is joined by the job's destructor: drop it once the signal has unwound
    QTimer::singleShot(0, engine, [engine, id] {
      auto jt = engine->exports().find(id);
      if (jt != engine->exports().end()) jt->second.job.reset();
    });
  });
  rec.job->start();
  QJsonObject out{{q("jobId"), id}, {q("outputPath"), QDir::toNativeSeparators(rec.outputPath)}, {q("plan"), planJson(*plan)}, {q("note"), QStringLiteral("Poll export.status with this jobId, or subscribe to export.progress / export.finished events.")}};
  if (autoOut) out.insert(q("autoOutput"), true);
  return Result::success(out);
}

Result exportStatus(CallContext& c, const QJsonObject& a) {
  Engine& eng = *c.engine;
  if (!a.contains(q("jobId"))) {
    QJsonArray all;
    for (const auto& [id, rec] : eng.exports()) all.append(recordJson(rec));
    return Result::success(QJsonObject{{q("jobs"), all}});
  }
  const QString id = a.value(q("jobId")).toString();
  if (!eng.exports().count(id)) return notFound(QStringLiteral("No export job %1.").arg(id));
  const int waitMs = a.value(q("waitMs")).toInt(0);
  if (waitMs > 0) {
    QElapsedTimer t;
    t.start();
    while (eng.exports().at(id).state == QLatin1String("running") && t.elapsed() < waitMs) QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
  }
  return Result::success(recordJson(eng.exports().at(id)));
}

} // namespace

void registerExportCommands(Engine& e) {
  e.add(spec("export.start", "exportVideo", "engine", false,
             QStringLiteral("Start an export job (render + encode on worker threads) and return a jobId at once; poll export.status or listen for export.progress / export.finished events. "
                            "Options as `splitframe-cli export`: preset (\"720p\", \"1080p\", \"1440p\", \"2160p\", \"vertical\", \"TikTok / Reels / Shorts\", \"YouTube 1080p\", \"Square 1080\"), quality "
                            "(draft|standard|high|master), codec (h264|h265|prores|dnxhr), container, audioCodec, noAudio, audioOnly, crf, bitrate (kbps), width/height, range [in, out) in frames, "
                            "lufs / truePeak (loudness normalisation). Without `out` the file goes next to the project (or a temp folder). The project is exported as it is now."),
             js::obj({{q("out"), js::str(QStringLiteral("Output file (.mp4 .mov .mkv .wav)"))}, {q("overwrite"), js::boolean()}, {q("preset"), js::str()},
                      {q("quality"), js::choice({q("draft"), q("standard"), q("high"), q("master")})}, {q("codec"), js::choice({q("h264"), q("h265"), q("prores"), q("dnxhr")})},
                      {q("container"), js::choice({q("mp4"), q("mov"), q("mkv"), q("wav")})}, {q("audioCodec"), js::choice({q("aac"), q("pcm16"), q("pcm24"), q("pcm32f"), q("none")})},
                      {q("noAudio"), js::boolean()}, {q("audioOnly"), js::boolean()}, {q("crf"), js::integer(QString(), 0, 63)}, {q("encoderPreset"), js::str()},
                      {q("bitrate"), js::integer(QStringLiteral("Average video bitrate, kbps"), 1)}, {q("width"), js::integer(QString(), 16, 8192)}, {q("height"), js::integer(QString(), 16, 8192)},
                      {q("range"), [] { QJsonObject o = js::array(js::integer(QString(), 0)); o.insert(q("minItems"), 2); o.insert(q("maxItems"), 2); return o; }()},
                      {q("tenBit"), js::boolean()}, {q("proresProfile"), js::choice({q("proxy"), q("lt"), q("standard"), q("hq"), q("4444")})}, {q("dnxProfile"), js::str()},
                      {q("lufs"), js::number(QString(), -70, 0)}, {q("truePeak"), js::number(QString(), -20, 3)}, {q("hw"), js::boolean(QStringLiteral("Use NVENC when available"))},
                      {q("hwDecode"), js::boolean()}}),
             exportStart));
  e.add(spec("export.status", "getExportStatus", "engine", false,
             QStringLiteral("State of an export job: running | done | failed | cancelled, with progress, output path and result. Without jobId lists all jobs. waitMs blocks (processing events) up to that long for it to finish."),
             js::obj({{q("jobId"), js::str()}, {q("waitMs"), js::integer(QString(), 0, 600000)}}), exportStatus, false));
  e.add(spec("export.cancel", "", "engine", false, QStringLiteral("Cancel a running export (the partial file is removed)."), js::obj({{q("jobId"), js::str()}}, {q("jobId")}),
             [](CallContext& c, const QJsonObject& a) {
               auto it = c.engine->exports().find(a.value(q("jobId")).toString());
               if (it == c.engine->exports().end()) return notFound(QStringLiteral("No export job %1.").arg(a.value(q("jobId")).toString()));
               if (it->second.state != QLatin1String("running") || !it->second.job) return Result::success(QJsonObject{{q("cancelled"), false}, {q("state"), it->second.state}});
               it->second.job->cancel();
               return Result::success(QJsonObject{{q("cancelled"), true}, {q("state"), it->second.state}});
             },
             false));
  e.add(spec("presets.list", "listExportPresets", "engine", false, QStringLiteral("Export presets (size / bitrate / codec / container), quality tiers and which software encoders this FFmpeg build has."), js::obj({}),
             [](CallContext&, const QJsonObject&) {
               QJsonArray presets;
               for (const xport::Preset& p : xport::listPresets()) {
                 QJsonObject o{{q("name"), p.name}, {q("video"), xport::videoCodecName(p.video)}, {q("container"), xport::containerName(p.container)}};
                 if (p.shortSide > 0) o.insert(q("shortSide"), p.shortSide);
                 else {
                   o.insert(q("width"), p.width);
                   o.insert(q("height"), p.height);
                   o.insert(q("videoBitrateK"), p.videoBitrateK);
                 }
                 presets.append(o);
               }
               QJsonObject enc;
               for (const auto& [name, codec] : {std::pair{"h264", xport::VideoCodec::H264}, std::pair{"h265", xport::VideoCodec::H265}, std::pair{"prores", xport::VideoCodec::ProRes},
                                                  std::pair{"dnxhr", xport::VideoCodec::DnxHr}})
                 enc.insert(q(name), xport::softwareEncoderAvailable(codec));
               return Result::success(QJsonObject{{q("presets"), presets}, {q("qualities"), QJsonArray{q("draft"), q("standard"), q("high"), q("master")}}, {q("softwareEncoders"), enc}});
             },
             false));
}

} // namespace sf::engine
