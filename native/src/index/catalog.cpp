#include "index/catalog.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlQuery>
#include <QSqlError>
#include <QUuid>

namespace sf::index {

namespace {

QString sanitizeFtsQuery(const QString& query) {
  // Strip characters that break FTS5 grammar, format words for prefix/exact matching
  QString trimmed = query.trimmed();
  if (trimmed.isEmpty()) return {};

  QString cleaned;
  for (const QChar& c : trimmed) {
    if (c.isLetterOrNumber() || c.isSpace()) {
      cleaned.append(c);
    } else {
      cleaned.append(QLatin1Char(' '));
    }
  }

  const QStringList tokens = cleaned.split(QLatin1Char(' '), Qt::SkipEmptyParts);
  if (tokens.isEmpty()) return {};

  QStringList quoted;
  for (const QString& t : tokens) {
    quoted.append(QStringLiteral("\"%1\"*").arg(t));
  }
  return quoted.join(QStringLiteral(" AND "));
}

} // namespace

Catalog::Catalog() {
  connectionName_ = QStringLiteral("sf_index_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

Catalog::~Catalog() {
  close();
}

bool Catalog::open(const QString& dbPath) {
  close();

  if (dbPath != QStringLiteral(":memory:")) {
    QFileInfo fi(dbPath);
    QDir().mkpath(fi.absolutePath());
  }

  QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
  database.setDatabaseName(dbPath);
  if (!database.open()) {
    QSqlDatabase::removeDatabase(connectionName_);
    return false;
  }

  open_ = true;
  return initSchema();
}

void Catalog::close() {
  if (open_) {
    {
      QSqlDatabase database = QSqlDatabase::database(connectionName_);
      if (database.isOpen()) {
        database.close();
      }
    }
    QSqlDatabase::removeDatabase(connectionName_);
    open_ = false;
  }
}

bool Catalog::isOpen() const {
  return open_ && QSqlDatabase::contains(connectionName_) && QSqlDatabase::database(connectionName_).isOpen();
}

QSqlDatabase Catalog::db() const {
  return QSqlDatabase::database(connectionName_);
}

bool Catalog::initSchema() {
  if (!isOpen()) return false;

  QSqlQuery q(db());
  
  // Enable WAL mode for high concurrency
  q.exec(QStringLiteral("PRAGMA journal_mode = WAL;"));
  q.exec(QStringLiteral("PRAGMA synchronous = NORMAL;"));

  const char* schema = R"(
    CREATE TABLE IF NOT EXISTS sf_assets (
      id TEXT PRIMARY KEY,
      path TEXT,
      width INTEGER,
      height INTEGER,
      fps_num INTEGER,
      fps_den INTEGER,
      duration_frames INTEGER,
      sample_rate INTEGER,
      channels INTEGER,
      created_at INTEGER
    );

    CREATE TABLE IF NOT EXISTS sf_scenes (
      id TEXT PRIMARY KEY,
      asset_id TEXT,
      start_frame INTEGER,
      end_frame INTEGER,
      start_sec REAL,
      end_sec REAL,
      sharpness REAL,
      luma_avg REAL,
      luma_clipped_pct INTEGER,
      is_freeze INTEGER,
      is_black INTEGER,
      motion_score REAL,
      thumbnail_path TEXT
    );

    CREATE TABLE IF NOT EXISTS sf_speech_spans (
      id TEXT PRIMARY KEY,
      asset_id TEXT,
      start_frame INTEGER,
      end_frame INTEGER,
      start_sec REAL,
      end_sec REAL,
      is_speech INTEGER,
      mean_volume_db REAL,
      max_volume_db REAL
    );

    CREATE TABLE IF NOT EXISTS sf_transcripts (
      id TEXT PRIMARY KEY,
      asset_id TEXT,
      start_ms INTEGER,
      end_ms INTEGER,
      text TEXT,
      speaker TEXT,
      confidence REAL
    );

    CREATE TABLE IF NOT EXISTS sf_ocr_records (
      id TEXT PRIMARY KEY,
      asset_id TEXT,
      frame INTEGER,
      time_sec REAL,
      text TEXT,
      x REAL,
      y REAL,
      w REAL,
      h REAL,
      confidence REAL
    );

    CREATE TABLE IF NOT EXISTS sf_vlm_tags (
      id TEXT PRIMARY KEY,
      asset_id TEXT,
      scene_id TEXT,
      summary TEXT,
      shot_type TEXT,
      subject TEXT
    );

    CREATE VIRTUAL TABLE IF NOT EXISTS sf_fts USING fts5(
      asset_id UNINDEXED,
      item_type UNINDEXED,
      ref_id UNINDEXED,
      time_sec UNINDEXED,
      frame UNINDEXED,
      content,
      tokenize='porter unicode61'
    );
  )";

  const QStringList statements = QString::fromUtf8(schema).split(QLatin1Char(';'), Qt::SkipEmptyParts);
  for (const QString& stmt : statements) {
    const QString trimmed = stmt.trimmed();
    if (!trimmed.isEmpty()) {
      if (!q.exec(trimmed)) {
        return false;
      }
    }
  }

  return true;
}

bool Catalog::insertAsset(const QString& id, const QString& path, int width, int height,
                          int fpsNum, int fpsDen, qint64 durationFrames,
                          int sampleRate, int channels) {
  if (!isOpen()) return false;

  QSqlQuery q(db());
  q.prepare(QStringLiteral(
    "INSERT OR REPLACE INTO sf_assets (id, path, width, height, fps_num, fps_den, duration_frames, sample_rate, channels, created_at) "
    "VALUES (:id, :path, :width, :height, :fps_num, :fps_den, :duration_frames, :sample_rate, :channels, :created_at);"
  ));
  q.bindValue(QStringLiteral(":id"), id);
  q.bindValue(QStringLiteral(":path"), path);
  q.bindValue(QStringLiteral(":width"), width);
  q.bindValue(QStringLiteral(":height"), height);
  q.bindValue(QStringLiteral(":fps_num"), fpsNum);
  q.bindValue(QStringLiteral(":fps_den"), fpsDen);
  q.bindValue(QStringLiteral(":duration_frames"), durationFrames);
  q.bindValue(QStringLiteral(":sample_rate"), sampleRate);
  q.bindValue(QStringLiteral(":channels"), channels);
  q.bindValue(QStringLiteral(":created_at"), QDateTime::currentSecsSinceEpoch());

  return q.exec();
}

bool Catalog::insertScene(const SceneRecord& scene) {
  return insertScenes({scene});
}

bool Catalog::insertScenes(const std::vector<SceneRecord>& scenes) {
  if (!isOpen() || scenes.empty()) return false;

  QSqlDatabase database = db();
  database.transaction();

  QSqlQuery q(database);
  q.prepare(QStringLiteral(
    "INSERT OR REPLACE INTO sf_scenes "
    "(id, asset_id, start_frame, end_frame, start_sec, end_sec, sharpness, luma_avg, luma_clipped_pct, is_freeze, is_black, motion_score, thumbnail_path) "
    "VALUES (:id, :asset_id, :start_frame, :end_frame, :start_sec, :end_sec, :sharpness, :luma_avg, :luma_clipped_pct, :is_freeze, :is_black, :motion_score, :thumbnail_path);"
  ));

  for (const auto& s : scenes) {
    q.bindValue(QStringLiteral(":id"), s.id);
    q.bindValue(QStringLiteral(":asset_id"), s.assetId);
    q.bindValue(QStringLiteral(":start_frame"), s.startFrame);
    q.bindValue(QStringLiteral(":end_frame"), s.endFrame);
    q.bindValue(QStringLiteral(":start_sec"), s.startSec);
    q.bindValue(QStringLiteral(":end_sec"), s.endSec);
    q.bindValue(QStringLiteral(":sharpness"), s.sharpness);
    q.bindValue(QStringLiteral(":luma_avg"), s.lumaAvg);
    q.bindValue(QStringLiteral(":luma_clipped_pct"), s.lumaClippedPct);
    q.bindValue(QStringLiteral(":is_freeze"), s.isFreeze ? 1 : 0);
    q.bindValue(QStringLiteral(":is_black"), s.isBlack ? 1 : 0);
    q.bindValue(QStringLiteral(":motion_score"), s.motionScore);
    q.bindValue(QStringLiteral(":thumbnail_path"), s.thumbnailPath);
    if (!q.exec()) {
      database.rollback();
      return false;
    }
  }

  database.commit();
  return true;
}

std::vector<SceneRecord> Catalog::scenes(const QString& assetId) const {
  std::vector<SceneRecord> res;
  if (!isOpen()) return res;

  QSqlQuery q(db());
  q.prepare(QStringLiteral(
    "SELECT id, asset_id, start_frame, end_frame, start_sec, end_sec, sharpness, luma_avg, luma_clipped_pct, is_freeze, is_black, motion_score, thumbnail_path "
    "FROM sf_scenes WHERE asset_id = :asset_id ORDER BY start_frame ASC;"
  ));
  q.bindValue(QStringLiteral(":asset_id"), assetId);

  if (q.exec()) {
    while (q.next()) {
      SceneRecord r;
      r.id = q.value(0).toString();
      r.assetId = q.value(1).toString();
      r.startFrame = q.value(2).toLongLong();
      r.endFrame = q.value(3).toLongLong();
      r.startSec = q.value(4).toDouble();
      r.endSec = q.value(5).toDouble();
      r.sharpness = q.value(6).toDouble();
      r.lumaAvg = q.value(7).toDouble();
      r.lumaClippedPct = q.value(8).toInt();
      r.isFreeze = q.value(9).toInt() != 0;
      r.isBlack = q.value(10).toInt() != 0;
      r.motionScore = q.value(11).toDouble();
      r.thumbnailPath = q.value(12).toString();
      res.push_back(r);
    }
  }
  return res;
}

bool Catalog::insertSpeechSpan(const SpeechSpan& span) {
  return insertSpeechSpans({span});
}

bool Catalog::insertSpeechSpans(const std::vector<SpeechSpan>& spans) {
  if (!isOpen() || spans.empty()) return false;

  QSqlDatabase database = db();
  database.transaction();

  QSqlQuery q(database);
  q.prepare(QStringLiteral(
    "INSERT OR REPLACE INTO sf_speech_spans "
    "(id, asset_id, start_frame, end_frame, start_sec, end_sec, is_speech, mean_volume_db, max_volume_db) "
    "VALUES (:id, :asset_id, :start_frame, :end_frame, :start_sec, :end_sec, :is_speech, :mean_volume_db, :max_volume_db);"
  ));

  for (const auto& s : spans) {
    q.bindValue(QStringLiteral(":id"), s.id);
    q.bindValue(QStringLiteral(":asset_id"), s.assetId);
    q.bindValue(QStringLiteral(":start_frame"), s.startFrame);
    q.bindValue(QStringLiteral(":end_frame"), s.endFrame);
    q.bindValue(QStringLiteral(":start_sec"), s.startSec);
    q.bindValue(QStringLiteral(":end_sec"), s.endSec);
    q.bindValue(QStringLiteral(":is_speech"), s.isSpeech ? 1 : 0);
    q.bindValue(QStringLiteral(":mean_volume_db"), s.meanVolumeDb);
    q.bindValue(QStringLiteral(":max_volume_db"), s.maxVolumeDb);
    if (!q.exec()) {
      database.rollback();
      return false;
    }
  }

  database.commit();
  return true;
}

std::vector<SpeechSpan> Catalog::speechSpans(const QString& assetId) const {
  std::vector<SpeechSpan> res;
  if (!isOpen()) return res;

  QSqlQuery q(db());
  q.prepare(QStringLiteral(
    "SELECT id, asset_id, start_frame, end_frame, start_sec, end_sec, is_speech, mean_volume_db, max_volume_db "
    "FROM sf_speech_spans WHERE asset_id = :asset_id ORDER BY start_frame ASC;"
  ));
  q.bindValue(QStringLiteral(":asset_id"), assetId);

  if (q.exec()) {
    while (q.next()) {
      SpeechSpan r;
      r.id = q.value(0).toString();
      r.assetId = q.value(1).toString();
      r.startFrame = q.value(2).toLongLong();
      r.endFrame = q.value(3).toLongLong();
      r.startSec = q.value(4).toDouble();
      r.endSec = q.value(5).toDouble();
      r.isSpeech = q.value(6).toInt() != 0;
      r.meanVolumeDb = q.value(7).toDouble();
      r.maxVolumeDb = q.value(8).toDouble();
      res.push_back(r);
    }
  }
  return res;
}

bool Catalog::insertTranscript(const TranscriptRecord& tr) {
  return insertTranscripts({tr});
}

bool Catalog::insertTranscripts(const std::vector<TranscriptRecord>& trs) {
  if (!isOpen() || trs.empty()) return false;

  QSqlDatabase database = db();
  database.transaction();

  QSqlQuery q(database);
  q.prepare(QStringLiteral(
    "INSERT OR REPLACE INTO sf_transcripts (id, asset_id, start_ms, end_ms, text, speaker, confidence) "
    "VALUES (:id, :asset_id, :start_ms, :end_ms, :text, :speaker, :confidence);"
  ));

  QSqlQuery ftsQ(database);
  ftsQ.prepare(QStringLiteral(
    "INSERT INTO sf_fts (asset_id, item_type, ref_id, time_sec, frame, content) "
    "VALUES (:asset_id, 'transcript', :ref_id, :time_sec, :frame, :content);"
  ));

  for (const auto& t : trs) {
    q.bindValue(QStringLiteral(":id"), t.id);
    q.bindValue(QStringLiteral(":asset_id"), t.assetId);
    q.bindValue(QStringLiteral(":start_ms"), t.startMs);
    q.bindValue(QStringLiteral(":end_ms"), t.endMs);
    q.bindValue(QStringLiteral(":text"), t.text);
    q.bindValue(QStringLiteral(":speaker"), t.speaker);
    q.bindValue(QStringLiteral(":confidence"), t.confidence);
    if (!q.exec()) {
      database.rollback();
      return false;
    }

    ftsQ.bindValue(QStringLiteral(":asset_id"), t.assetId);
    ftsQ.bindValue(QStringLiteral(":ref_id"), t.id);
    ftsQ.bindValue(QStringLiteral(":time_sec"), t.startMs / 1000.0);
    ftsQ.bindValue(QStringLiteral(":frame"), 0);
    ftsQ.bindValue(QStringLiteral(":content"), t.text);
    if (!ftsQ.exec()) {
      database.rollback();
      return false;
    }
  }

  database.commit();
  return true;
}

std::vector<TranscriptRecord> Catalog::transcripts(const QString& assetId) const {
  std::vector<TranscriptRecord> res;
  if (!isOpen()) return res;

  QSqlQuery q(db());
  q.prepare(QStringLiteral(
    "SELECT id, asset_id, start_ms, end_ms, text, speaker, confidence "
    "FROM sf_transcripts WHERE asset_id = :asset_id ORDER BY start_ms ASC;"
  ));
  q.bindValue(QStringLiteral(":asset_id"), assetId);

  if (q.exec()) {
    while (q.next()) {
      TranscriptRecord r;
      r.id = q.value(0).toString();
      r.assetId = q.value(1).toString();
      r.startMs = q.value(2).toLongLong();
      r.endMs = q.value(3).toLongLong();
      r.text = q.value(4).toString();
      r.speaker = q.value(5).toString();
      r.confidence = q.value(6).toDouble();
      res.push_back(r);
    }
  }
  return res;
}

bool Catalog::insertOcrRecord(const OcrRecord& ocr) {
  return insertOcrRecords({ocr});
}

bool Catalog::insertOcrRecords(const std::vector<OcrRecord>& records) {
  if (!isOpen() || records.empty()) return false;

  QSqlDatabase database = db();
  database.transaction();

  QSqlQuery q(database);
  q.prepare(QStringLiteral(
    "INSERT OR REPLACE INTO sf_ocr_records (id, asset_id, frame, time_sec, text, x, y, w, h, confidence) "
    "VALUES (:id, :asset_id, :frame, :time_sec, :text, :x, :y, :w, :h, :confidence);"
  ));

  QSqlQuery ftsQ(database);
  ftsQ.prepare(QStringLiteral(
    "INSERT INTO sf_fts (asset_id, item_type, ref_id, time_sec, frame, content) "
    "VALUES (:asset_id, 'ocr', :ref_id, :time_sec, :frame, :content);"
  ));

  for (const auto& r : records) {
    q.bindValue(QStringLiteral(":id"), r.id);
    q.bindValue(QStringLiteral(":asset_id"), r.assetId);
    q.bindValue(QStringLiteral(":frame"), r.frame);
    q.bindValue(QStringLiteral(":time_sec"), r.timeSec);
    q.bindValue(QStringLiteral(":text"), r.text);
    q.bindValue(QStringLiteral(":x"), r.x);
    q.bindValue(QStringLiteral(":y"), r.y);
    q.bindValue(QStringLiteral(":w"), r.w);
    q.bindValue(QStringLiteral(":h"), r.h);
    q.bindValue(QStringLiteral(":confidence"), r.confidence);
    if (!q.exec()) {
      database.rollback();
      return false;
    }

    ftsQ.bindValue(QStringLiteral(":asset_id"), r.assetId);
    ftsQ.bindValue(QStringLiteral(":ref_id"), r.id);
    ftsQ.bindValue(QStringLiteral(":time_sec"), r.timeSec);
    ftsQ.bindValue(QStringLiteral(":frame"), r.frame);
    ftsQ.bindValue(QStringLiteral(":content"), r.text);
    if (!ftsQ.exec()) {
      database.rollback();
      return false;
    }
  }

  database.commit();
  return true;
}

std::vector<OcrRecord> Catalog::ocrRecords(const QString& assetId, qint64 frame) const {
  std::vector<OcrRecord> res;
  if (!isOpen()) return res;

  QSqlQuery q(db());
  if (frame >= 0) {
    q.prepare(QStringLiteral(
      "SELECT id, asset_id, frame, time_sec, text, x, y, w, h, confidence "
      "FROM sf_ocr_records WHERE asset_id = :asset_id AND frame = :frame ORDER BY y ASC, x ASC;"
    ));
    q.bindValue(QStringLiteral(":frame"), frame);
  } else {
    q.prepare(QStringLiteral(
      "SELECT id, asset_id, frame, time_sec, text, x, y, w, h, confidence "
      "FROM sf_ocr_records WHERE asset_id = :asset_id ORDER BY frame ASC, y ASC, x ASC;"
    ));
  }
  q.bindValue(QStringLiteral(":asset_id"), assetId);

  if (q.exec()) {
    while (q.next()) {
      OcrRecord r;
      r.id = q.value(0).toString();
      r.assetId = q.value(1).toString();
      r.frame = q.value(2).toLongLong();
      r.timeSec = q.value(3).toDouble();
      r.text = q.value(4).toString();
      r.x = q.value(5).toDouble();
      r.y = q.value(6).toDouble();
      r.w = q.value(7).toDouble();
      r.h = q.value(8).toDouble();
      r.confidence = q.value(9).toDouble();
      res.push_back(r);
    }
  }
  return res;
}

bool Catalog::insertVlmTag(const VlmTag& tag) {
  if (!isOpen()) return false;

  QSqlDatabase database = db();
  database.transaction();

  QSqlQuery q(database);
  q.prepare(QStringLiteral(
    "INSERT OR REPLACE INTO sf_vlm_tags (id, asset_id, scene_id, summary, shot_type, subject) "
    "VALUES (:id, :asset_id, :scene_id, :summary, :shot_type, :subject);"
  ));
  q.bindValue(QStringLiteral(":id"), tag.id);
  q.bindValue(QStringLiteral(":asset_id"), tag.assetId);
  q.bindValue(QStringLiteral(":scene_id"), tag.sceneId);
  q.bindValue(QStringLiteral(":summary"), tag.summary);
  q.bindValue(QStringLiteral(":shot_type"), tag.shotType);
  q.bindValue(QStringLiteral(":subject"), tag.subject);
  if (!q.exec()) {
    database.rollback();
    return false;
  }

  // Combine summary and subject into FTS content
  const QString content = QStringLiteral("%1 %2 %3").arg(tag.summary, tag.shotType, tag.subject);
  QSqlQuery ftsQ(database);
  ftsQ.prepare(QStringLiteral(
    "INSERT INTO sf_fts (asset_id, item_type, ref_id, time_sec, frame, content) "
    "VALUES (:asset_id, 'vlm', :ref_id, 0.0, 0, :content);"
  ));
  ftsQ.bindValue(QStringLiteral(":asset_id"), tag.assetId);
  ftsQ.bindValue(QStringLiteral(":ref_id"), tag.id);
  ftsQ.bindValue(QStringLiteral(":content"), content);
  if (!ftsQ.exec()) {
    database.rollback();
    return false;
  }

  database.commit();
  return true;
}

std::vector<VlmTag> Catalog::vlmTags(const QString& assetId) const {
  std::vector<VlmTag> res;
  if (!isOpen()) return res;

  QSqlQuery q(db());
  q.prepare(QStringLiteral(
    "SELECT id, asset_id, scene_id, summary, shot_type, subject "
    "FROM sf_vlm_tags WHERE asset_id = :asset_id;"
  ));
  q.bindValue(QStringLiteral(":asset_id"), assetId);

  if (q.exec()) {
    while (q.next()) {
      VlmTag r;
      r.id = q.value(0).toString();
      r.assetId = q.value(1).toString();
      r.sceneId = q.value(2).toString();
      r.summary = q.value(3).toString();
      r.shotType = q.value(4).toString();
      r.subject = q.value(5).toString();
      res.push_back(r);
    }
  }
  return res;
}

std::vector<SearchResult> Catalog::search(const QString& query, int limit) const {
  std::vector<SearchResult> res;
  if (!isOpen()) return res;

  const QString sanitized = sanitizeFtsQuery(query);
  if (sanitized.isEmpty()) return res;

  QSqlQuery q(db());
  q.prepare(QStringLiteral(
    "SELECT asset_id, item_type, ref_id, time_sec, frame, snippet(sf_fts, 5, '<b>', '</b>', '...', 16), rank "
    "FROM sf_fts WHERE sf_fts MATCH :query ORDER BY rank ASC LIMIT :limit;"
  ));
  q.bindValue(QStringLiteral(":query"), sanitized);
  q.bindValue(QStringLiteral(":limit"), limit <= 0 ? 20 : limit);

  if (q.exec()) {
    while (q.next()) {
      SearchResult r;
      r.assetId = q.value(0).toString();
      r.itemType = q.value(1).toString();
      r.refId = q.value(2).toString();
      r.timeSec = q.value(3).toDouble();
      r.frame = q.value(4).toLongLong();
      r.snippet = q.value(5).toString();
      r.rank = q.value(6).toDouble();
      res.push_back(r);
    }
  }
  return res;
}

bool Catalog::clearAsset(const QString& assetId) {
  if (!isOpen()) return false;

  QSqlDatabase database = db();
  database.transaction();

  QSqlQuery q(database);
  q.prepare(QStringLiteral("DELETE FROM sf_assets WHERE id = :id;"));
  q.bindValue(QStringLiteral(":id"), assetId);
  if (!q.exec()) {
    database.rollback();
    return false;
  }

  for (const QString& table : {QStringLiteral("sf_scenes"),
                               QStringLiteral("sf_speech_spans"), QStringLiteral("sf_transcripts"),
                               QStringLiteral("sf_ocr_records"), QStringLiteral("sf_vlm_tags"),
                               QStringLiteral("sf_fts")}) {
    q.prepare(QStringLiteral("DELETE FROM %1 WHERE asset_id = :asset_id;").arg(table));
    q.bindValue(QStringLiteral(":asset_id"), assetId);
    if (!q.exec()) {
      database.rollback();
      return false;
    }
  }

  database.commit();
  return true;
}

} // namespace sf::index
