#pragma once

// ComfyUI workflow generation background job analyzer.

#include "comfy/comfy_process.h"
#include "jobs/job_scheduler.h"

#include <functional>

namespace sf::comfy {

struct ServerStateProvider {
  std::function<ComfyServerState()> serverState;
  std::function<QJsonObject()> lastExit;
  std::function<QStringList(int)> recentLogs;
};

class ComfyGenerateAnalyzer final : public jobs::Analyzer {
public:
  explicit ComfyGenerateAnalyzer(ServerStateProvider provider = {})
    : provider_(std::move(provider)) {}

  QString id() const override { return QStringLiteral("comfy.generate"); }
  int version() const override { return 1; }
  QString description() const override { return QStringLiteral("Runs a ComfyUI workflow graph on a local ComfyUI server."); }
  bool isMl() const override { return true; }
  QString unavailableReason() const override { return {}; }
  QJsonObject defaultParams() const override { return {}; }

  jobs::AnalyzerResult run(jobs::JobContext& ctx) override;

private:
  ServerStateProvider provider_;
};

} // namespace sf::comfy
