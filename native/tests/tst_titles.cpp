// Tests for motion-graphics title templates: parsing, instantiation, keyframe stretch, kinetic words, portrait safety and maxChars warnings.

#include "core/schema.h"
#include "core/timeline_doc.h"
#include "engine/engine.h"
#include "render/effects.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTest>

#include <memory>
#include <set>

using namespace sf;
using namespace sf::engine;

namespace {

QString Q(const char* s) { return QString::fromLatin1(s); }

} // namespace

class TstTitles : public QObject {
  Q_OBJECT

  std::unique_ptr<Engine> e_;

  QJsonObject ok(const QString& cmd, const QJsonObject& args = {}) {
    const Result r = e_->call(cmd, args);
    if (!r.ok) qWarning().noquote() << cmd << "failed:" << r.error.code << r.error.message;
    return r.ok ? r.value.toObject() : QJsonObject{{Q("__failed"), true}};
  }

  Result raw(const QString& cmd, const QJsonObject& args = {}) { return e_->call(cmd, args); }

  void fresh(int w, int h, int fps = 30) {
    ok(Q("project.new"), {{Q("name"), Q("TitlesTest")}, {Q("width"), w}, {Q("height"), h}, {Q("fps"), fps}});
  }

private slots:
  void initTestCase() {
    e_ = std::make_unique<Engine>();
  }

  void cleanupTestCase() {
    e_.reset();
  }

  // 1. allTemplatesValid: every template parses, uses only known properties, easings and effect ids, and has every {field} declared in fields.
  void allTemplatesValid() {
    QFile f(QStringLiteral(":/sf/style/titles.json"));
    QVERIFY2(f.open(QIODevice::ReadOnly), "Could not open :/sf/style/titles.json");
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    QVERIFY(doc.isObject());

    const QJsonArray items = doc.object().value(Q("items")).toArray();
    QVERIFY(items.size() >= 8);

    const std::set<QString> knownProps = {
      Q("positionX"), Q("positionY"), Q("scale"), Q("opacity"), Q("rotation"),
      Q("transform.x"), Q("transform.y"), Q("transform.scale"), Q("transform.opacity"), Q("transform.rotation")
    };

    const std::set<QString> knownEasings = {
      Q("linear"), Q("hold"), Q("easeIn"), Q("easeInCubic"),
      Q("easeOut"), Q("easeOutCubic"), Q("easeOutBack"),
      Q("easeInOut"), Q("easeInOutCubic")
    };

    const QRegularExpression fieldRx(QStringLiteral("\\{([^\\}]+)\\}"));

    for (const QJsonValue& iv : items) {
      const QJsonObject t = iv.toObject();
      const QString tName = t.value(Q("name")).toString();
      QVERIFY(!tName.isEmpty());
      QVERIFY(!t.value(Q("description")).toString().isEmpty());
      QVERIFY(t.value(Q("durationSec")).toDouble() > 0.0);

      const QJsonObject fields = t.value(Q("fields")).toObject();
      const QJsonArray layers = t.value(Q("layers")).toArray();
      QVERIFY(!layers.isEmpty());

      std::set<QString> layerIds;
      for (const QJsonValue& lv : layers) {
        const QJsonObject l = lv.toObject();
        const QString lid = l.value(Q("id")).toString();
        QVERIFY(!lid.isEmpty());
        layerIds.insert(lid);

        const QString kind = l.value(Q("kind")).toString();
        QVERIFY(kind == Q("shape") || kind == Q("text"));

        if (kind == Q("text")) {
          const QString txt = l.value(Q("text")).toString();
          auto matchIt = fieldRx.globalMatch(txt);
          while (matchIt.hasNext()) {
            const auto m = matchIt.next();
            const QString captured = m.captured(1);
            const bool isRepeatWord = l.contains(Q("repeat")) && (captured == Q("word") || captured == Q("i"));
            QVERIFY2(fields.contains(captured) || isRepeatWord,
                     qPrintable(QStringLiteral("Template %1 layer %2 references undeclared field {%3}").arg(tName, lid, captured)));
          }
        }

        const QJsonObject kfs = l.value(Q("keyframes")).toObject();
        for (auto kit = kfs.begin(); kit != kfs.end(); ++kit) {
          QVERIFY2(knownProps.count(kit.key()) > 0,
                   qPrintable(QStringLiteral("Template %1 layer %2 has unknown property %3").arg(tName, lid, kit.key())));

          for (const QJsonValue& kfv : kit.value().toArray()) {
            const QJsonArray kf = kfv.toArray();
            QVERIFY(kf.size() >= 2);
            QVERIFY(kf.at(0).toDouble() >= 0.0);
            if (kf.size() >= 3) {
              const QString easing = kf.at(2).toString();
              QVERIFY2(knownEasings.count(easing) > 0,
                       qPrintable(QStringLiteral("Template %1 layer %2 has unknown easing %3").arg(tName, lid, easing)));
            }
          }
        }
      }

      const QJsonArray effects = t.value(Q("effects")).toArray();
      for (const QJsonValue& ev : effects) {
        const QJsonObject eff = ev.toObject();
        const QString targetLayer = eff.value(Q("layer")).toString();
        QVERIFY(layerIds.count(targetLayer) > 0);
        const QString effectId = eff.value(Q("effectId")).toString();
        QVERIFY2(effectId == Q("glow") || render::FxRegistry::instance().find(effectId) != nullptr,
                 qPrintable(QStringLiteral("Template %1 references unknown effect %2").arg(tName, effectId)));
      }
    }
  }

