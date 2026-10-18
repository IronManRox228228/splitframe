// Variations contact sheet: render one frame under N variants into a labelled grid PNG.
// Deterministic evaluation without altering project state.

#include "engine/cmd_util.h"
#include "engine/cmd_style.h"
#include "engine/cmd_fx.h"
#include "engine/style_library.h"
#include "render/colour_analysis.h"
#include "render/color.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace sf::engine {

namespace {

double round4(double v) { return std::round(v * 10000.0) / 10000.0; }

render::ColourMetrics measureImage(const QImage& img, double* outDisplayMeanLuma) {
  if (img.isNull()) {
    if (outDisplayMeanLuma) *outDisplayMeanLuma = 0.0;
    return {};
  }
  const QImage d8 = (img.format() == QImage::Format_RGBA8888) ? img : img.convertToFormat(QImage::Format_RGBA8888);
  const int w = d8.width(), h = d8.height();
  const int maxSide = render::kScopeMaxSide; // 480
  const int step = std::max(1, (std::max(w, h) + maxSide - 1) / maxSide);
  const size_t cap = static_cast<size_t>((w + step - 1) / step) * static_cast<size_t>((h + step - 1) / step);
  render::PixelSet ps;
  ps.lin.reserve(cap * 3);
  ps.disp.reserve(cap * 3);
  double dispLumaSum = 0.0;
  size_t validPixels = 0;
  for (int y = 0; y < h; y += step) {
    const uchar* row = d8.constScanLine(y);
    for (int x = 0; x < w; x += step) {
      const uchar a = row[x * 4 + 3];
      if (a < 250) continue;
      const float r = row[x * 4] / 255.f;
      const float g = row[x * 4 + 1] / 255.f;
      const float b = row[x * 4 + 2] / 255.f;
      ps.disp.insert(ps.disp.end(), {r, g, b});
      ps.lin.insert(ps.lin.end(), {
        static_cast<float>(render::toLinear(render::Transfer::Srgb, r)),
        static_cast<float>(render::toLinear(render::Transfer::Srgb, g)),
        static_cast<float>(render::toLinear(render::Transfer::Srgb, b))
      });
      dispLumaSum += (0.2126 * r + 0.7152 * g + 0.0722 * b);
      validPixels++;
    }
  }
  if (validPixels == 0) {
    for (int y = 0; y < h; y += step) {
      const uchar* row = d8.constScanLine(y);
      for (int x = 0; x < w; x += step) {
        const float r = row[x * 4] / 255.f;
        const float g = row[x * 4 + 1] / 255.f;
        const float b = row[x * 4 + 2] / 255.f;
        ps.disp.insert(ps.disp.end(), {r, g, b});
        ps.lin.insert(ps.lin.end(), {
          static_cast<float>(render::toLinear(render::Transfer::Srgb, r)),
          static_cast<float>(render::toLinear(render::Transfer::Srgb, g)),
          static_cast<float>(render::toLinear(render::Transfer::Srgb, b))
        });
        dispLumaSum += (0.2126 * r + 0.7152 * g + 0.0722 * b);
        validPixels++;
      }
    }
  }
  if (outDisplayMeanLuma) {
    *outDisplayMeanLuma = validPixels > 0 ? (dispLumaSum / static_cast<double>(validPixels)) : 0.0;
  }
  return render::analyze(ps);
}

Result variationsSheetCmd(CallContext& c, const QJsonObject& a) {
  const QString itemId = a.value(q("itemId")).toString();
  if (itemId.isEmpty()) return invalid(QStringLiteral("Argument 'itemId' is required."));
  const TimelineDoc& origDoc = c.session->project->doc();
  const Item* it = getItem(origDoc, itemId);
  if (!it) return notFound(QStringLiteral("Item %1 not found.").arg(itemId));
  if (it->type() != ItemType::Video && it->type() != ItemType::Image) {
    return invalid(QStringLiteral("Item %1 is %2: variations apply to video and image items.").arg(itemId, enumName(it->type())));
  }

  const Frame frame = a.contains(q("frame")) ? a.value(q("frame")).toInteger() : (it->startFrame + it->durationFrames / 2);
  const bool includeOriginal = a.value(q("includeOriginal")).toBool(true);

  struct VariantSpec {
    QString label;
    QJsonObject def;
    bool isOriginal = false;
  };
  std::vector<VariantSpec> variantList;
  if (includeOriginal) {
    variantList.push_back({QStringLiteral("original"), {}, true});
  }

  const QJsonArray variantsArr = a.value(q("variants")).toArray();
  for (const auto& vVal : variantsArr) {
    const QJsonObject vObj = vVal.toObject();
    QString label = vObj.value(q("label")).toString();
    if (label.isEmpty()) {
      if (vObj.contains(q("look"))) label = vObj.value(q("look")).toObject().value(q("look")).toString();
      else if (vObj.contains(q("effect"))) label = vObj.value(q("effect")).toObject().value(q("effectId")).toString();
      else if (vObj.contains(q("ops"))) label = QStringLiteral("custom");
    }
    variantList.push_back({label, vObj, false});
  }

  if (variantList.empty()) return invalid(QStringLiteral("No variants specified."));
  if (variantList.size() > 16) return invalid(QStringLiteral("Maximum of 16 variants supported."));

  const int n = static_cast<int>(variantList.size());
  int cols = a.contains(q("columns")) ? a.value(q("columns")).toInt() : static_cast<int>(std::ceil(std::sqrt(n)));
  cols = std::clamp(cols, 1, 16);
  const int rows = (n + cols - 1) / cols;

  const double aspect = static_cast<double>(origDoc.project.height) / static_cast<double>(origDoc.project.width > 0 ? origDoc.project.width : 1);
  const int cellW = std::clamp(a.value(q("cellWidth")).toInt(480), 240, 960);
  const int cellH = std::max(1, static_cast<int>(std::lround(cellW * aspect)));
  const int labelBarH = 28;
  const int cellTotalH = cellH + labelBarH;
  const int gutter = 8;
  const int sheetW = gutter + cols * (cellW + gutter);
  const int sheetH = gutter + rows * (cellTotalH + gutter);

  std::vector<QImage> cellImages;
  QStringList cellLabels;
  QJsonArray cellsOut;
  QJsonArray errorsOut;

  for (int i = 0; i < n; ++i) {
    const VariantSpec& vspec = variantList[static_cast<size_t>(i)];
    const char letterChar = static_cast<char>('A' + i);
    const QString letterStr = QString(QChar(letterChar));
    const int col = i % cols;
    const int row = i / cols;
    const int cellX = gutter + col * (cellW + gutter);
    const int cellY = gutter + row * (cellTotalH + gutter);

    QJsonArray opsUsed;
    TimelineDoc variantDoc = origDoc;
    bool buildOk = true;
    QString errMsg;

    if (vspec.isOriginal) {
      // original unchanged
    } else if (vspec.def.contains(q("look"))) {
      QJsonObject lookArgs = vspec.def.value(q("look")).toObject();
      lookArgs.insert(q("itemIds"), QJsonArray{itemId});
      lookArgs.insert(q("apply"), true);
      lookArgs.insert(q("force"), true);
      QJsonObject info;
      Result r = buildLookOps(c, lookArgs, &opsUsed, &info);
      if (!r.ok) {
        buildOk = false;
        errMsg = r.error.message;
      }
    } else if (vspec.def.contains(q("effect"))) {
      QJsonObject effObj = vspec.def.value(q("effect")).toObject();
      const QString effectId = effObj.value(q("effectId")).toString();
      const QJsonObject effParams = effObj.value(q("params")).toObject();
      Result r = buildEffectAddOps(c, itemId, effectId, effParams, &opsUsed);
      if (!r.ok) {
        buildOk = false;
        errMsg = r.error.message;
      }
    } else if (vspec.def.contains(q("ops"))) {
      opsUsed = vspec.def.value(q("ops")).toArray();
    } else {
      buildOk = false;
      errMsg = QStringLiteral("Variant has no look, effect, or ops.");
    }

    if (buildOk && !opsUsed.isEmpty()) {
      try {
        ApplyResult ar = sf::applyOps(origDoc, parseOps(opsUsed));
        variantDoc = std::move(ar.doc);
      } catch (const std::exception& ex) {
        buildOk = false;
        errMsg = QString::fromUtf8(ex.what());
      }
    }

    QJsonObject measuredObj;

    if (!buildOk) {
      errorsOut.append(QJsonObject{
        {q("letter"), letterStr},
        {q("label"), vspec.label},
        {q("error"), errMsg}
      });
      QImage errImg(cellW, cellH, QImage::Format_RGBA8888);
      errImg.fill(QColor(QStringLiteral("#555555")));
      QPainter ep(&errImg);
      ep.setRenderHint(QPainter::Antialiasing, true);
      ep.setRenderHint(QPainter::TextAntialiasing, true);
      ep.setPen(QColor(QStringLiteral("#ff6666")));
      QFont ef; ef.setPixelSize(14); ep.setFont(ef);
      ep.drawText(QRect(8, 8, cellW - 16, cellH - 16), Qt::AlignCenter | Qt::TextWordWrap, QStringLiteral("error: %1").arg(errMsg));
      ep.end();
      cellImages.push_back(errImg);
    } else {
      QImage renderedImg;
      Result rRender = renderFrames(c, variantDoc, {frame}, [&](Frame, const QImage& img) {
        renderedImg = img;
      });
      if (!rRender.ok || renderedImg.isNull()) {
        const QString rErr = rRender.ok ? QStringLiteral("Null image rendered") : rRender.error.message;
        errorsOut.append(QJsonObject{
          {q("letter"), letterStr},
          {q("label"), vspec.label},
          {q("error"), rErr}
        });
        QImage errImg(cellW, cellH, QImage::Format_RGBA8888);
        errImg.fill(QColor(QStringLiteral("#555555")));
        QPainter ep(&errImg);
        ep.setRenderHint(QPainter::Antialiasing, true);
        ep.setRenderHint(QPainter::TextAntialiasing, true);
        ep.setPen(QColor(QStringLiteral("#ff6666")));
        QFont ef; ef.setPixelSize(14); ep.setFont(ef);
        ep.drawText(QRect(8, 8, cellW - 16, cellH - 16), Qt::AlignCenter | Qt::TextWordWrap, QStringLiteral("error: %1").arg(rErr));
        ep.end();
        cellImages.push_back(errImg);
      } else {
        double dispLuma = 0.0;
        render::ColourMetrics cm = measureImage(renderedImg, &dispLuma);
        measuredObj.insert(q("meanLuma"), round4(dispLuma));
        measuredObj.insert(q("cast"), QJsonObject{
          {q("temperature"), round4(cm.castTemperature)},
          {q("tint"), round4(cm.castTint)}
        });
        measuredObj.insert(q("clippedHighPct"), round4(cm.clippedPct.all));
        measuredObj.insert(q("clippedLowPct"), round4(cm.crushedPct.all));

        cellImages.push_back(renderedImg);
      }
    }

    cellLabels.append(QStringLiteral("%1  %2").arg(letterStr, vspec.label));

    QJsonObject cellObj{
      {q("letter"), letterStr},
      {q("label"), vspec.label},
      {q("x"), cellX},
      {q("y"), cellY},
      {q("w"), cellW},
      {q("h"), cellTotalH},
      {q("measured"), measuredObj},
      {q("ops"), opsUsed}
    };
    cellsOut.append(cellObj);
  }

  QImage sheet = composeGrid(cellImages, cellLabels, cols, cellW);

  QString outPath = a.value(q("path")).toString();
  if (outPath.isEmpty()) {
    outPath = QDir::temp().filePath(QStringLiteral("variations_%1_%2.png").arg(itemId).arg(frame));
  }
  QDir().mkpath(QFileInfo(outPath).absolutePath());
  if (!sheet.save(outPath, "PNG")) {
    return Result::failure(q("io_error"), QStringLiteral("Failed to write contact sheet to %1").arg(outPath));
  }

  return Result::success(QJsonObject{
    {q("path"), QDir::toNativeSeparators(outPath)},
    {q("width"), sheetW},
    {q("height"), sheetH},
    {q("frame"), static_cast<double>(frame)},
    {q("cells"), cellsOut},
    {q("errors"), errorsOut}
  });
}

Result variationsLooksCmd(CallContext& c, const QJsonObject& a) {
  QStringList looks;
  if (a.contains(q("looks"))) {
    for (const auto& v : a.value(q("looks")).toArray()) looks.append(v.toString());
  } else {
    looks = style::lookNames();
  }
  const double intensity = a.value(q("intensity")).toDouble(1.0);
  QJsonArray variants;
  for (const QString& name : looks) {
    variants.append(QJsonObject{
      {q("label"), name},
      {q("look"), QJsonObject{{q("look"), name}, {q("intensity"), intensity}}}
    });
  }
  QJsonObject sheetArgs{{q("session"), c.session->id}, {q("itemId"), a.value(q("itemId"))}, {q("variants"), variants}};
  for (const char* k : {"frame", "columns", "cellWidth", "path", "includeOriginal"}) {
    if (a.contains(q(k))) sheetArgs.insert(q(k), a.value(q(k)));
  }
  return c.engine->call(q("variations.sheet"), sheetArgs);
}

Result variationsEffectsCmd(CallContext& c, const QJsonObject& a) {
  QJsonArray variants;
  for (const auto& v : a.value(q("effectIds")).toArray()) {
    const QString effId = v.toString();
    variants.append(QJsonObject{
      {q("label"), effId},
      {q("effect"), QJsonObject{{q("effectId"), effId}}}
    });
  }
  QJsonObject sheetArgs{{q("session"), c.session->id}, {q("itemId"), a.value(q("itemId"))}, {q("variants"), variants}};
  for (const char* k : {"frame", "columns", "cellWidth", "path", "includeOriginal"}) {
    if (a.contains(q(k))) sheetArgs.insert(q(k), a.value(q(k)));
  }
  return c.engine->call(q("variations.sheet"), sheetArgs);
}

} // namespace

