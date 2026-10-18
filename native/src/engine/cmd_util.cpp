#include "engine/cmd_util.h"
#include "media/frame_service.h"
#include "render/colour_capture.h"
#include "render/layer_painter.h"
#include "render/layout.h"
#include "render/media_provider.h"
#include "render/offscreen.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <QStandardPaths>

#include <atomic>
#include <cmath>

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


Result renderFrames(CallContext& c, const TimelineDoc& doc, const std::vector<Frame>& frames, const FrameFn& fn) {
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
  for (const Frame f : frames) {
    provider.prepare(doc, f, 0);
    const QImage img = off->render(doc, f, provider);
    if (img.isNull()) return Result::failure(q("unavailable"), QStringLiteral("The renderer returned no picture for frame %1.").arg(f));
    fn(f, img.convertToFormat(QImage::Format_RGBA8888));
  }
  return Result::success(QJsonObject{});
}

QImage composeGrid(const std::vector<QImage>& cells, const QStringList& labels, int columns, int cellWidth) {
  const int n = static_cast<int>(cells.size());
  if (n <= 0) return QImage();

  int cols = columns > 0 ? columns : static_cast<int>(std::ceil(std::sqrt(n)));
  cols = std::clamp(cols, 1, 16);
  const int rows = (n + cols - 1) / cols;

  double aspect = 9.0 / 16.0;
  for (const QImage& img : cells) {
    if (!img.isNull() && img.width() > 0) {
      aspect = static_cast<double>(img.height()) / static_cast<double>(img.width());
      break;
    }
  }

  const int cellW = std::clamp(cellWidth > 0 ? cellWidth : 480, 240, 960);
  const int cellH = std::max(1, static_cast<int>(std::lround(cellW * aspect)));
  const int labelBarH = 28;
  const int cellTotalH = cellH + labelBarH;
  const int gutter = 8;
  const int sheetW = gutter + cols * (cellW + gutter);
  const int sheetH = gutter + rows * (cellTotalH + gutter);

  QImage sheet(sheetW, sheetH, QImage::Format_RGBA8888);
  sheet.fill(QColor(QStringLiteral("#111111")));
  QPainter painter(&sheet);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setRenderHint(QPainter::TextAntialiasing, true);
  QFont font;
  font.setPixelSize(14);
  painter.setFont(font);

  for (int i = 0; i < n; ++i) {
    const int col = i % cols;
    const int row = i / cols;
    const int cellX = gutter + col * (cellW + gutter);
    const int cellY = gutter + row * (cellTotalH + gutter);

    const QImage& cellImg = cells[static_cast<size_t>(i)];
    if (cellImg.isNull()) {
      painter.fillRect(QRect(cellX, cellY, cellW, cellH), QColor(QStringLiteral("#555555")));
      painter.setPen(QColor(QStringLiteral("#ff6666")));
      painter.drawText(QRect(cellX + 8, cellY + 8, cellW - 16, cellH - 16), Qt::AlignCenter | Qt::TextWordWrap, QStringLiteral("error"));
    } else {
      QImage scaled = cellImg.scaled(cellW, cellH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
      painter.drawImage(cellX, cellY, scaled);
    }

    // Draw label bar
    painter.fillRect(QRect(cellX, cellY + cellH, cellW, labelBarH), QColor(QStringLiteral("#111111")));
    painter.setPen(QColor(QStringLiteral("#ffffff")));
    const QString labelText = (i < labels.size()) ? labels[i] : QString();
    if (!labelText.isEmpty()) {
      painter.drawText(QRect(cellX + 8, cellY + cellH, cellW - 16, labelBarH), Qt::AlignVCenter | Qt::AlignLeft, labelText);
    }
  }
  painter.end();
  return sheet;
}

} // namespace sf::engine
