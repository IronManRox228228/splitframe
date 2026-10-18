#pragma once

#include "comfy/comfy_client.h"
#include "comfy/comfy_graph.h"
#include "comfy/comfy_process.h"
#include "comfy/comfy_schema.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace sf::engine {

class Engine;

class ComfyService {
public:
  comfy::Client client;
  std::optional<comfy::SchemaSet> schema; // cached object_info
  QString schemaSource;                  // URL or file path it came from
  std::unique_ptr<comfy::ComfyProcess> process;

  explicit ComfyService(Engine* engine = nullptr);

  comfy::ComfyServerState serverState() const {
    return process ? process->state() : comfy::ComfyServerState::None;
  }

  QJsonObject lastExit() const {
    return process ? process->lastExit() : QJsonObject{};
  }

  QStringList recentLogs(int maxLines = 100) const {
    return process ? process->recentLogs(maxLines) : QStringList{};
  }

  struct Doc {
    comfy::Graph graph;
    std::vector<comfy::Graph> undo;
    QString name;
    QJsonObject uploads;
    QJsonObject params;
    qint64 seed = 0;
  };
  std::map<QString, Doc> graphs; // graphId -> doc; ids "g1", "g2", ...
  int nextGraph = 1;
  std::map<QString, QJsonObject> lastReshoot; // itemId -> lastReshoot
};

} // namespace sf::engine
