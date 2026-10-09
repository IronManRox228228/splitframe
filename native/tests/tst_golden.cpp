#include "core/snap.h"
#include "json_compare.h"
#include "test_helpers.h"

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

// Golden parity with the TypeScript packages. The fixtures under tests/fixtures are produced by
// tests/fixtures/gen (zod + editor-core running over the same inputs); here the native port loads
// the same docs, applies the same ops and must produce identical JSON (compared parsed).
using namespace sf;
using namespace sf::test;

namespace {

QString fixture(const QString& rel) { return QDir(QStringLiteral(SF_FIXTURE_DIR)).filePath(rel); }

QStringList jsonFiles(const QString& dir, bool underscored) {
  const QDir d(fixture(dir));
  QStringList out;
  for (const QFileInfo& f : d.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name))
    if (f.fileName().startsWith(QLatin1Char('_')) == underscored) out << f.fileName();
  return out;
}

QString kindOf(const std::exception_ptr& p, QString* message) {
  try {
    std::rethrow_exception(p);
  } catch (const SchemaError&) {
    return QStringLiteral("ZodError");
  } catch (const OpError& e) {
    *message = QString::fromUtf8(e.what());
    return QStringLiteral("OpError");
  } catch (const std::exception& e) {
    *message = QString::fromUtf8(e.what());
    return QStringLiteral("Other");
  }
}

SnapOptions snapOptions(const QJsonObject& o) {
  SnapOptions s;
  if (o.contains(QStringLiteral("includeItemEdges"))) s.includeItemEdges = o.value(QStringLiteral("includeItemEdges")).toBool();
  if (o.contains(QStringLiteral("includeMarkers"))) s.includeMarkers = o.value(QStringLiteral("includeMarkers")).toBool();
  if (o.contains(QStringLiteral("includePlayhead"))) s.includePlayhead = o.value(QStringLiteral("includePlayhead")).toDouble();
  if (o.contains(QStringLiteral("gridFrames"))) s.gridFrames = o.value(QStringLiteral("gridFrames")).toDouble();
  for (const QJsonValue& v : o.value(QStringLiteral("excludeItemIds")).toArray()) s.excludeItemIds.push_back(v.toString());
  for (const QJsonValue& v : o.value(QStringLiteral("extra")).toArray())
    s.extra.push_back({v.toObject().value(QStringLiteral("frame")).toDouble(), SnapCandidate::Kind::Playhead, std::nullopt});
  return s;
}

QString kindName(SnapCandidate::Kind k) {
  switch (k) {
    case SnapCandidate::Kind::ItemEdge: return QStringLiteral("item-edge");
    case SnapCandidate::Kind::Marker: return QStringLiteral("marker");
    case SnapCandidate::Kind::Playhead: return QStringLiteral("playhead");
    case SnapCandidate::Kind::Grid: return QStringLiteral("grid");
    case SnapCandidate::Kind::Zero: return QStringLiteral("zero");
  }
  return {};
}

QJsonObject candidateJson(const SnapCandidate& c) {
  QJsonObject o{{QStringLiteral("frame"), c.frame}, {QStringLiteral("kind"), kindName(c.kind)}};
  if (c.source) o.insert(QStringLiteral("source"), *c.source);
  return o;
}

} // namespace

