#include "index/ocr_bridge.h"

#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QTest>

class TstIndexOcr : public QObject {
  Q_OBJECT

private slots:
  void ocrAvailabilityAndRecognition() {
    sf::index::OcrBridge ocr;

#ifdef _WIN32
    QVERIFY(ocr.isAvailable());

    // Create a 600x200 high-contrast test image
    QImage img(600, 200, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);

    {
      QPainter p(&img);
      p.setPen(Qt::black);
      QFont font(QStringLiteral("Arial"), 36, QFont::Bold);
      p.setFont(font);
      p.drawText(QRect(20, 20, 560, 160), Qt::AlignCenter, QStringLiteral("SPLITFRAME EDIT"));
    }

    const auto records = ocr.recognize(img, QStringLiteral("asset_1"), 42, 1.4);
    QVERIFY(!records.empty());

    bool foundWord = false;
    for (const auto& r : records) {
      if (r.text.contains(QStringLiteral("SPLITFRAME"), Qt::CaseInsensitive) ||
          r.text.contains(QStringLiteral("EDIT"), Qt::CaseInsensitive)) {
        foundWord = true;
      }
      QCOMPARE(r.assetId, QStringLiteral("asset_1"));
      QCOMPARE(r.frame, 42);
      QCOMPARE(r.timeSec, 1.4);
      QVERIFY(r.x >= 0.0 && r.x <= 1.0);
      QVERIFY(r.y >= 0.0 && r.y <= 1.0);
      QVERIFY(r.w > 0.0 && r.w <= 1.0);
      QVERIFY(r.h > 0.0 && r.h <= 1.0);
    }
    QVERIFY(foundWord);

#else
    // Non-Windows stub
    QVERIFY(!ocr.isAvailable());
#endif
  }
};

int main(int argc, char* argv[]) {
  QGuiApplication app(argc, argv);
  TstIndexOcr tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_index_ocr.moc"
