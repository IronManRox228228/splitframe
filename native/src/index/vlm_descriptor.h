#pragma once

#include "index/types.h"
#include "agent/llm_client.h"

#include <QImage>
#include <memory>

namespace sf::index {

class VlmDescriptor {
public:
  explicit VlmDescriptor(std::shared_ptr<agent::LlmClient> llm = nullptr);
  ~VlmDescriptor();

  void setLlmClient(std::shared_ptr<agent::LlmClient> llm);

  // Generates structured 1-sentence descriptor for a keyframe image
  VlmTag describeKeyframe(const QImage& keyframe,
                          const QString& assetId,
                          const QString& sceneId);

private:
  std::shared_ptr<agent::LlmClient> llm_;
};

} // namespace sf::index
