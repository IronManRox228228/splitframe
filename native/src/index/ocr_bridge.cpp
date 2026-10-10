#include "index/ocr_bridge.h"

#include <QUuid>
#include <future>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>

#pragma comment(lib, "windowsapp.lib")

using namespace winrt;
using namespace Windows::Media::Ocr;
using namespace Windows::Graphics::Imaging;
using namespace Windows::Storage::Streams;
#endif

namespace sf::index {

struct OcrBridge::Impl {
  bool available = false;
#ifdef _WIN32
  OcrEngine engine = nullptr;
#endif

  Impl() {
#ifdef _WIN32
    try {
      try {
        init_apartment(apartment_type::multi_threaded);
      } catch (...) {
        // Apartment may already be initialized on this thread
      }

      engine = OcrEngine::TryCreateFromUserProfileLanguages();
      if (!engine) {
        auto langs = OcrEngine::AvailableRecognizerLanguages();
        if (langs.Size() > 0) {
          engine = OcrEngine::TryCreateFromLanguage(langs.GetAt(0));
        }
      }
      available = (engine != nullptr);
    } catch (...) {
      available = false;
    }
#else
    available = false;
#endif
  }
};

OcrBridge::OcrBridge() : impl_(std::make_unique<Impl>()) {}
OcrBridge::~OcrBridge() = default;

bool OcrBridge::isAvailable() const {
  return impl_ && impl_->available;
}

std::vector<OcrRecord> OcrBridge::recognize(const QImage& image,
                                           const QString& assetId,
                                           qint64 frame,
                                           double timeSec) {
  std::vector<OcrRecord> results;
  if (!isAvailable() || image.isNull()) {
    return results;
  }

#ifdef _WIN32
  try {
    const int w = image.width();
    const int h = image.height();
    if (w <= 0 || h <= 0) return results;

    // Convert to ARGB32_Premultiplied (in-memory layout matches Bgra8 on little-endian)
    const QImage bgra = (image.format() == QImage::Format_ARGB32_Premultiplied)
      ? image
      : image.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    DataWriter writer;
    writer.WriteBytes(array_view<const uint8_t>(
      reinterpret_cast<const uint8_t*>(bgra.constBits()),
      static_cast<uint32_t>(bgra.sizeInBytes())
    ));
    IBuffer buf = writer.DetachBuffer();

    auto softwareBitmap = SoftwareBitmap::CreateCopyFromBuffer(
      buf,
      BitmapPixelFormat::Bgra8,
      w,
      h,
      BitmapAlphaMode::Premultiplied
    );

    auto ocrTask = std::async(std::launch::async, [&]() -> OcrResult {
      try {
        init_apartment(apartment_type::multi_threaded);
      } catch (...) {}
      return impl_->engine.RecognizeAsync(softwareBitmap).get();
    });
    auto ocrResult = ocrTask.get();
    if (!ocrResult) return results;

    const double invW = 1.0 / w;
    const double invH = 1.0 / h;

    auto lines = ocrResult.Lines();
    for (uint32_t l = 0; l < lines.Size(); ++l) {
      auto line = lines.GetAt(l);
      auto words = line.Words();
      for (uint32_t wd = 0; wd < words.Size(); ++wd) {
        auto word = words.GetAt(wd);
        auto rect = word.BoundingRect();

        OcrRecord rec;
        rec.id = QStringLiteral("ocr_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
        rec.assetId = assetId;
        rec.frame = frame;
        rec.timeSec = timeSec;
        rec.text = QString::fromWCharArray(word.Text().c_str());
        rec.x = rect.X * invW;
        rec.y = rect.Y * invH;
        rec.w = rect.Width * invW;
        rec.h = rect.Height * invH;
        rec.confidence = 1.0;

        results.push_back(rec);
      }
    }
  } catch (...) {
    // OCR failed or threw; return partial/empty results cleanly
  }
#else
  Q_UNUSED(frame);
  Q_UNUSED(timeSec);
  Q_UNUSED(assetId);
#endif

  return results;
}

} // namespace sf::index
