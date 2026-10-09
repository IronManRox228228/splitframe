#pragma once

// Fixtures for the editor suites: a project with a few tracks and clips, and (with the ffmpeg CLI)
// a real multi-track project on disk for the UI smoke test and screenshots.

#include "core/ops.h"
#include "core/timeline_doc.h"
#include "editor/project.h"
#include "media_testutil.h"

#include <QDir>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QTest>

namespace sf::test {

using editor::Project;

inline Asset makeAsset(const QString& id, AssetKind kind, const QString& name, qint64 durationMs, const QString& path = {}) {
  Asset a;
  a.id = id;
  a.projectId = QStringLiteral("prj_test");
  a.kind = kind;
  a.path = path.isEmpty() ? QStringLiteral("C:/media/") + name : path;
  a.originalName = name;
  a.status = AssetStatus::Analyzed;
  a.durationMs = durationMs;
  a.width = kind == AssetKind::Audio ? 0 : 1920;
  a.height = kind == AssetKind::Audio ? 0 : 1080;
  a.fps = kind == AssetKind::Video ? std::optional<double>(30) : std::nullopt;
  a.hasAudio = kind != AssetKind::Image;
  a.createdAt = QStringLiteral("2026-01-01T00:00:00.000Z");
  return a;
}

inline const Track& trackNamed(const TimelineDoc& doc, TrackKind kind) {
  for (const Track& t : doc.tracks)
    if (t.kind == kind) return t;
  throw std::runtime_error("no such track");
}

inline int trackIndex(const TimelineDoc& doc, TrackKind kind) {
  for (size_t i = 0; i < doc.tracks.size(); ++i)
    if (doc.tracks[i].kind == kind) return static_cast<int>(i);
  return -1;
}

// Three default tracks (text, video, audio) and, by id: v1 [0,90) and v2 [120,210) on the video
// track, a1 [0,200) on audio, t1 [30,90) text. Assets ast_video (3 s) and ast_music (10 s) exist.
inline void fillProject(Project& p, int fps = 30) {
  p.newProject(QStringLiteral("Test project"), 1920, 1080, fps);
  p.addAsset(makeAsset(QStringLiteral("ast_video"), AssetKind::Video, QStringLiteral("clip.mp4"), 3000));
  p.addAsset(makeAsset(QStringLiteral("ast_music"), AssetKind::Audio, QStringLiteral("music.wav"), 10000));
  p.addAsset(makeAsset(QStringLiteral("ast_still"), AssetKind::Image, QStringLiteral("still.png"), 0));
  const TimelineDoc& doc = p.doc();
  const auto item = [&](const QString& id, ItemType type, TrackKind track, Frame start, Frame dur, const QString& asset) {
    ItemInit init;
    init.id = id;
    init.trackId = trackNamed(doc, track).id;
    init.startFrame = start;
    init.durationFrames = dur;
    if (!asset.isEmpty()) init.assetId = asset;
    if (isMediaType(type)) init.sourceInFrame = 0;
    if (type == ItemType::Text) {
      TextStyle s;
      s.fontFamily = QStringLiteral("Inter");
      s.fontSize = 64;
      s.color = QStringLiteral("#ffffff");
      init.props = TextProps{QStringLiteral("Hello"), s};
    }
    return ItemAdd{createItem(type, init), false};
  };
  std::vector<Op> ops;
  ops.emplace_back(item(QStringLiteral("itm_v1"), ItemType::Video, TrackKind::Video, 0, 90, QStringLiteral("ast_video")));
  ops.emplace_back(item(QStringLiteral("itm_v2"), ItemType::Video, TrackKind::Video, 120, 90, QStringLiteral("ast_video")));
  ops.emplace_back(item(QStringLiteral("itm_a1"), ItemType::Audio, TrackKind::Audio, 0, 200, QStringLiteral("ast_music")));
  ops.emplace_back(item(QStringLiteral("itm_t1"), ItemType::Text, TrackKind::Text, 30, 60, {}));
  QVERIFY2(p.apply(ops, QStringLiteral("fixture")), qPrintable(p.lastError()));
  // the fixture is the starting point: nothing to undo, nothing to save
  p.load(p.bundle(), QString(), false);
}

inline const Item& itemOf(const Project& p, const QString& id) { return requireItem(p.doc(), id); }

// ---------------------------------------------------------------- demo project on disk

// Writes clips and a multi-track project into `dir`; returns the project file. Needs the ffmpeg CLI.
// Layout: a title and a lower third on the text track, a shape on an overlay track, two video
// tracks (b-roll over main), two audio tracks (music and a voice-like tone), plus a still.
inline QString writeDemoProject(const QString& dir) {
  const QString clipA = QDir(dir).absoluteFilePath(QStringLiteral("clip_a.mp4"));
  const QString clipB = QDir(dir).absoluteFilePath(QStringLiteral("clip_b.mp4"));
  const QString music = QDir(dir).absoluteFilePath(QStringLiteral("music.wav"));
  const QString voice = QDir(dir).absoluteFilePath(QStringLiteral("voice.wav"));
  const QString still = QDir(dir).absoluteFilePath(QStringLiteral("still.png"));
  QString log;
  const QStringList mpeg4{QStringLiteral("-c:v"), QStringLiteral("mpeg4"), QStringLiteral("-q:v"), QStringLiteral("4"), QStringLiteral("-g"), QStringLiteral("10")};
  if (!makeIndexClip(clipA, mpeg4, 320, 180, 30, 6, {}, &log)) return {};
  if (!runFfmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("testsrc2=s=320x180:r=30:d=5"), QStringLiteral("-c:v"),
                  QStringLiteral("mpeg4"), QStringLiteral("-q:v"), QStringLiteral("4"), QStringLiteral("-g"), QStringLiteral("10"), clipB}, &log))
    return {};
  if (!runFfmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("sine=frequency=330:duration=20"), QStringLiteral("-ac"), QStringLiteral("2"),
                  QStringLiteral("-c:a"), QStringLiteral("pcm_s16le"), music}, &log))
    return {};
  if (!runFfmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("sine=frequency=880:duration=8"), QStringLiteral("-af"),
                  QStringLiteral("tremolo=f=3:d=0.9"), QStringLiteral("-c:a"), QStringLiteral("pcm_s16le"), voice}, &log))
    return {};
  QImage img(640, 360, QImage::Format_RGB32);
  img.fill(QColor(0x2c, 0x5a, 0x4e));
  {
    QPainter p(&img);
    p.setPen(Qt::white);
    p.setBrush(QColor(0xfb, 0xbf, 0x24));
    p.drawEllipse(QRect(220, 90, 200, 180));
  }
  if (!img.save(still)) return {};

  Project p;
  p.newProject(QStringLiteral("Demo edit"), 1280, 720, 30);
  p.addAsset(makeAsset(QStringLiteral("ast_a"), AssetKind::Video, QStringLiteral("clip_a.mp4"), 6000, clipA));
  p.addAsset(makeAsset(QStringLiteral("ast_b"), AssetKind::Video, QStringLiteral("clip_b.mp4"), 5000, clipB));
  p.addAsset(makeAsset(QStringLiteral("ast_music"), AssetKind::Audio, QStringLiteral("music.wav"), 20000, music));
  p.addAsset(makeAsset(QStringLiteral("ast_voice"), AssetKind::Audio, QStringLiteral("voice.wav"), 8000, voice));
  p.addAsset(makeAsset(QStringLiteral("ast_still"), AssetKind::Image, QStringLiteral("still.png"), 0, still));
  // extra tracks first, so every later op can name them
  const auto addTrack = [&](const QString& id, TrackKind kind, const QString& name, std::optional<std::int64_t> index) {
    TrackAdd t;
    t.trackId = id;
    t.kind = kind;
    t.name = name;
    t.index = index;
    return t;
  };
  std::vector<Op> ops;
  ops.emplace_back(addTrack(QStringLiteral("trk_overlay"), TrackKind::Overlay, QStringLiteral("Overlay 1"), 1));
  ops.emplace_back(addTrack(QStringLiteral("trk_broll"), TrackKind::Video, QStringLiteral("B-roll"), 2));
  ops.emplace_back(addTrack(QStringLiteral("trk_audio2"), TrackKind::Audio, QStringLiteral("Voice"), std::nullopt));
  if (!p.apply(ops, QStringLiteral("tracks"))) return {};
  ops.clear();

  const TimelineDoc& doc = p.doc();
  const QString textTrack = trackNamed(doc, TrackKind::Text).id;
  QString mainId;
  for (const Track& t : doc.tracks)
    if (t.kind == TrackKind::Video && t.name == QStringLiteral("Main Video")) mainId = t.id;
  const QString audioMain = [&] {
    for (const Track& t : doc.tracks)
      if (t.kind == TrackKind::Audio && t.name == QStringLiteral("Audio 1")) return t.id;
    return QString();
  }();

  const auto media = [&](const QString& id, ItemType type, const QString& track, Frame start, Frame dur, const QString& asset, Frame sourceIn = 0) {
    ItemInit init;
    init.id = id;
    init.trackId = track;
    init.startFrame = start;
    init.durationFrames = dur;
    init.assetId = asset;
    if (isMediaType(type)) init.sourceInFrame = sourceIn;
    return ItemAdd{createItem(type, init), false};
  };
  // main video: five clips cut from the two sources, with a gap
  ops.emplace_back(media(QStringLiteral("itm_m1"), ItemType::Video, mainId, 0, 90, QStringLiteral("ast_a")));
  ops.emplace_back(media(QStringLiteral("itm_m2"), ItemType::Video, mainId, 90, 75, QStringLiteral("ast_b")));
  ops.emplace_back(media(QStringLiteral("itm_m3"), ItemType::Video, mainId, 165, 60, QStringLiteral("ast_a"), 90));
  ops.emplace_back(media(QStringLiteral("itm_m4"), ItemType::Video, mainId, 240, 90, QStringLiteral("ast_b"), 30));
  ops.emplace_back(media(QStringLiteral("itm_still"), ItemType::Image, mainId, 330, 90, QStringLiteral("ast_still")));
  // b-roll above
  ops.emplace_back(media(QStringLiteral("itm_b1"), ItemType::Video, QStringLiteral("trk_broll"), 60, 60, QStringLiteral("ast_b"), 60));
  ops.emplace_back(media(QStringLiteral("itm_b2"), ItemType::Video, QStringLiteral("trk_broll"), 200, 70, QStringLiteral("ast_a"), 30));
  // audio
  ops.emplace_back(media(QStringLiteral("itm_au1"), ItemType::Audio, audioMain, 0, 420, QStringLiteral("ast_music")));
  ops.emplace_back(media(QStringLiteral("itm_au2"), ItemType::Audio, QStringLiteral("trk_audio2"), 45, 120, QStringLiteral("ast_voice")));
  ops.emplace_back(media(QStringLiteral("itm_au3"), ItemType::Audio, QStringLiteral("trk_audio2"), 210, 150, QStringLiteral("ast_voice"), 30));
  // titles and a shape
  const auto text = [&](const QString& id, Frame start, Frame dur, const QString& str, double size) {
    TextStyle s;
    s.fontFamily = QStringLiteral("Segoe UI");
    s.fontSize = size;
    s.fontWeight = 700;
    s.color = QStringLiteral("#ffffff");
    s.strokeColor = QStringLiteral("#000000");
    s.strokeWidth = 2;
    ItemInit init;
    init.id = id;
    init.trackId = textTrack;
    init.startFrame = start;
    init.durationFrames = dur;
    init.props = TextProps{str, s};
    return ItemAdd{createItem(ItemType::Text, init), false};
  };
  ops.emplace_back(text(QStringLiteral("itm_title"), 10, 80, QStringLiteral("Demo edit"), 72));
  ops.emplace_back(text(QStringLiteral("itm_lower"), 120, 90, QStringLiteral("Lower third"), 40));
  ops.emplace_back(text(QStringLiteral("itm_outro"), 340, 80, QStringLiteral("Thanks for watching"), 56));
  {
    ShapeProps shape;
    shape.shape = ShapeKind::Ellipse;
    shape.fill = QStringLiteral("#5FB7A1");
    shape.width = 240;
    shape.height = 240;
    ItemInit init;
    init.id = QStringLiteral("itm_shape");
    init.trackId = QStringLiteral("trk_overlay");
    init.startFrame = 150;
    init.durationFrames = 90;
    init.props = shape;
    init.transform = TransformPatch{.x = 380, .y = -160, .opacity = 0.8};
    ops.emplace_back(ItemAdd{createItem(ItemType::Shape, init), false});
  }
  if (!p.apply(ops, QStringLiteral("clips"))) return {};
  QString err;
  const QString file = QDir(dir).absoluteFilePath(QStringLiteral("demo.json"));
  if (!p.saveAs(file, &err)) return {};
  return file;
}

} // namespace sf::test
