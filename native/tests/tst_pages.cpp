// The Colour and Audio pages' models on a fixture project: the colour model writes item colour blocks through
// ops (undoable) and the project's colour management directly; the mixer model writes the mixer block through
// MixerSet ops and follows the track list. No window: the QML itself is exercised by tst_ui_smoke.

#include "app/colour_page.h"
#include "app/mixer_model.h"
#include "editor_testutil.h"

#include <QSignalSpy>

namespace tst {
using namespace sf;
using namespace sf::editor;
using namespace sf::test;
using sf::app::ColourPage;
using sf::app::MixerModel;
using sf::editor::Project; // not sf::Project

namespace {
const QString V1 = QStringLiteral("itm_v1"), V2 = QStringLiteral("itm_v2"), A1 = QStringLiteral("itm_a1");

QString trackId(const Project& p, TrackKind k) { return trackNamed(p.doc(), k).id; }
const MixNode* stripOf(const Project& p, const QString& id) {
  if (!p.doc().mixer) return nullptr;
  for (const MixNode& n : p.doc().mixer->strips)
    if (n.id == id) return &n;
  return nullptr;
}
} // namespace

class TstPages : public QObject {
  Q_OBJECT
private slots:
  // ---------------------------------------------------------------- colour
  void clipStripListsVideoClipsAndFollowsTheSelection() {
    Project p;
    fillProject(p);
    ColourPage col(p, nullptr, nullptr);
    QCOMPARE(col.clips().size(), 2);
    QCOMPARE(col.clipId(), V1);
    col.selectClip(V2);
    QCOMPARE(col.clipId(), V2);
    QCOMPARE(p.selection(), QStringList{V2});
    p.setSelection({A1}); // an audio item is not a clip: the page keeps its clip
    QCOMPARE(col.clipId(), V2);
    p.setSelection({V1});
    QCOMPARE(col.clipId(), V1);
  }

  void inputSpaceLutAndIntensityWriteTheItemColourBlockAndUndo() {
    Project p;
    fillProject(p);
    ColourPage col(p, nullptr, nullptr);
    QVERIFY(!itemOf(p, V1).color);

    QVERIFY(col.setInputSpace(QStringLiteral("ACEScg")));
    QCOMPARE(*itemOf(p, V1).color->inputSpace, QStringLiteral("ACEScg"));
    QCOMPARE(col.inputSpace(), QStringLiteral("ACEScg"));

    QVERIFY(col.setLut(QStringLiteral("file:///C:/luts/warm.cube")));
    QCOMPARE(*itemOf(p, V1).color->lutPath, QStringLiteral("C:/luts/warm.cube"));
    QCOMPARE(col.lutName(), QStringLiteral("warm.cube"));
    QVERIFY(col.hasColour());

    // a slider drag is one undo step
    QVERIFY(col.dragIntensity(0.6));
    QVERIFY(col.dragIntensity(0.3));
    col.endDrag();
    QCOMPARE(itemOf(p, V1).color->lutIntensity, 0.3);
    QCOMPARE(col.lutIntensity(), 0.3);

    QVERIFY(p.undo());
    QCOMPARE(itemOf(p, V1).color->lutIntensity, 1.0);
    QCOMPARE(*itemOf(p, V1).color->lutPath, QStringLiteral("C:/luts/warm.cube"));
    QVERIFY(p.undo()); // the LUT
    QVERIFY(!itemOf(p, V1).color->lutPath);
    QCOMPARE(*itemOf(p, V1).color->inputSpace, QStringLiteral("ACEScg"));
    QVERIFY(p.undo()); // the input space: the block is gone again
    QVERIFY(!itemOf(p, V1).color);
    QVERIFY(!col.hasColour());
    QVERIFY(p.redo());
    QCOMPARE(col.inputSpace(), QStringLiteral("ACEScg"));
  }

  void clearLutAndResetClip() {
    Project p;
    fillProject(p);
    ColourPage col(p, nullptr, nullptr);
    QVERIFY(col.setLut(QStringLiteral("C:/luts/a.cube")));
    QVERIFY(col.setInputSpace(QStringLiteral("ARRI LogC3 (EI800)")));
    QVERIFY(col.clearLut());
    QVERIFY(!itemOf(p, V1).color->lutPath);
    QCOMPARE(*itemOf(p, V1).color->inputSpace, QStringLiteral("ARRI LogC3 (EI800)"));
    QVERIFY(!col.clearLut()); // nothing to clear
    QVERIFY(col.resetClip());
    QVERIFY(!itemOf(p, V1).color);
    QVERIFY(p.undo());
    QVERIFY(itemOf(p, V1).color);
  }

  void editsTouchOnlyTheCurrentClip() {
    Project p;
    fillProject(p);
    ColourPage col(p, nullptr, nullptr);
    col.selectClip(V2);
    QVERIFY(col.setInputSpace(QStringLiteral("ACEScg")));
    QVERIFY(!itemOf(p, V1).color);
    QVERIFY(itemOf(p, V2).color);
  }

