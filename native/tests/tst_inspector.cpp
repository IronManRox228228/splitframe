#include "editor/inspector.h"
#include "editor_testutil.h"

#include <QSignalSpy>

namespace tst {
using namespace sf;
using namespace sf::editor;
using namespace sf::test;
using sf::editor::Project; // not sf::Project

namespace {
const QString V1 = QStringLiteral("itm_v1"), A1 = QStringLiteral("itm_a1"), T1 = QStringLiteral("itm_t1");
}

class TstInspector : public QObject {
  Q_OBJECT
private slots:
  void valuesFollowTheSelection() {
    Project p;
    fillProject(p);
    Inspector i(p);
    QCOMPARE(i.count(), 0);
    p.setSelection({V1});
    QCOMPARE(i.count(), 1);
    QCOMPARE(i.itemType(), QStringLiteral("video"));
    QCOMPARE(i.values().value(QStringLiteral("opacity")).toDouble(), 1.0);
    QCOMPARE(i.values().value(QStringLiteral("name")).toString(), QStringLiteral("clip.mp4"));
    QVERIFY(i.values().contains(QStringLiteral("fadeIn")));
    p.setSelection({T1});
    QCOMPARE(i.itemType(), QStringLiteral("text"));
    QCOMPARE(i.values().value(QStringLiteral("text")).toString(), QStringLiteral("Hello"));
    QCOMPARE(i.values().value(QStringLiteral("fontSize")).toDouble(), 64.0);
    QVERIFY(!i.values().contains(QStringLiteral("volume")) || i.values().value(QStringLiteral("volume")).toDouble() == 1.0);
  }
  void sliderDragIsOneUndoStep() {
    Project p;
    fillProject(p);
    Inspector i(p);
    p.setSelection({V1});
    QSignalSpy changed(&i, &Inspector::changed);
    for (double v : {0.9, 0.8, 0.6, 0.5, 0.3}) QVERIFY(i.drag(QStringLiteral("opacity"), v));
    QVERIFY(changed.count() >= 5); // live: the preview sees every value
    QCOMPARE(itemOf(p, V1).transform.opacity, 0.3);
    i.endDrag();
    QVERIFY(p.undo());
    QCOMPARE(itemOf(p, V1).transform.opacity, 1.0);
    QVERIFY(!p.canUndo()); // nothing else was recorded
    QVERIFY(p.redo());
    QCOMPARE(itemOf(p, V1).transform.opacity, 0.3);
  }
  void discreteEditsAreSeparateSteps() {
    Project p;
    fillProject(p);
    Inspector i(p);
    p.setSelection({V1});
    QVERIFY(i.edit(QStringLiteral("opacity"), 0.5));
    QVERIFY(i.edit(QStringLiteral("opacity"), 0.25));
    QVERIFY(p.undo());
    QCOMPARE(itemOf(p, V1).transform.opacity, 0.5);
    QVERIFY(p.undo());
    QCOMPARE(itemOf(p, V1).transform.opacity, 1.0);
  }
  void transformSpeedVolumeMuteAndFades() {
    Project p;
    fillProject(p);
    Inspector i(p);
    p.setSelection({V1});
    QVERIFY(i.edit(QStringLiteral("x"), 120));
    QVERIFY(i.edit(QStringLiteral("scale"), 1.5));
    QVERIFY(i.edit(QStringLiteral("rotation"), -30));
    QVERIFY(i.edit(QStringLiteral("speed"), 2.0));
    QVERIFY(i.edit(QStringLiteral("volume"), 0.5));
    QVERIFY(i.edit(QStringLiteral("muted"), true));
    QVERIFY(i.edit(QStringLiteral("fadeIn"), 12));
    QVERIFY(i.edit(QStringLiteral("fadeOut"), 6));
    const Item& v = itemOf(p, V1);
    QCOMPARE(v.transform.x, 120.0);
    QCOMPARE(v.transform.scale, 1.5);
    QCOMPARE(v.transform.rotation, -30.0);
    QCOMPARE(v.speed, 2.0);
    QCOMPARE(v.volume, 0.5);
    QVERIFY(v.muted);
    QCOMPARE(std::get<VideoProps>(v.props).fadeInFrames, Frame(12));
    QCOMPARE(std::get<VideoProps>(v.props).fadeOutFrames, Frame(6));
    QCOMPARE(i.values().value(QStringLiteral("fadeIn")).toLongLong(), 12);
  }
  void audioClipsHaveNoPictureProperties() {
    Project p;
    fillProject(p);
    Inspector i(p);
    p.setSelection({A1});
    QVERIFY(!i.edit(QStringLiteral("opacity"), 0.5));
    QVERIFY(i.edit(QStringLiteral("volume"), 1.5));
    QVERIFY(!i.addEffect(QStringLiteral("sepia")));
  }
  void textEditsKeepTheRestOfTheProps() {
    Project p;
    fillProject(p);
    Inspector i(p);
    p.setSelection({T1});
    QVERIFY(i.edit(QStringLiteral("text"), QStringLiteral("New title")));
    QVERIFY(i.edit(QStringLiteral("fontSize"), 120));
    QVERIFY(i.edit(QStringLiteral("color"), QStringLiteral("#ffd84d")));
    QVERIFY(i.edit(QStringLiteral("align"), QStringLiteral("left")));
    QVERIFY(i.edit(QStringLiteral("strokeColor"), QStringLiteral("#000000")));
    QVERIFY(i.edit(QStringLiteral("strokeWidth"), 3));
    const auto& t = std::get<TextProps>(itemOf(p, T1).props);
    QCOMPARE(t.text, QStringLiteral("New title"));
    QCOMPARE(t.style.fontSize, 120.0);
    QCOMPARE(t.style.color, QStringLiteral("#ffd84d"));
    QCOMPARE(t.style.align, TextAlign::Left);
    QCOMPARE(t.style.strokeColor.value_or(QString()), QStringLiteral("#000000"));
    QCOMPARE(t.style.fontFamily, QStringLiteral("Inter")); // untouched
    QVERIFY(i.edit(QStringLiteral("strokeColor"), QString())); // empty clears an optional colour
    QVERIFY(!std::get<TextProps>(itemOf(p, T1).props).style.strokeColor.has_value());
    QVERIFY(!i.edit(QStringLiteral("text"), QStringLiteral("   "))); // an empty title is refused
    QCOMPARE(std::get<TextProps>(itemOf(p, T1).props).text, QStringLiteral("New title"));
  }
  void shapeProps() {
    Project p;
    fillProject(p);
    TimelineDoc d = p.doc();
    ShapeProps sp;
    sp.fill = QStringLiteral("#ff0000");
    sp.width = 100;
    sp.height = 50;
    ItemInit init;
    init.id = QStringLiteral("itm_shape");
    init.trackId = trackNamed(d, TrackKind::Text).id;
    init.startFrame = 0;
    init.durationFrames = 30;
    init.props = sp;
    QVERIFY(p.apply(ItemAdd{createItem(ItemType::Shape, init), false}));
    Inspector i(p);
    p.setSelection({QStringLiteral("itm_shape")});
    QCOMPARE(i.itemType(), QStringLiteral("shape"));
    QVERIFY(i.edit(QStringLiteral("shape"), QStringLiteral("ellipse")));
    QVERIFY(i.edit(QStringLiteral("fill"), QStringLiteral("#00ff00")));
    QVERIFY(i.edit(QStringLiteral("width"), 300));
    QVERIFY(i.edit(QStringLiteral("radius"), 12));
    const auto& s = std::get<ShapeProps>(itemOf(p, QStringLiteral("itm_shape")).props);
    QCOMPARE(s.shape, ShapeKind::Ellipse);
    QCOMPARE(s.fill, QStringLiteral("#00ff00"));
    QCOMPARE(s.width, 300.0);
    QCOMPARE(s.radius, 12.0);
  }
  void effectsAddAdjustRemove() {
    Project p;
    fillProject(p);
    Inspector i(p);
    p.setSelection({V1});
    QVERIFY(i.addEffect(QStringLiteral("saturation")));
    QCOMPARE(itemOf(p, V1).effects.size(), size_t(1));
    const QString id = itemOf(p, V1).effects[0].id;
    QCOMPARE(i.effects().size(), 1);
    for (double v : {1.0, 0.5, 0.2}) QVERIFY(i.setEffectParam(id, v, true));
    i.endDrag();
    QCOMPARE(std::get<double>(itemOf(p, V1).effects[0].params.at(QStringLiteral("amount"))), 0.2);
    QVERIFY(p.undo()); // the whole drag
    QCOMPARE(std::get<double>(itemOf(p, V1).effects[0].params.at(QStringLiteral("amount"))), 1.4);
    QVERIFY(i.removeEffect(id));
    QVERIFY(itemOf(p, V1).effects.empty());
    QVERIFY(!i.addEffect(QStringLiteral("nonsense")));
  }
  void multiSelectionEditsEveryClip() {
    Project p;
    fillProject(p);
    Inspector i(p);
    p.setSelection({V1, QStringLiteral("itm_v2")});
    QCOMPARE(i.count(), 2);
    QVERIFY(i.edit(QStringLiteral("opacity"), 0.4));
    QCOMPARE(itemOf(p, V1).transform.opacity, 0.4);
    QCOMPARE(itemOf(p, QStringLiteral("itm_v2")).transform.opacity, 0.4);
    QVERIFY(p.undo()); // one step for both
    QCOMPARE(itemOf(p, QStringLiteral("itm_v2")).transform.opacity, 1.0);
  }
  void lockedTrackRejectsInspectorEdits() {
    Project p;
    fillProject(p);
    TrackUpdate lock;
    lock.trackId = trackNamed(p.doc(), TrackKind::Video).id;
    lock.patch.locked = true;
    QVERIFY(p.apply(lock));
    Inspector i(p);
    p.setSelection({V1});
    QSignalSpy rejected(&p, &Project::editRejected);
    QVERIFY(!i.edit(QStringLiteral("opacity"), 0.1));
    QCOMPARE(rejected.count(), 1);
    QCOMPARE(itemOf(p, V1).transform.opacity, 1.0);
  }
  void projectSettings() {
    Project p;
    fillProject(p);
    Inspector i(p);
    QCOMPARE(i.projectValues().value(QStringLiteral("width")).toInt(), 1920);
    QVERIFY(i.editProject(QStringLiteral("name"), QStringLiteral("Renamed")));
    QCOMPARE(p.name(), QStringLiteral("Renamed"));
    QVERIFY(i.editProject(QStringLiteral("width"), 1080));
    QVERIFY(i.editProject(QStringLiteral("height"), 1920));
    QCOMPARE(p.doc().project.width, std::int64_t(1080));
    QCOMPARE(p.doc().project.height, std::int64_t(1920));
    QVERIFY(i.editProject(QStringLiteral("background"), QStringLiteral("#112233")));
    QCOMPARE(p.doc().project.styleConfig.backgroundColor, QStringLiteral("#112233"));
    // frame rate change rescales clip times: 30 -> 60 doubles every frame count
    QVERIFY(i.editProject(QStringLiteral("fps"), 60));
    QCOMPARE(itemOf(p, QStringLiteral("itm_v2")).startFrame, Frame(240));
    QVERIFY(p.undo());
    QCOMPARE(p.doc().project.fps, std::int64_t(30));
    QCOMPARE(itemOf(p, QStringLiteral("itm_v2")).startFrame, Frame(120));
    QVERIFY(!i.editProject(QStringLiteral("width"), 1080)); // unchanged: no op
  }
};

} // namespace tst
using tst::TstInspector;
QTEST_GUILESS_MAIN(TstInspector)
#include "tst_inspector.moc"
