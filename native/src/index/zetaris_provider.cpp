#include "index/zetaris_provider.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <algorithm>

namespace sf::index {

QJsonObject FederatedAsset::toJson() const {
  QJsonArray tagArr;
  for (const QString& t : tags) tagArr.append(t);

  return QJsonObject{
    {QStringLiteral("id"), id},
    {QStringLiteral("uri"), uri},
    {QStringLiteral("name"), name},
    {QStringLiteral("storage_type"), storageTypeToString(storageType)},
    {QStringLiteral("description"), description},
    {QStringLiteral("tags"), tagArr},
    {QStringLiteral("transcript_snippet"), transcriptSnippet},
    {QStringLiteral("duration_ms"), static_cast<qint64>(durationMs)},
    {QStringLiteral("width"), width},
    {QStringLiteral("height"), height},
    {QStringLiteral("fps"), fps},
    {QStringLiteral("relevance_score"), static_cast<double>(relevanceScore)},
    {QStringLiteral("file_size_bytes"), static_cast<qint64>(fileSizeBytes)}
  };
}

FederatedAsset FederatedAsset::fromJson(const QJsonObject& obj) {
  FederatedAsset a;
  a.id = obj.value(QStringLiteral("id")).toString();
  a.uri = obj.value(QStringLiteral("uri")).toString();
  a.name = obj.value(QStringLiteral("name")).toString();
  a.storageType = storageTypeFromString(obj.value(QStringLiteral("storage_type")).toString());
  a.description = obj.value(QStringLiteral("description")).toString();
  
  const QJsonArray tagArr = obj.value(QStringLiteral("tags")).toArray();
  for (const QJsonValue& val : tagArr) {
    a.tags.append(val.toString());
  }

  a.transcriptSnippet = obj.value(QStringLiteral("transcript_snippet")).toString();
  a.durationMs = obj.value(QStringLiteral("duration_ms")).toVariant().toLongLong();
  a.width = obj.value(QStringLiteral("width")).toInt(1920);
  a.height = obj.value(QStringLiteral("height")).toInt(1080);
  a.fps = obj.value(QStringLiteral("fps")).toDouble(30.0);
  a.relevanceScore = static_cast<float>(obj.value(QStringLiteral("relevance_score")).toDouble(0.0));
  a.fileSizeBytes = obj.value(QStringLiteral("file_size_bytes")).toVariant().toLongLong();
  return a;
}

// ---- ZetarisProvider ----

ZetarisProvider::ZetarisProvider(QString apiKey, QString endpoint, QString catalogName)
  : apiKey_(std::move(apiKey)), endpoint_(std::move(endpoint)), catalogName_(std::move(catalogName)) {
  const auto env = QProcessEnvironment::systemEnvironment();
  if (apiKey_.isEmpty() && env.contains(QStringLiteral("ZETARIS_API_KEY"))) {
    apiKey_ = env.value(QStringLiteral("ZETARIS_API_KEY"));
  }
  if (env.contains(QStringLiteral("ZETARIS_ENDPOINT"))) {
    endpoint_ = env.value(QStringLiteral("ZETARIS_ENDPOINT"));
  }
  if (env.contains(QStringLiteral("ZETARIS_CATALOG"))) {
    catalogName_ = env.value(QStringLiteral("ZETARIS_CATALOG"));
  }
}

void ZetarisProvider::setMockAssets(std::vector<FederatedAsset> assets) {
  mockAssets_ = std::move(assets);
  mockEnabled_ = true;
}

void ZetarisProvider::addMockAsset(FederatedAsset asset) {
  mockAssets_.push_back(std::move(asset));
  mockEnabled_ = true;
}

std::vector<FederatedAsset> ZetarisProvider::query(const ZetarisQuery& q) {
  QElapsedTimer timer;
  timer.start();
  telemetry_.totalQueries++;

  std::vector<FederatedAsset> results;
  if (mockEnabled_ || apiKey_.isEmpty()) {
    results = queryMock(q);
  } else {
    results = queryRemote(q);
    if (results.empty() && !mockAssets_.empty()) {
      results = queryMock(q);
    }
  }

  telemetry_.lastLatencyMs = timer.elapsed();
  telemetry_.totalAssetsReturned += static_cast<int>(results.size());
  return results;
}

std::vector<FederatedAsset> ZetarisProvider::queryMock(const ZetarisQuery& q) {
  std::vector<FederatedAsset> matches;
  const QStringList queryTerms = q.naturalLanguage.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);

