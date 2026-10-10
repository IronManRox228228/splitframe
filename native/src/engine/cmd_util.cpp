#include "engine/cmd_util.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <QImage>
#include <QStandardPaths>

#include <atomic>

namespace sf::engine {

namespace {

void collectCreated(const QJsonObject& op, QJsonArray& items, QJsonArray& tracks, QJsonArray& markers, QJsonArray& other) {
  const QString type = op.value(q("type")).toString();
  if (type == QLatin1String("batch")) {
    for (const QJsonValue& v : op.value(q("ops")).toArray()) collectCreated(v.toObject(), items, tracks, markers, other);
  } else if (type == QLatin1String("item.add")) {
    items.append(op.value(q("item")).toObject().value(q("id")));
  } else if (type == QLatin1String("item.split") || type == QLatin1String("item.clone")) {
    items.append(op.value(q("newItemId")));
  } else if (type == QLatin1String("track.add")) {
    tracks.append(op.value(q("trackId")));
  } else if (type == QLatin1String("marker.add")) {
    markers.append(op.value(q("marker")).toObject().value(q("id")));
  } else if (type == QLatin1String("effect.add")) {
    other.append(op.value(q("effect")).toObject().value(q("id")));
  } else if (type == QLatin1String("mask.add")) {
    other.append(op.value(q("mask")).toObject().value(q("id")));
  }
}

} // namespace

QJsonObject createdIds(const std::vector<Op>& ops) {
  QJsonArray items, tracks, markers, other;
  for (const Op& o : ops) collectCreated(toJson(o), items, tracks, markers, other);
  QJsonObject out;
  if (!items.isEmpty()) out.insert(q("items"), items);
  if (!tracks.isEmpty()) out.insert(q("tracks"), tracks);
  if (!markers.isEmpty()) out.insert(q("markers"), markers);
  if (!other.isEmpty()) out.insert(q("other"), other);
  return out;
}

Result applyToSession(CallContext& c, const std::vector<Op>& ops, const QString& label, const QJsonObject& extra) {
  editor::Project& p = *c.session->project;
  if (!p.apply(ops, label, c.actor())) return Result::failure(q("edit_rejected"), p.lastError(), QJsonObject{{q("label"), label}});
  QJsonArray inverse;
  for (const Op& o : p.lastInverse()) inverse.append(toJson(o));
  QJsonObject out{{q("applied"), static_cast<int>(ops.size())}, {q("label"), label}, {q("created"), createdIds(ops)}, {q("inverse"), inverse},
                  {q("canUndo"), p.canUndo()}, {q("canRedo"), p.canRedo()}, {q("undoLabel"), p.undoLabel()}};
  for (auto it = extra.begin(); it != extra.end(); ++it) out.insert(it.key(), it.value());
  return Result::success(out);
}

QJsonObject itemSummary(const TimelineDoc& doc, const Item& item) {
  Q_UNUSED(doc);
  QJsonObject o{{q("id"), item.id}, {q("trackId"), item.trackId}, {q("type"), enumName(item.type())}, {q("startFrame"), static_cast<double>(item.startFrame)},
                {q("durationFrames"), static_cast<double>(item.durationFrames)}, {q("endFrame"), static_cast<double>(itemEnd(item))}};
  if (item.assetId) o.insert(q("assetId"), *item.assetId);
  if (item.sourceInFrame) o.insert(q("sourceInFrame"), static_cast<double>(*item.sourceInFrame));
  if (item.speed != 1) o.insert(q("speed"), item.speed);
  if (item.volume != 1) o.insert(q("volume"), item.volume);
  if (item.muted) o.insert(q("muted"), true);
  if (item.labels.name) o.insert(q("name"), *item.labels.name);
  if (const auto* t = std::get_if<TextProps>(&item.props)) o.insert(q("text"), t->text.left(80));
  return o;
}

QJsonObject trackJson(const Track& t) {
  return QJsonObject{{q("id"), t.id}, {q("kind"), enumName(t.kind)}, {q("name"), t.name}, {q("locked"), t.locked}, {q("muted"), t.muted}, {q("hidden"), t.hidden}};
}

QJsonObject assetBrief(const Asset& a, const editor::MediaPool* pool) {
  QJsonObject o{{q("id"), a.id}, {q("kind"), enumName(a.kind)}, {q("name"), a.originalName}, {q("path"), a.path}, {q("status"), enumName(a.status)},
                {q("durationMs"), static_cast<double>(a.durationMs)}, {q("hasAudio"), a.hasAudio}};
  if (a.width > 0) {
    o.insert(q("width"), static_cast<double>(a.width));
    o.insert(q("height"), static_cast<double>(a.height));
  }
  if (a.fps) o.insert(q("fps"), *a.fps);
  if (a.error) o.insert(q("error"), *a.error);
  if (pool) {
    o.insert(q("uses"), pool->usesOf(a.id));
    const QString proxy = pool->proxyState(a.id);
    if (proxy != QLatin1String("none")) o.insert(q("proxy"), proxy);
  }
  return o;
}

render::AssetTable assetTable(const Session& s) {
  render::AssetTable table;
  for (const Asset& a : s.project->assets()) {
    if (a.status == AssetStatus::Failed || a.path.isEmpty()) continue;
    table[a.id] = {a.path, a.kind, {}};
  }
  return table;
}

void addImageOutProps(QJsonObject& schema) {
  QJsonObject props = schema.value(q("properties")).toObject();
  props.insert(q("path"), js::str(QStringLiteral("Write the PNG here (default: a temp file, returned as `path`).")));
  props.insert(q("inline"), js::boolean(QStringLiteral("Also return the PNG as base64 in `base64` (and as an image content block over MCP).")));
  props.insert(q("maxWidth"), js::integer(QStringLiteral("Downscale to at most this width (default 1280)"), 16, 4096));
  props.insert(q("maxHeight"), js::integer(QStringLiteral("Downscale to at most this height (default 1280)"), 16, 4096));
  schema.insert(q("properties"), props);
}

ImageOut imageOutFrom(const QJsonObject& a) {
  ImageOut o;
  o.path = a.value(q("path")).toString();
  o.inlineData = a.value(q("inline")).toBool();
  o.maxWidth = a.value(q("maxWidth")).toInt(1280);
  o.maxHeight = a.value(q("maxHeight")).toInt(1280);
  return o;
}

Result imageResult(const QImage& image, const ImageOut& out, const QString& stem, const QJsonObject& extra) {
  if (image.isNull()) return Result::failure(q("unavailable"), QStringLiteral("No picture was produced."));
  QImage img = image;
  if (img.width() > out.maxWidth || img.height() > out.maxHeight)
    img = img.scaled(out.maxWidth, out.maxHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  QString path = out.path;
  if (path.isEmpty() || out.inlineData) {
    static std::atomic<int> counter{0};
    if (path.isEmpty()) {
      const QDir dir(QDir::temp().filePath(QStringLiteral("splitframe-engine")));
      QDir().mkpath(dir.absolutePath());
      path = dir.filePath(QStringLiteral("%1_%2_%3.png").arg(stem).arg(QCoreApplication::applicationPid()).arg(++counter));
    }
  }
  if (!out.path.isEmpty() || !out.inlineData) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (!img.save(path, "PNG")) return Result::failure(q("io_error"), QStringLiteral("Can't write %1").arg(path));
  }
  QJsonObject r = extra;
  r.insert(q("width"), img.width());
  r.insert(q("height"), img.height());
  r.insert(q("mimeType"), q("image/png"));
  if (!out.path.isEmpty() || !out.inlineData) r.insert(q("path"), QDir::toNativeSeparators(path));
  if (out.inlineData) {
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    r.insert(q("base64"), QString::fromLatin1(bytes.toBase64()));
  }
  return Result::success(r);
}

} // namespace sf::engine
