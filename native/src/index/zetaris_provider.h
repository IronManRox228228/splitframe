#pragma once

// Zetaris federated media catalog provider.
// Enables SplitFrame to discover and query footage, B-roll, transcripts, and scene logs
// federated across heterogeneous storage pools (local NVMe, studio NAS, cloud S3/GCS)
// through unified metadata virtualization.

#include "core/schema.h"
#include "editor/project.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <vector>

namespace sf::index {

enum class FederatedStorageType {
  Local,
  StudioNas,
  CloudS3,
  CloudGcs,
  Unknown
};

inline QString storageTypeToString(FederatedStorageType t) {
  switch (t) {
    case FederatedStorageType::Local: return QStringLiteral("local");
    case FederatedStorageType::StudioNas: return QStringLiteral("studio_nas");
    case FederatedStorageType::CloudS3: return QStringLiteral("cloud_s3");
    case FederatedStorageType::CloudGcs: return QStringLiteral("cloud_gcs");
    case FederatedStorageType::Unknown: return QStringLiteral("unknown");
  }
  return QStringLiteral("unknown");
}

inline FederatedStorageType storageTypeFromString(const QString& s) {
  const QString lower = s.toLower();
  if (lower == QStringLiteral("local")) return FederatedStorageType::Local;
  if (lower == QStringLiteral("studio_nas") || lower == QStringLiteral("nas") || lower == QStringLiteral("smb") || lower == QStringLiteral("nfs")) {
    return FederatedStorageType::StudioNas;
  }
  if (lower == QStringLiteral("cloud_s3") || lower == QStringLiteral("s3")) return FederatedStorageType::CloudS3;
  if (lower == QStringLiteral("cloud_gcs") || lower == QStringLiteral("gcs")) return FederatedStorageType::CloudGcs;
  return FederatedStorageType::Unknown;
}

struct FederatedAsset {
  QString id;
  QString uri;             // e.g. "s3://broll-bucket/japan_sunset.mp4", "file:///D:/raw/scene1.mov"
  QString name;
  FederatedStorageType storageType = FederatedStorageType::Local;
  QString description;
  QStringList tags;
  QString transcriptSnippet;
  std::int64_t durationMs = 0;
  int width = 1920;
  int height = 1080;
  double fps = 30.0;
  float relevanceScore = 0.0f;
  std::int64_t fileSizeBytes = 0;

  QJsonObject toJson() const;
  static FederatedAsset fromJson(const QJsonObject& obj);
};

struct ZetarisQuery {
  QString naturalLanguage;
  QStringList filterTags;
  std::vector<FederatedStorageType> storageFilter;
  int minWidth = 0;
  int minHeight = 0;
  int limit = 50;
};

struct ZetarisTelemetry {
  int totalQueries = 0;
  int totalAssetsReturned = 0;
  qint64 lastLatencyMs = 0;
};

class ZetarisProvider {
public:
  explicit ZetarisProvider(QString apiKey = {},
                           QString endpoint = QStringLiteral("https://api.zetaris.com/v1/federation/query"),
                           QString catalogName = QStringLiteral("production_media_catalog"));
  virtual ~ZetarisProvider() = default;

  // Execute federated query across storage silos
  std::vector<FederatedAsset> query(const ZetarisQuery& q);

  // Ingest or reference a federated asset into the editor's project
  QString importToProject(const FederatedAsset& asset, editor::Project& project);

  const QString& apiKey() const { return apiKey_; }
  void setApiKey(QString key) { apiKey_ = std::move(key); }

  const QString& endpoint() const { return endpoint_; }
  void setEndpoint(QString ep) { endpoint_ = std::move(ep); }

  const QString& catalogName() const { return catalogName_; }
  void setCatalogName(QString name) { catalogName_ = std::move(name); }

  void setTimeoutMs(int ms) { timeoutMs_ = ms; }
  int timeoutMs() const { return timeoutMs_; }

  const ZetarisTelemetry& telemetry() const { return telemetry_; }
  void resetTelemetry() { telemetry_ = {}; }

  // Virtual catalog for testing and offline development
  void setMockAssets(std::vector<FederatedAsset> assets);
  void addMockAsset(FederatedAsset asset);
  bool isMockEnabled() const { return mockEnabled_; }
  void setMockEnabled(bool enabled) { mockEnabled_ = enabled; }

private:
  QString apiKey_;
  QString endpoint_;
  QString catalogName_;
  int timeoutMs_ = 30000;
  bool mockEnabled_ = false;
  std::vector<FederatedAsset> mockAssets_;
  ZetarisTelemetry telemetry_;

  std::vector<FederatedAsset> queryMock(const ZetarisQuery& q);
  std::vector<FederatedAsset> queryRemote(const ZetarisQuery& q);
};

} // namespace sf::index