  for (const auto& a : mockAssets_) {
    // 1. Storage filter
    if (!q.storageFilter.empty()) {
      bool storageMatch = false;
      for (auto st : q.storageFilter) {
        if (a.storageType == st) {
          storageMatch = true;
          break;
        }
      }
      if (!storageMatch) continue;
    }

    // 2. Resolution filter
    if (q.minWidth > 0 && a.width < q.minWidth) continue;
    if (q.minHeight > 0 && a.height < q.minHeight) continue;

    // 3. Tag filter
    if (!q.filterTags.isEmpty()) {
      bool allTagsMatched = true;
      for (const QString& reqTag : q.filterTags) {
        bool tagFound = false;
        for (const QString& assetTag : a.tags) {
          if (assetTag.compare(reqTag, Qt::CaseInsensitive) == 0) {
            tagFound = true;
            break;
          }
        }
        if (!tagFound) {
          allTagsMatched = false;
          break;
        }
      }
      if (!allTagsMatched) continue;
    }

    // 4. Natural language relevance scoring
    float score = 1.0f;
    if (!queryTerms.isEmpty()) {
      score = 0.0f;
      for (const QString& term : queryTerms) {
        // Tag match
        for (const QString& tag : a.tags) {
          if (tag.toLower().contains(term)) score += 3.0f;
        }
        // Name match
        if (a.name.toLower().contains(term)) score += 2.0f;
        // Description match
        if (a.description.toLower().contains(term)) score += 1.5f;
        // Transcript snippet match
        if (a.transcriptSnippet.toLower().contains(term)) score += 1.0f;
      }
      if (score <= 0.0f) continue;
    }

    FederatedAsset matchedAsset = a;
    matchedAsset.relevanceScore = score;
    matches.push_back(matchedAsset);
  }

  // Sort descending by relevance score
  std::sort(matches.begin(), matches.end(), [](const FederatedAsset& l, const FederatedAsset& r) {
    return l.relevanceScore > r.relevanceScore;
  });

  if (q.limit > 0 && static_cast<int>(matches.size()) > q.limit) {
    matches.resize(static_cast<size_t>(q.limit));
  }

  return matches;
}

std::vector<FederatedAsset> ZetarisProvider::queryRemote(const ZetarisQuery& q) {
  QJsonObject req;
  req.insert(QStringLiteral("catalog"), catalogName_);
  req.insert(QStringLiteral("query"), q.naturalLanguage);

  QJsonArray tagArr;
  for (const QString& t : q.filterTags) tagArr.append(t);
  req.insert(QStringLiteral("tags"), tagArr);

  QJsonArray stArr;
  for (auto st : q.storageFilter) stArr.append(storageTypeToString(st));
  req.insert(QStringLiteral("storage_types"), stArr);

  req.insert(QStringLiteral("min_width"), q.minWidth);
  req.insert(QStringLiteral("min_height"), q.minHeight);
  req.insert(QStringLiteral("limit"), q.limit);

  QNetworkAccessManager manager;
  QNetworkRequest netReq;
  netReq.setUrl(QUrl(endpoint_));
  netReq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  netReq.setRawHeader("User-Agent", "SplitFrame-Zetaris/0.1.0");

  if (!apiKey_.isEmpty()) {
    netReq.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(apiKey_).toUtf8());
  }

  const QByteArray postData = QJsonDocument(req).toJson(QJsonDocument::Compact);
  QNetworkReply* reply = manager.post(netReq, postData);

  QEventLoop loop;
  QTimer timeoutTimer;
  timeoutTimer.setSingleShot(true);
  QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

  timeoutTimer.start(timeoutMs_);
  loop.exec();

  if (timeoutTimer.isActive()) {
    timeoutTimer.stop();
  } else {
    reply->abort();
    reply->deleteLater();
    return {};
  }

  if (reply->error() != QNetworkReply::NoError) {
    reply->deleteLater();
    return {};
  }

  const QByteArray respData = reply->readAll();
  reply->deleteLater();

  const QJsonDocument doc = QJsonDocument::fromJson(respData);
  if (!doc.isObject()) return {};

  const QJsonObject rootObj = doc.object();
  const QJsonArray assetArray = rootObj.value(QStringLiteral("assets")).toArray();
  std::vector<FederatedAsset> results;
  for (const QJsonValue& v : assetArray) {
    if (v.isObject()) {
      results.push_back(FederatedAsset::fromJson(v.toObject()));
    }
  }

  return results;
}

QString ZetarisProvider::importToProject(const FederatedAsset& asset, editor::Project& project) {
  Asset newAsset;
  newAsset.id = asset.id.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : asset.id;
  newAsset.path = asset.uri;
  newAsset.originalName = asset.name.isEmpty() ? newAsset.id : asset.name;
  newAsset.kind = AssetKind::Video;
  newAsset.status = AssetStatus::Analyzed;
  newAsset.durationMs = asset.durationMs;
  newAsset.width = asset.width;
  newAsset.height = asset.height;
  newAsset.fps = asset.fps;
  newAsset.sizeBytes = asset.fileSizeBytes;
  newAsset.metadata.location = storageTypeToString(asset.storageType);
  newAsset.metadata.capturedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

  project.addAsset(newAsset);
  return newAsset.id;
}

} // namespace sf::index