  void projectColourManagementIsWrittenDirectly() {
    Project p;
    fillProject(p);
    ColourPage col(p, nullptr, nullptr);
    QVERIFY(!p.doc().project.colorManagement);
    QCOMPARE(col.blendSpace(), QStringLiteral("linear"));
    col.setBlendSpace(QStringLiteral("display"));
    QCOMPARE(p.doc().project.colorManagement->blendSpace, BlendSpace::Display);
    col.setWorkingSpace(QStringLiteral("ACEScg"));
    col.setDisplayView(QStringLiteral("sRGB - Display/ACES 1.0 - SDR Video"));
    QCOMPARE(col.workingSpace(), QStringLiteral("ACEScg"));
    QCOMPARE(col.displayView(), QStringLiteral("sRGB - Display/ACES 1.0 - SDR Video"));
    QVERIFY(p.isDirty());
    // back to all defaults removes the block
    col.setBlendSpace(QStringLiteral("linear"));
    col.setWorkingSpace(QString());
    col.setDisplayView(QString());
    QVERIFY(!p.doc().project.colorManagement);
  }

  void optionsComeFromTheOcioConfig() {
    if (!sf::render::ColorManager::available()) QSKIP("built without OpenColorIO");
    Project p;
    fillProject(p);
    sf::app::Player player;
    ColourPage col(p, nullptr, &player);
    const QStringList working = col.workingSpaces();
    QCOMPARE(working.first(), QString()); // the built-in space is always offered
    QVERIFY2(working.contains(QStringLiteral("ACEScg")), qPrintable(working.join(QLatin1Char('|'))));
    QVERIFY(!working.contains(QStringLiteral("sRGB - Texture")));
    QVERIFY(col.inputSpaces().contains(QStringLiteral("ACEScg")));
    QVERIFY(!col.displayViews().isEmpty());
    QVERIFY(col.displayViews().first().contains(QLatin1Char('/')));
    // before / after goes to the player
    QVERIFY(!col.bypass());
    col.setBypass(true);
    QVERIFY(player.colourBypass());
    col.setBypass(false);
  }

  // ---------------------------------------------------------------- mixer
  void stripsAreTheAudioCapableTracksInOrder() {
    Project p;
    fillProject(p);
    MixerModel m(p);
    QCOMPARE(m.rowCount(), 2); // video + audio; the text track has no sound
    QCOMPARE(m.data(m.index(0), MixerModel::TrackIdRole).toString(), trackId(p, TrackKind::Video));
    QCOMPARE(m.data(m.index(1), MixerModel::TrackIdRole).toString(), trackId(p, TrackKind::Audio));
    QCOMPARE(m.data(m.index(1), MixerModel::GainDbRole).toDouble(), 0.0);
    QVERIFY(!p.doc().mixer); // reading creates nothing
  }

  void faderPanMuteAndSoloUpdateTheMixerBlockAndUndo() {
    Project p;
    fillProject(p);
    MixerModel m(p);
    const QString a = trackId(p, TrackKind::Audio);

    QVERIFY(m.edit(a, QStringLiteral("gainDb"), -6.0));
    QVERIFY(stripOf(p, a));
    QVERIFY(qAbs(stripOf(p, a)->volume - 0.5012) < 0.001);
    QVERIFY(qAbs(m.data(m.index(1), MixerModel::GainDbRole).toDouble() + 6.0) < 0.01);

    QVERIFY(m.edit(a, QStringLiteral("pan"), -0.5));
    QCOMPARE(stripOf(p, a)->pan, -0.5);
    QCOMPARE(m.data(m.index(1), MixerModel::PanRole).toDouble(), -0.5);

    QVERIFY(m.edit(a, QStringLiteral("muted"), true));
    QVERIFY(stripOf(p, a)->muted);
    QCOMPARE(m.data(m.index(1), MixerModel::MutedRole).toBool(), true);

    QVERIFY(m.edit(a, QStringLiteral("solo"), true));
    QVERIFY(stripOf(p, a)->solo);
    QVERIFY(m.anySolo());

    QVERIFY(p.undo()); // solo
    QVERIFY(!stripOf(p, a)->solo);
    QVERIFY(!m.anySolo());
    QVERIFY(p.undo()); // mute
    QVERIFY(!stripOf(p, a)->muted);
    QVERIFY(p.undo()); // pan
    QCOMPARE(stripOf(p, a)->pan, 0.0);
    QVERIFY(p.undo()); // fader: the block is gone again
    QVERIFY(!p.doc().mixer);
    QCOMPARE(m.data(m.index(1), MixerModel::GainDbRole).toDouble(), 0.0);
  }

  void aFaderDragIsOneUndoStep() {
    Project p;
    fillProject(p);
    MixerModel m(p);
    const QString v = trackId(p, TrackKind::Video);
    QVERIFY(m.drag(v, QStringLiteral("gainDb"), -3.0));
    QVERIFY(m.drag(v, QStringLiteral("gainDb"), -9.0));
    QVERIFY(m.drag(v, QStringLiteral("gainDb"), -12.0));
    m.endDrag();
    QVERIFY(qAbs(m.data(m.index(0), MixerModel::GainDbRole).toDouble() + 12.0) < 0.01);
    QVERIFY(p.undo());
    QVERIFY(!p.doc().mixer);
    QVERIFY(p.redo());
    QVERIFY(qAbs(stripOf(p, v)->volume - 0.2512) < 0.001);
  }