  // 2. applyCreatesLayers: lower-third-clean with {title: "Ada", subtitle: "Engineer"} on a 1920x1080 project:
  //    - 3 items on one new track;
  //    - the text values are filled in;
  //    - the bar's x keyframe starts off-canvas;
  //    - the duration is 4 s in frames.
  void applyCreatesLayers() {
    fresh(1920, 1080, 30);
    const QJsonObject r = ok(Q("title.apply"), {
      {Q("name"), Q("lower-third-clean")},
      {Q("fields"), QJsonObject{{Q("title"), Q("Ada")}, {Q("subtitle"), Q("Engineer")}}}
    });
    QVERIFY(!r.contains(Q("__failed")));

    const QJsonArray createdItems = r.value(Q("items")).toArray();
    QCOMPARE(createdItems.size(), 3);

    const QString trackId = r.value(Q("track")).toString();
    QVERIFY(!trackId.isEmpty());

    const TimelineDoc& doc = e_->session()->project->doc();
    const Track* trk = getTrack(doc, trackId);
    QVERIFY(trk != nullptr);
    QCOMPARE(trk->name, Q("lower-third-clean"));

    // Verify items on this track
    std::vector<const Item*> trkItems = itemsOnTrack(doc, trackId);
    QCOMPARE(trkItems.size(), size_t{3});

    const Item* barItem = nullptr;
    const Item* t1Item = nullptr;
    const Item* t2Item = nullptr;

    for (const Item* it : trkItems) {
      QCOMPARE(it->durationFrames, Frame{120}); // 4s at 30fps = 120 frames
      if (it->type() == ItemType::Shape) {
        barItem = it;
      } else if (it->type() == ItemType::Text) {
        const auto& tp = std::get<TextProps>(it->props);
        if (tp.text == Q("Ada")) t1Item = it;
        else if (tp.text == Q("Engineer")) t2Item = it;
      }
    }

    QVERIFY(barItem != nullptr);
    QVERIFY(t1Item != nullptr);
    QVERIFY(t2Item != nullptr);

    // The bar's x keyframe starts off-canvas
    const auto kfIt = barItem->keyframes.find(QStringLiteral("transform.x"));
    QVERIFY(kfIt != barItem->keyframes.end());
    QVERIFY(!kfIt->second.empty());
    const double startX = kfIt->second.front().value;
    // On 1920 canvas, canvas centre is 960; an x offset < -960 places the center off-canvas to the left
    QVERIFY2(startX < -1920.0 / 2.0, qPrintable(QStringLiteral("startX = %1, expected < -960").arg(startX)));
  }

  // 3. oneUndo: a single undo removes everything.
  void oneUndo() {
    // Current session still has the applied title from applyCreatesLayers
    const TimelineDoc& docBefore = e_->session()->project->doc();
    const size_t tracksBefore = docBefore.tracks.size();
    QVERIFY(!docBefore.items.empty());

    const QJsonObject undoRes = ok(Q("history.undo"));
    QCOMPARE(undoRes.value(Q("steps")).toInt(), 1);

    const TimelineDoc& docAfter = e_->session()->project->doc();
    QVERIFY(docAfter.items.empty());
    QCOMPARE(docAfter.tracks.size(), tracksBefore - 1);
  }

  // 4. stretchHold: with durationSec 8, the last keyframe is at 8 s and the first-in keyframe times are unchanged.
  void stretchHold() {
    fresh(1920, 1080, 30);
    const QJsonObject r = ok(Q("title.apply"), {
      {Q("name"), Q("lower-third-clean")},
      {Q("durationSec"), 8.0}
    });
    QVERIFY(!r.contains(Q("__failed")));

    const TimelineDoc& doc = e_->session()->project->doc();
    QCOMPARE(doc.items.size(), size_t{3});

    // Check bar shape item
    const Item* barItem = nullptr;
    for (const Item& it : doc.items) {
      if (it.type() == ItemType::Shape) barItem = &it;
    }
    QVERIFY(barItem != nullptr);
    QCOMPARE(barItem->durationFrames, Frame{240}); // 8s at 30fps = 240 frames

    const auto& kfs = barItem->keyframes.at(QStringLiteral("transform.x"));
    QVERIFY(kfs.size() >= 4);

    // First keyframe at 0s (frame 0)
    QCOMPARE(kfs.front().frame, Frame{0});
    // Second keyframe was at 0.45s (0.45 * 30 = 14 frames, unchanged because <= 0.25 * 4.0 = 1.0s)
    QCOMPARE(kfs[1].frame, Frame{14});
    // Last keyframe at 8s (frame 240)
    QCOMPARE(kfs.back().frame, Frame{240});
  }