class TstGolden : public QObject {
  Q_OBJECT
private slots:
  // ---- docs: load applies the same defaults zod does, save writes the same shape ----
  void docs_data() {
    QTest::addColumn<QString>("file");
    for (const QString& f : jsonFiles(QStringLiteral("docs"), false)) QTest::newRow(qPrintable(f)) << f;
  }
  void docs() {
    QFETCH(QString, file);
    const QJsonObject fx = readJsonFile(fixture(QStringLiteral("docs/") + file)).toObject();
    const TimelineDoc doc = parseTimelineDoc(Rd(fx.value(QStringLiteral("input"))));
    const QString diff = compareJson(toJson(doc), fx.value(QStringLiteral("expected")));
    QVERIFY2(diff.isEmpty(), qPrintable(diff));
    // and through a real file
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("p.json"));
    saveTimelineDoc(path, doc);
    QVERIFY(loadTimelineDoc(path) == doc);
  }

  void rejectsTheSameDocsAtTheSamePath_data() {
    QTest::addColumn<QJsonValue>("input");
    QTest::addColumn<QString>("path");
    const QJsonArray all = readJsonFile(fixture(QStringLiteral("docs/_bad.json"))).toArray();
    QVERIFY(!all.isEmpty());
    for (const QJsonValue& v : all) {
      const QJsonObject o = v.toObject();
      QTest::newRow(qPrintable(o.value(QStringLiteral("name")).toString())) << o.value(QStringLiteral("input")) << o.value(QStringLiteral("path")).toString();
    }
  }
  void rejectsTheSameDocsAtTheSamePath() {
    QFETCH(QJsonValue, input);
    QFETCH(QString, path);
    try {
      parseTimelineDoc(Rd(input));
    } catch (const SchemaError& e) {
      QCOMPARE(e.path(), path);
      return;
    }
    QFAIL("expected SchemaError");
  }

  void projectBundle() {
    const QJsonObject fx = readJsonFile(fixture(QStringLiteral("docs/_bundle.json"))).toObject();
    const ProjectBundle b = parseProjectBundle(Rd(fx.value(QStringLiteral("input"))));
    const QString diff = compareJson(toJson(b), fx.value(QStringLiteral("expected")));
    QVERIFY2(diff.isEmpty(), qPrintable(diff));
  }

  // ---- op sequences ----
  void opSequences_data() {
    QTest::addColumn<QString>("file");
    for (const QString& f : jsonFiles(QStringLiteral("cases"), false)) QTest::newRow(qPrintable(f)) << f;
  }
  void opSequences() {
    QFETCH(QString, file);
    const QJsonObject fx = readJsonFile(fixture(QStringLiteral("cases/") + file)).toObject();
    TimelineDoc cur = parseTimelineDoc(Rd(fx.value(QStringLiteral("initial"))));
    QString diff = compareJson(toJson(cur), fx.value(QStringLiteral("initial")));
    QVERIFY2(diff.isEmpty(), qPrintable(QStringLiteral("initial doc: ") + diff));

    const QJsonArray steps = fx.value(QStringLiteral("steps")).toArray();
    QVERIFY(!steps.isEmpty());
    int applied = 0;
    int rejected = 0;
    for (qsizetype i = 0; i < steps.size(); ++i) {
      const QJsonObject step = steps.at(i).toObject();
      const QJsonObject opJson = step.value(QStringLiteral("op")).toObject();
      const QString where = QStringLiteral("%1 step %2 (%3)").arg(file).arg(i).arg(opJson.value(QStringLiteral("type")).toString());
      const bool expectOk = step.value(QStringLiteral("ok")).toBool();

      std::optional<ApplyResult> r;
      std::exception_ptr failure;
      try {
        r = applyOp(cur, parseOp(Rd(opJson))); // parse errors are zod errors on the TS side too
      } catch (...) {
        failure = std::current_exception();
      }

      if (!expectOk) {
        const QJsonObject err = step.value(QStringLiteral("error")).toObject();
        if (!failure) QFAIL(qPrintable(where + QStringLiteral(": expected a rejection, but the op applied")));
        QString message;
        const QString kind = kindOf(failure, &message);
        QVERIFY2(kind == err.value(QStringLiteral("kind")).toString(),
                 qPrintable(QStringLiteral("%1: error kind %2 (%3), expected %4").arg(where, kind, message, err.value(QStringLiteral("kind")).toString())));
        if (kind == QStringLiteral("OpError"))
          QCOMPARE(message, err.value(QStringLiteral("message")).toString());
        ++rejected;
        continue;
      }

      if (failure) {
        QString message;
        const QString kind = kindOf(failure, &message);
        QFAIL(qPrintable(QStringLiteral("%1: unexpected %2: %3").arg(where, kind, message)));
      }
      // the resulting doc
      diff = compareJson(toJson(r->doc), step.value(QStringLiteral("doc")));
      QVERIFY2(diff.isEmpty(), qPrintable(where + QStringLiteral(": doc: ") + diff));
      // the inverse ops, as a persisted log would see them
      QJsonArray inv;
      for (const Op& o : r->inverse) inv.append(toJson(o));
      diff = compareJson(inv, step.value(QStringLiteral("inverse")));
      QVERIFY2(diff.isEmpty(), qPrintable(where + QStringLiteral(": inverse: ") + diff));
      // undoing with our own inverse lands where undoing with theirs does
      const TimelineDoc undone = applyOps(r->doc, r->inverse, {.enforceLocks = false}).doc;
      diff = compareJson(toJson(undone), step.value(QStringLiteral("undone")));
      QVERIFY2(diff.isEmpty(), qPrintable(where + QStringLiteral(": undone: ") + diff));
      cur = r->doc;
      ++applied;
    }
    QVERIFY(applied > 0);
    qInfo().noquote() << file << ":" << applied << "applied," << rejected << "rejected as expected";
  }

  // ---- queries: source mapping, durations, ordering, snapping ----
  void queries() {
    const QJsonObject fx = readJsonFile(fixture(QStringLiteral("queries.json"))).toObject();
    const TimelineDoc doc = parseTimelineDoc(Rd(fx.value(QStringLiteral("doc"))));
    QCOMPARE_EQ(docDurationFrames(doc), static_cast<Frame>(fx.value(QStringLiteral("duration")).toDouble()));

    const QJsonObject onTrack = fx.value(QStringLiteral("itemsOnTrack")).toObject();
    for (const Track& t : doc.tracks) {
      QJsonArray ids;
      for (const Item* i : itemsOnTrack(doc, t.id)) ids.append(i->id);
      const QString diff = compareJson(ids, onTrack.value(t.id));
      QVERIFY2(diff.isEmpty(), qPrintable(t.id + QStringLiteral(": ") + diff));
    }

    QJsonArray frames = fx.value(QStringLiteral("frames")).toArray();
    for (const QJsonValue& m : fx.value(QStringLiteral("mapping")).toArray()) {
      const QJsonObject mo = m.toObject();
      const Item& item = requireItem(doc, mo.value(QStringLiteral("id")).toString());
      QJsonArray got;
      for (const QJsonValue& f : frames) got.append(static_cast<qint64>(sourceFrameAt(item, static_cast<Frame>(f.toDouble()))));
      QString diff = compareJson(got, mo.value(QStringLiteral("sourceFrames")));
      QVERIFY2(diff.isEmpty(), qPrintable(item.id + QStringLiteral(" sourceFrameAt ") + diff));
      QCOMPARE_EQ(sourceOutFrame(item), static_cast<Frame>(mo.value(QStringLiteral("sourceOut")).toDouble()));
      QCOMPARE_EQ(effectiveSpeed(item), mo.value(QStringLiteral("effectiveSpeed")).toDouble());
    }

    int checks = 0;
    for (const QJsonValue& sv : fx.value(QStringLiteral("snaps")).toArray()) {
      const QJsonObject so = sv.toObject();
      const SnapOptions opts = snapOptions(so.value(QStringLiteral("opts")).toObject());
      const auto candidates = getSnapCandidates(doc, opts);
      QJsonArray cj;
      for (const SnapCandidate& c : candidates) cj.append(candidateJson(c));
      QString diff = compareJson(cj, so.value(QStringLiteral("candidates")));
      QVERIFY2(diff.isEmpty(), qPrintable(QStringLiteral("candidates: ") + diff));
      for (const QJsonValue& rv : so.value(QStringLiteral("snapFrame")).toArray()) {
        const QJsonObject ro = rv.toObject();
        const auto res = snapFrame(ro.value(QStringLiteral("frame")).toDouble(), candidates, ro.value(QStringLiteral("threshold")).toDouble());
        const QJsonValue expected = ro.value(QStringLiteral("result"));
        if (expected.isNull()) {
          QVERIFY(!res.has_value());
        } else {
          QVERIFY(res.has_value());
          QJsonObject got{{QStringLiteral("frame"), res->frame}, {QStringLiteral("candidate"), candidateJson(res->candidate)}, {QStringLiteral("delta"), res->delta}};
          diff = compareJson(got, expected);
          QVERIFY2(diff.isEmpty(), qPrintable(QStringLiteral("snapFrame: ") + diff));
        }
        ++checks;
      }
      for (const QJsonValue& rv : so.value(QStringLiteral("snapItemStart")).toArray()) {
        const QJsonObject ro = rv.toObject();
        const double got = snapItemStart(doc, ro.value(QStringLiteral("itemId")).toString(), ro.value(QStringLiteral("proposed")).toDouble(),
                                         ro.value(QStringLiteral("threshold")).toDouble(), opts);
        QCOMPARE_EQ(got, ro.value(QStringLiteral("result")).toDouble());
        ++checks;
      }
    }
    QVERIFY(checks > 100);
  }
};

QTEST_APPLESS_MAIN(TstGolden)
#include "tst_golden.moc"