  void bottomOfTheFaderIsSilence() {
    Project p;
    fillProject(p);
    MixerModel m(p);
    const QString a = trackId(p, TrackKind::Audio);
    QVERIFY(m.edit(a, QStringLiteral("gainDb"), MixerModel::kFloorDb));
    QCOMPARE(stripOf(p, a)->volume, 0.0);
    QCOMPARE(m.data(m.index(1), MixerModel::GainDbRole).toDouble(), MixerModel::kFloorDb);
  }

  void masterIsEditableAndUnknownNodesAreRefused() {
    Project p;
    fillProject(p);
    MixerModel m(p);
    QVERIFY(m.edit(QStringLiteral("master"), QStringLiteral("gainDb"), -3.0));
    QVERIFY(qAbs(p.doc().mixer->master.volume - 0.7079) < 0.001);
    QVERIFY(qAbs(m.master().value(QStringLiteral("gainDb")).toDouble() + 3.0) < 0.01);
    QVERIFY(m.edit(QStringLiteral("master"), QStringLiteral("muted"), true));
    QVERIFY(m.master().value(QStringLiteral("muted")).toBool());
    QVERIFY(!m.edit(QStringLiteral("nope"), QStringLiteral("gainDb"), -3.0));
    QVERIFY(!m.edit(trackId(p, TrackKind::Text), QStringLiteral("gainDb"), -3.0));
    QVERIFY(!m.edit(trackId(p, TrackKind::Audio), QStringLiteral("gainDb"), 0.0)); // already there
  }

  void stripsFollowTheTrackList() {
    Project p;
    fillProject(p);
    MixerModel m(p);
    QSignalSpy reset(&m, &QAbstractItemModel::modelReset);
    QVERIFY(p.apply(Op(TrackAdd{QStringLiteral("trk_a2"), TrackKind::Audio, QStringLiteral("Voice"), false, false, false, std::nullopt}), QStringLiteral("add track")));
    QCOMPARE(m.rowCount(), 3);
    QCOMPARE(reset.count(), 1);
    QCOMPARE(m.data(m.index(2), MixerModel::TrackIdRole).toString(), QStringLiteral("trk_a2"));
    QCOMPARE(m.data(m.index(2), MixerModel::NameRole).toString(), QStringLiteral("Voice"));
    QVERIFY(m.edit(QStringLiteral("trk_a2"), QStringLiteral("gainDb"), -4.0)); // a new track is editable at once
    QVERIFY(stripOf(p, QStringLiteral("trk_a2")));
    QVERIFY(p.undo());
    QVERIFY(p.undo()); // and the track goes again
    QCOMPARE(m.rowCount(), 2);
    // a text track adds no strip
    QVERIFY(p.apply(Op(TrackAdd{QStringLiteral("trk_t2"), TrackKind::Text, QStringLiteral("Titles"), false, false, false, std::nullopt}), QStringLiteral("add text track")));
    QCOMPARE(m.rowCount(), 2);
  }

  void insertsShowUpAndBypassIsUndoable() {
    Project p;
    fillProject(p);
    MixerModel m(p);
    const QString a = trackId(p, TrackKind::Audio);
    Mixer mix;
    MixNode node{.id = a};
    node.inserts.push_back(MixInsert{.id = QStringLiteral("ins_1"), .type = QStringLiteral("eq")});
    mix.strips.push_back(node);
    QVERIFY(p.apply(Op(MixerSet{mix}), QStringLiteral("mixer.set")));
    const QVariantList ins = m.data(m.index(1), MixerModel::InsertsRole).toList();
    QCOMPARE(ins.size(), 1);
    QCOMPARE(ins[0].toMap().value(QStringLiteral("type")).toString(), QStringLiteral("eq"));
    QVERIFY(m.setInsertBypass(a, QStringLiteral("ins_1"), true));
    QVERIFY(stripOf(p, a)->inserts[0].bypass);
    QVERIFY(p.undo());
    QVERIFY(!stripOf(p, a)->inserts[0].bypass);
  }

  void silentTimelineMeasuresAsSilent() {
    Project p;
    fillProject(p); // its media files do not exist: the engine renders silence
    MixerModel m(p);
    QSignalSpy done(&m, &MixerModel::analysisChanged);
    m.analyze();
    QVERIFY(m.analysing());
    QVERIFY(done.wait(30000));
    QVERIFY(!m.analysing());
    QVERIFY(m.analysed());
    QVERIFY(m.integratedLufs() < -90); // below the reading floor: shown as an em dash
  }
};

} // namespace tst

QTEST_GUILESS_MAIN(tst::TstPages)
#include "tst_pages.moc"
