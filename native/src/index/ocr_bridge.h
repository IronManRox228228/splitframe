#pragma once

#include "index/types.h"

#include <QImage>
#include <QString>
#include <memory>
#include <vector>

namespace sf::index {

class OcrBridge {
public:
  OcrBridge();
  ~OcrBridge();

  OcrBridge(const OcrBridge&) = delete;
  OcrBridge& operator=(const OcrBridge&) = delete;

  // Returns true if the native OCR engine is initialized and ready
  bool isAvailable() const;

  // Recognizes text in image and returns positioned records with normalized coordinates
  std::vector<OcrRecord> recognize(const QImage& image,
                                   const QString& assetId,
                                   qint64 frame = 0,
                                   double timeSec = 0.0);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace sf::index
