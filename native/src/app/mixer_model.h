#pragma once

// The Audio page's mixer: one strip per audio-capable track (video, audio and overlay tracks; text has no
// sound) plus the master, read from and written to the document's mixer block through MixerSet ops, so every
// fader move is undoable. Tracks without a strip in the block show the defaults (unity, centre) and get a
// strip when first edited. Live meters come from the preview audio engine while the player runs.
//
// Edits come in two flavours like the Inspector's: edit() is one undo step; drag() applies live while a
// fader moves and every value of the drag joins one undo group that endDrag() closes.

#include "app/player.h"
#include "editor/project.h"

#include <QAbstractListModel>
#include <QPointer>
#include <QQmlEngine>
#include <QTimer>
#include <QVariantList>

#include <vector>

class QThread;

namespace sf::app {

class MixerModel : public QAbstractListModel {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("created by the application")

  // gainDb, pan, muted, inserts and (while metering) peakL/peakR/holdL/holdR in dB
  Q_PROPERTY(QVariantMap master READ master NOTIFY masterChanged)
  Q_PROPERTY(bool anySolo READ anySolo NOTIFY anySoloChanged)
  // Poll the engine's meters (the Audio page is visible)
  Q_PROPERTY(bool metering READ metering WRITE setMetering NOTIFY meteringChanged)
  Q_PROPERTY(double momentaryLufs READ momentaryLufs NOTIFY metersChanged)
  Q_PROPERTY(double integratedLufs READ integratedLufs NOTIFY analysisChanged)
  Q_PROPERTY(double truePeakDb READ truePeakDb NOTIFY analysisChanged)
  Q_PROPERTY(bool analysed READ analysed NOTIFY analysisChanged)
  Q_PROPERTY(bool analysing READ analysing NOTIFY analysingChanged)
  Q_PROPERTY(double targetLufs READ targetLufs CONSTANT)

public:
  enum Role {
    TrackIdRole = Qt::UserRole + 1,
    NameRole,
    KindRole,
    GainDbRole,
    PanRole,
    MutedRole,
    SoloRole,
    InsertsRole,
    PeakLRole, // dB, floored at kFloorDb
    PeakRRole,
    HoldLRole,
    HoldRRole,
  };

  static constexpr double kFloorDb = -60; // the fader's bottom: 0 linear, shown as -inf
  static constexpr double kMaxDb = 12;    // the fader's top
  static constexpr double kTargetLufs = -14;

  explicit MixerModel(editor::Project& project, QObject* parent = nullptr);
  ~MixerModel() override;

  void setPlayer(Player* player);

  int rowCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  QHash<int, QByteArray> roleNames() const override;

  QVariantMap master() const { return master_; }
  bool anySolo() const { return anySolo_; }
  bool metering() const { return timer_.isActive(); }
  void setMetering(bool on);
  double momentaryLufs() const { return momentary_; }
  double integratedLufs() const { return integrated_; }
  double truePeakDb() const { return truePeak_; }
  bool analysed() const { return analysed_; }
  bool analysing() const { return analysing_; }
  double targetLufs() const { return kTargetLufs; }

  // nodeId: a track id or "master". key: gainDb, pan, muted, solo. False when nothing changed.
  Q_INVOKABLE bool edit(const QString& nodeId, const QString& key, const QVariant& value);
  Q_INVOKABLE bool drag(const QString& nodeId, const QString& key, const QVariant& value);
  Q_INVOKABLE void endDrag();
  Q_INVOKABLE bool setInsertBypass(const QString& nodeId, const QString& insertId, bool bypass);
  // Offline integrated loudness of the whole timeline (what audio.analyze measures), on a worker thread.
  Q_INVOKABLE void analyze();

  static double dbOf(double linear);
  static double linearOf(double db);

signals:
  void masterChanged();
  void anySoloChanged();
  void meteringChanged();
  void metersChanged();
  void analysisChanged();
  void analysingChanged();

private:
  struct Strip {
    QString id, name, kind;
    double volume = 1, pan = 0;
    bool muted = false, solo = false;
    QVariantList inserts;
    float peak[2] = {0, 0}, hold[2] = {0, 0}; // linear, from the meters
    bool sameSettings(const Strip& o) const {
      return id == o.id && name == o.name && kind == o.kind && volume == o.volume && pan == o.pan && muted == o.muted && solo == o.solo && inserts == o.inserts;
    }
  };

  void refresh();
  void tick();
  bool run(const QString& nodeId, const QString& key, const QVariant& value, bool live);
  static Strip stripFor(const Track& track, const MixNode* node);
  static QVariantList insertsOf(const MixNode& node);

  editor::Project& project_;
  QPointer<Player> player_;
  std::vector<Strip> strips_;
  QVariantMap master_;
  bool anySolo_ = false;
  bool dragging_ = false;
  bool wasPlaying_ = false;
  QTimer timer_;
  double momentary_ = kFloorDb - 40; // below the floor: no reading
  double integrated_ = kFloorDb - 40, truePeak_ = kFloorDb - 40;
  bool analysed_ = false, analysing_ = false;
  QThread* worker_ = nullptr;
};

} // namespace sf::app