void registerVariationsCommands(Engine& e) {
  e.add(spec("variations.sheet", "", "style", false,
             QStringLiteral("Render one frame under N variants (looks, effects, or raw ops) into a single labelled grid PNG without changing the project. Evaluates deterministically."),
             js::obj({{q("itemId"), js::str(QStringLiteral("Target item ID (video or image)"))},
                      {q("frame"), js::integer(QStringLiteral("Timeline frame to render (default: middle frame of item)"), 0)},
                      {q("variants"), js::array(js::any(), QStringLiteral("Array of 1..16 variant definitions"))},
                      {q("includeOriginal"), js::boolean(QStringLiteral("Include original unchanged frame as cell A (default true)"))},
                      {q("columns"), js::integer(QStringLiteral("Grid columns (default ceil(sqrt(n)))"), 1, 16)},
                      {q("cellWidth"), js::integer(QStringLiteral("Cell width in pixels (240..960, default 480)"), 240, 960)},
                      {q("path"), js::str(QStringLiteral("Output PNG path"))}},
                     {q("itemId"), q("variants")}),
             variationsSheetCmd));

  e.add(spec("variations.looks", "", "style", false,
             QStringLiteral("Render variations contact sheet for all or selected looks with a given intensity. Evaluates deterministically."),
             js::obj({{q("itemId"), js::str(QStringLiteral("Target item ID"))},
                      {q("frame"), js::integer(QStringLiteral("Timeline frame to render"), 0)},
                      {q("looks"), js::array(js::str(), QStringLiteral("List of look names (default: all available looks)"))},
                      {q("intensity"), js::number(QStringLiteral("Look intensity (0..1, default 1)"), 0, 1)},
                      {q("includeOriginal"), js::boolean()},
                      {q("columns"), js::integer(QString(), 1, 16)},
                      {q("cellWidth"), js::integer(QString(), 240, 960)},
                      {q("path"), js::str()}},
                     {q("itemId")}),
             variationsLooksCmd));

  e.add(spec("variations.effects", "", "style", false,
             QStringLiteral("Render variations contact sheet comparing multiple effects. Evaluates deterministically."),
             js::obj({{q("itemId"), js::str(QStringLiteral("Target item ID"))},
                      {q("frame"), js::integer(QStringLiteral("Timeline frame to render"), 0)},
                      {q("effectIds"), js::array(js::str(), QStringLiteral("List of effect IDs"))},
                      {q("includeOriginal"), js::boolean()},
                      {q("columns"), js::integer(QString(), 1, 16)},
                      {q("cellWidth"), js::integer(QString(), 240, 960)},
                      {q("path"), js::str()}},
                     {q("itemId"), q("effectIds")}),
             variationsEffectsCmd));
}

} // namespace sf::engine