  // 5. kineticWords: headline-kinetic with words: "make it pop" gives 3 text items, each with its scale keyframes 0.15 s apart.
  void kineticWords() {
    fresh(1920, 1080, 30);
    const QJsonObject r = ok(Q("title.apply"), {
      {Q("name"), Q("headline-kinetic")},
      {Q("fields"), QJsonObject{{Q("words"), Q("make it pop")}}}
    });
    QVERIFY(!r.contains(Q("__failed")));

    const QJsonArray createdItems = r.value(Q("items")).toArray();
    QCOMPARE(createdItems.size(), 3);

    const TimelineDoc& doc = e_->session()->project->doc();
    std::vector<const Item*> textItems;
    for (const QString& id : {createdItems.at(0).toString(), createdItems.at(1).toString(), createdItems.at(2).toString()}) {
      const Item* it = getItem(doc, id);
      QVERIFY(it != nullptr);
      QCOMPARE(it->type(), ItemType::Text);
      textItems.push_back(it);
    }

    QCOMPARE(textItems.size(), size_t{3});
    QCOMPARE(std::get<TextProps>(textItems[0]->props).text, Q("make"));
    QCOMPARE(std::get<TextProps>(textItems[1]->props).text, Q("it"));
    QCOMPARE(std::get<TextProps>(textItems[2]->props).text, Q("pop"));

    // Scale keyframes start times:
    // Item 0: start at 0.0s -> frame 0
    // Item 1: start at 0.15s -> frame round(0.15 * 30) = 5 (or 4-5)
    // Item 2: start at 0.30s -> frame round(0.30 * 30) = 9
    const auto& kf0 = textItems[0]->keyframes.at(QStringLiteral("transform.scale"));
    const auto& kf1 = textItems[1]->keyframes.at(QStringLiteral("transform.scale"));
    const auto& kf2 = textItems[2]->keyframes.at(QStringLiteral("transform.scale"));

    QCOMPARE(kf0.front().frame, Frame{0});
    const Frame delta1 = kf1.front().frame - kf0.front().frame;
    const Frame delta2 = kf2.front().frame - kf1.front().frame;

    // 0.15s at 30fps is 4.5 frames (rounds to 4 or 5)
    QVERIFY(delta1 >= 4 && delta1 <= 5);
    QVERIFY(delta2 >= 4 && delta2 <= 5);
  }

  // 6. safeOnPortrait: on a 1080x1920 project, lower-third-clean-portrait and end-card-portrait give no safe_zones violations in check.
  void safeOnPortrait() {
    fresh(1080, 1920, 30);
    const QJsonObject r1 = ok(Q("title.apply"), {{Q("name"), Q("lower-third-clean-portrait")}});
    QVERIFY(!r1.contains(Q("__failed")));
    const QJsonObject chk1 = r1.value(Q("check")).toObject();
    QVERIFY(!chk1.isEmpty());
    QVERIFY(chk1.contains(Q("pass")));
    QCOMPARE(chk1.value(Q("pass")).toBool(), true);
    const QJsonArray viols1 = chk1.value(Q("violations")).toArray();
    QVERIFY2(viols1.isEmpty(), qPrintable(QString::fromUtf8(QJsonDocument(chk1).toJson())));

    fresh(1080, 1920, 30);
    const QJsonObject r2 = ok(Q("title.apply"), {{Q("name"), Q("end-card-portrait")}});
    QVERIFY(!r2.contains(Q("__failed")));
    const QJsonObject chk2 = r2.value(Q("check")).toObject();
    QVERIFY(!chk2.isEmpty());
    QVERIFY(chk2.contains(Q("pass")));
    QCOMPARE(chk2.value(Q("pass")).toBool(), true);
    const QJsonArray viols2 = chk2.value(Q("violations")).toArray();
    QVERIFY2(viols2.isEmpty(), qPrintable(QString::fromUtf8(QJsonDocument(chk2).toJson())));
  }

  // 7. maxCharsWarns: a 50-char title for lower-third-clean produces a warning.
  void maxCharsWarns() {
    fresh(1920, 1080, 30);
    const QString fiftyCharTitle = QStringLiteral("This title has exactly fifty characters long right now");
    QVERIFY(fiftyCharTitle.size() >= 50);

    const QJsonObject r = ok(Q("title.apply"), {
      {Q("name"), Q("lower-third-clean")},
      {Q("fields"), QJsonObject{{Q("title"), fiftyCharTitle}}}
    });
    QVERIFY(!r.contains(Q("__failed")));

    const QJsonArray warnings = r.value(Q("warnings")).toArray();
    QVERIFY(!warnings.isEmpty());

    bool foundMaxCharsWarn = false;
    for (const QJsonValue& w : warnings) {
      if (w.toString().contains(Q("maxChars"), Qt::CaseInsensitive) || w.toString().contains(Q("length"), Qt::CaseInsensitive)) {
        foundMaxCharsWarn = true;
      }
    }
    QVERIFY(foundMaxCharsWarn);
  }
};

QTEST_MAIN(TstTitles)
#include "tst_titles.moc"
