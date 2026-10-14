#include "jobs/hands_tracker.h"

#include "media/video_decoder.h"

#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QPointF>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sf::jobs {

namespace {

constexpr int kPalmSize = 192;
constexpr int kPoseSize = 224;
constexpr int kAnchors = 2016;
constexpr int kMaxSide = 1280; // frames bigger than this are shrunk first: the models only see 192 / 224 pixels anyway
const char* const kPalmModel = "hands/palm_detection_mediapipe_2023feb.onnx";
const char* const kPoseModel = "hands/handpose_estimation_mediapipe_2023feb.onnx";

QString q(const char* s) { return QString::fromLatin1(s); }
double round4(double v) { return std::round(v * 10000.0) / 10000.0; }

struct Palm {
  double x1, y1, x2, y2; // normalized frame coordinates
  std::array<QPointF, 7> keypoints;
  double score;
};

double iou(const Palm& a, const Palm& b) {
  const double ix = std::max(0.0, std::min(a.x2, b.x2) - std::max(a.x1, b.x1));
  const double iy = std::max(0.0, std::min(a.y2, b.y2) - std::max(a.y1, b.y1));
  const double inter = ix * iy;
  const double uni = (a.x2 - a.x1) * (a.y2 - a.y1) + (b.x2 - b.x1) * (b.y2 - b.y1) - inter;
  return uni > 0 ? inter / uni : 0;
}

// RGB bytes of `img` (RGB888) as 0..1 floats, top-left origin, HWC.
void appendRgb(const QImage& rgb, std::vector<float>* out) {
  for (int y = 0; y < rgb.height(); ++y) {
    const uchar* row = rgb.constScanLine(y);
    for (int x = 0; x < rgb.width() * 3; ++x) out->push_back(static_cast<float>(row[x]) / 255.0f);
  }
}

float bilinear(const QImage& img, double x, double y, int ch) {
  // pixel centres at +0.5; outside the picture counts as black (the zoo pads with zeros)
  x -= 0.5;
  y -= 0.5;
  const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
  const double fx = x - x0, fy = y - y0;
  const auto at = [&](int px, int py) -> double {
    if (px < 0 || py < 0 || px >= img.width() || py >= img.height()) return 0.0;
    return img.constScanLine(py)[px * 3 + ch];
  };
  return static_cast<float>((at(x0, y0) * (1 - fx) * (1 - fy) + at(x0 + 1, y0) * fx * (1 - fy) + at(x0, y0 + 1) * (1 - fx) * fy + at(x0 + 1, y0 + 1) * fx * fy) / 255.0);
}

} // namespace

QJsonObject Hand::toJson() const {
  QJsonArray lm;
  for (const auto& p : landmarks) lm.append(QJsonArray{round4(p[0]), round4(p[1]), round4(p[2])});
  return QJsonObject{{q("handedness"), handedness}, {q("score"), round4(score)}, {q("palmScore"), round4(palmScore)}, {q("landmarks"), lm}};
}

std::vector<std::array<float, 2>> HandTracker::palmAnchors() {
  // MediaPipe SSD anchors: strides 8, 16, 16, 16 on a 192 input; layers of equal stride share one grid with 2 anchors each.
  std::vector<std::array<float, 2>> a;
  const int strides[] = {8, 16, 16, 16};
  size_t layer = 0;
  while (layer < 4) {
    size_t last = layer;
    while (last + 1 < 4 && strides[last + 1] == strides[layer]) ++last;
    const int perCell = 2 * static_cast<int>(last - layer + 1);
    const int fm = static_cast<int>(std::ceil(static_cast<double>(kPalmSize) / strides[layer]));
    for (int y = 0; y < fm; ++y)
      for (int x = 0; x < fm; ++x)
        for (int k = 0; k < perCell; ++k) a.push_back({(static_cast<float>(x) + 0.5f) / static_cast<float>(fm), (static_cast<float>(y) + 0.5f) / static_cast<float>(fm)});
    layer = last + 1;
  }
  return a;
}

QString HandTracker::unavailableReason() {
  if (!ort::compiledIn()) return q("ONNX Runtime is not available in this build (configure with SF_ORT_DIR).");
  QString err;
  if (ort::findModel(q(kPalmModel), &err).isEmpty()) return err;
  if (ort::findModel(q(kPoseModel), &err).isEmpty()) return err;
  return {};
}

std::unique_ptr<HandTracker> HandTracker::create(int threads, QString* error) {
  QString err;
  const QString palmPath = ort::findModel(q(kPalmModel), &err);
  if (palmPath.isEmpty()) {
    if (error) *error = err;
    return nullptr;
  }
  const QString posePath = ort::findModel(q(kPoseModel), &err);
  if (posePath.isEmpty()) {
    if (error) *error = err;
    return nullptr;
  }
  std::unique_ptr<HandTracker> t(new HandTracker);
  t->palm_ = ort::Model::load(palmPath, threads, error);
  if (!t->palm_) return nullptr;
  t->pose_ = ort::Model::load(posePath, threads, error);
  if (!t->pose_) return nullptr;
  return t;
}

std::vector<Hand> HandTracker::detect(const QImage& frameIn, const HandsOptions& opt, QString* error) {
  std::vector<Hand> hands;
  if (frameIn.isNull()) return hands;
  QImage frame = frameIn.convertToFormat(QImage::Format_RGB888);
  if (std::max(frame.width(), frame.height()) > kMaxSide) frame = frame.scaled(kMaxSide, kMaxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  const double W = frame.width(), H = frame.height();

  // ---- palm detection on the letterboxed frame
  const double ratio = std::min(kPalmSize / H, kPalmSize / W);
  const int newW = std::max(1, static_cast<int>(W * ratio)), newH = std::max(1, static_cast<int>(H * ratio));
  const int padL = (kPalmSize - newW) / 2, padT = (kPalmSize - newH) / 2;
  QImage canvas(kPalmSize, kPalmSize, QImage::Format_RGB888);
  canvas.fill(Qt::black);
  {
    const QImage small = frame.scaled(newW, newH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB888);
    for (int y = 0; y < newH; ++y) memcpy(canvas.scanLine(y + padT) + padL * 3, small.constScanLine(y), static_cast<size_t>(newW) * 3);
  }
  std::vector<float> in;
  in.reserve(kPalmSize * kPalmSize * 3);
  appendRgb(canvas, &in);
  std::vector<ort::Tensor> out;
  if (!palm_->run(in, {1, kPalmSize, kPalmSize, 3}, &out, error)) return hands;
  if (out.size() < 2) {
    if (error) *error = q("The palm detector returned fewer outputs than expected.");
    return hands;
  }
  // The two outputs are told apart by size: [1,2016,18] boxes + keypoints, [1,2016,1] scores.
  const ort::Tensor* boxes = out[0].data.size() == static_cast<size_t>(kAnchors) * 18 ? &out[0] : &out[1];
  const ort::Tensor* scores = boxes == &out[0] ? &out[1] : &out[0];
  if (boxes->data.size() != static_cast<size_t>(kAnchors) * 18 || scores->data.size() != static_cast<size_t>(kAnchors)) {
    if (error) *error = q("The palm detector output has an unexpected shape.");
    return hands;
  }
  static const std::vector<std::array<float, 2>> anchors = palmAnchors();
  std::vector<Palm> palms;
  for (int i = 0; i < kAnchors; ++i) {
    const double s = 1.0 / (1.0 + std::exp(-static_cast<double>(scores->data[static_cast<size_t>(i)])));
    if (s < opt.palmScore) continue;
    const float* d = &boxes->data[static_cast<size_t>(i) * 18];
    const double ax = anchors[static_cast<size_t>(i)][0], ay = anchors[static_cast<size_t>(i)][1];
    // canvas pixel -> normalized frame coordinate
    const auto toX = [&](double v) { return (v * kPalmSize - padL) / newW; };
    const auto toY = [&](double v) { return (v * kPalmSize - padT) / newH; };
    const double cx = d[0] / kPalmSize + ax, cy = d[1] / kPalmSize + ay, w = d[2] / kPalmSize, h = d[3] / kPalmSize;
    Palm p;
    p.x1 = toX(cx - w / 2);
    p.x2 = toX(cx + w / 2);
    p.y1 = toY(cy - h / 2);
    p.y2 = toY(cy + h / 2);
    for (int k = 0; k < 7; ++k) p.keypoints[static_cast<size_t>(k)] = QPointF(toX(d[4 + 2 * k] / kPalmSize + ax), toY(d[5 + 2 * k] / kPalmSize + ay));
    p.score = s;
    palms.push_back(p);
  }
  std::sort(palms.begin(), palms.end(), [](const Palm& a, const Palm& b) { return a.score > b.score; });
  std::vector<Palm> kept;
  for (const Palm& p : palms) {
    bool dup = false;
    for (const Palm& k : kept)
      if (iou(p, k) > opt.nmsIou) {
        dup = true;
        break;
      }
    if (!dup) kept.push_back(p);
    if (static_cast<int>(kept.size()) >= opt.maxHands) break;
  }

  // ---- one rotated crop per palm through the pose model
  for (const Palm& p : kept) {
    const double bw = (p.x2 - p.x1) * W, bh = (p.y2 - p.y1) * H;
    const double side = 2.6 * std::max(bw, bh);
    if (side < 8) continue;
    const QPointF wrist(p.keypoints[0].x() * W, p.keypoints[0].y() * H), mid(p.keypoints[2].x() * W, p.keypoints[2].y() * H);
    const double theta = std::atan2(mid.x() - wrist.x(), -(mid.y() - wrist.y())); // crop "up" -> wrist-to-middle-finger direction
    const double cs = std::cos(theta), sn = std::sin(theta);
    const auto rot = [&](double x, double y) { return QPointF(cs * x - sn * y, sn * x + cs * y); };
    const QPointF c0((p.x1 + p.x2) / 2 * W, (p.y1 + p.y2) / 2 * H);
    const QPointF centre = c0 + rot(0, -0.4 * bh);
    std::vector<float> crop;
    crop.reserve(kPoseSize * kPoseSize * 3);
    for (int v = 0; v < kPoseSize; ++v)
      for (int u = 0; u < kPoseSize; ++u) {
        const QPointF src = centre + rot(((u + 0.5) / kPoseSize - 0.5) * side, ((v + 0.5) / kPoseSize - 0.5) * side);
        for (int ch = 0; ch < 3; ++ch) crop.push_back(bilinear(frame, src.x(), src.y(), ch));
      }
    std::vector<ort::Tensor> po;
    if (!pose_->run(crop, {1, kPoseSize, kPoseSize, 3}, &po, error)) return hands;
    if (po.size() < 3 || po[0].data.size() < 63) {
      if (error) *error = q("The hand-pose model returned fewer outputs than expected.");
      return hands;
    }
    const double conf = po[1].data.empty() ? 0.0 : po[1].data[0];
    if (conf < opt.handScore) continue;
    Hand hand;
    hand.score = conf;
    hand.palmScore = p.score;
    hand.handedness = (po[2].data.empty() ? 0.0f : po[2].data[0]) > 0.5f ? q("Right") : q("Left");
    for (size_t k = 0; k < 21; ++k) {
      const double lx = po[0].data[k * 3], ly = po[0].data[k * 3 + 1], lz = po[0].data[k * 3 + 2];
      const QPointF src = centre + rot((lx / kPoseSize - 0.5) * side, (ly / kPoseSize - 0.5) * side);
      hand.landmarks[k] = {src.x() / W, src.y() / H, lz / kPoseSize * side / W};
    }
    hands.push_back(hand);
  }
  return hands;
}

// ---------------------------------------------------------------- the analyzer

namespace {

class HandsAnalyzer final : public Analyzer {
public:
  QString id() const override { return q("hands"); }
  int version() const override { return 1; }
  QString description() const override {
    return q("Hand landmarks: palm detection then a rotated-crop hand-pose model (MediaPipe via ONNX Runtime, CPU), sampled at `fps`. Result: frames[{t, hands[{handedness, score, landmarks[21][x,y,z]}]}], "
             "landmarks in normalized frame coordinates.");
  }
  bool isMl() const override { return true; }
  QString unavailableReason() const override { return HandTracker::unavailableReason(); }
  QJsonObject defaultParams() const override {
    return {{q("fps"), 10.0}, {q("maxSeconds"), 0.0}, {q("palmScore"), 0.6}, {q("handScore"), 0.8}, {q("maxHands"), 2}};
  }
  QJsonObject normalizeParams(const QJsonObject& p) const override {
    const QJsonObject d = defaultParams();
    const auto num = [&](const char* k, double lo, double hi) { return std::clamp(p.value(q(k)).toDouble(d.value(q(k)).toDouble()), lo, hi); };
    return {{q("fps"), num("fps", 0.1, 60)}, {q("maxSeconds"), num("maxSeconds", 0, 86400)}, {q("palmScore"), num("palmScore", 0.05, 0.99)},
            {q("handScore"), num("handScore", 0.05, 0.99)}, {q("maxHands"), static_cast<int>(num("maxHands", 1, 8))}};
  }

  AnalyzerResult run(JobContext& ctx) override {
    QString err;
    auto tracker = HandTracker::create(ctx.threads, &err);
    if (!tracker) return AnalyzerResult::failure(err);
    HandsOptions opt;
    opt.palmScore = ctx.params.value(q("palmScore")).toDouble(0.6);
    opt.handScore = ctx.params.value(q("handScore")).toDouble(0.8);
    opt.maxHands = ctx.params.value(q("maxHands")).toInt(2);
    const double fps = ctx.params.value(q("fps")).toDouble(10);
    const double maxSeconds = ctx.params.value(q("maxSeconds")).toDouble(0);

    QJsonArray frames;
    int withHands = 0;
    int width = 0, height = 0;
    double duration = 0;
    const auto addFrame = [&](double t, const QImage& img) -> bool {
      const std::vector<Hand> hs = tracker->detect(img, opt, &err);
      if (!err.isEmpty()) return false;
      QJsonArray arr;
      for (const Hand& h : hs) arr.append(h.toJson());
      if (!hs.empty()) ++withHands;
      frames.append(QJsonObject{{q("t"), round4(t)}, {q("hands"), arr}});
      return true;
    };

    if (!QImageReader::imageFormat(ctx.path).isEmpty()) { // a still image: one frame at t = 0
      const QImage img(ctx.path);
      if (img.isNull()) return AnalyzerResult::failure(QStringLiteral("Cannot read image %1.").arg(ctx.path));
      width = img.width();
      height = img.height();
      if (ctx.stop()) return AnalyzerResult::failure(q("cancelled"));
      if (!addFrame(0, img)) return AnalyzerResult::failure(err);
      ctx.report(1.0);
    } else {
      VideoOpenOptions vo;
      vo.hw = HwMode::Off; // the GPU is shared with the editor
      vo.swThreads = 2;
      auto dec = VideoDecoder::open(ctx.path, vo, &err);
      if (!dec) return AnalyzerResult::failure(err.isEmpty() ? QStringLiteral("Cannot open %1.").arg(ctx.path) : err);
      const VideoStreamInfo& info = dec->info();
      width = info.width;
      height = info.height;
      duration = info.durationSec;
      const double span = maxSeconds > 0 ? std::min(duration, maxSeconds) : duration;
      const int n = std::max(1, static_cast<int>(std::ceil(span * fps - 1e-6)));
      for (int k = 0; k < n; ++k) {
        if (ctx.stop()) return AnalyzerResult::failure(q("cancelled"));
        const double t = k / fps;
        const VideoFramePtr f = dec->frameAt(dec->indexAtTime(info.startSec + t), ctx.cancelled);
        if (!f) {
          if (ctx.cancelled && ctx.cancelled()) return AnalyzerResult::failure(q("cancelled"));
          return AnalyzerResult::failure(dec->lastError().isEmpty() ? QStringLiteral("Cannot decode frame at %1 s.").arg(t) : dec->lastError());
        }
        QImage img = f->image;
        if (info.rotation != 0) img = img.transformed(QTransform().rotate(info.rotation), Qt::SmoothTransformation);
        if (!addFrame(t, img)) return AnalyzerResult::failure(err);
        ctx.report(static_cast<double>(k + 1) / n);
      }
    }
    QJsonObject o{{q("fps"), fps},          {q("width"), width},   {q("height"), height},        {q("durationSec"), round4(duration)}, {q("frameCount"), static_cast<int>(frames.size())},
                  {q("framesWithHands"), withHands}, {q("frames"), frames},
                  {q("method"), q("palm detection (192 letterbox) + rotated 224 crop hand-pose; landmarks in normalized frame coordinates; handedness is the model's label for the unmirrored frame")}};
    return AnalyzerResult::success(o);
  }
};

} // namespace

std::shared_ptr<Analyzer> makeHandsAnalyzer() { return std::make_shared<HandsAnalyzer>(); }

} // namespace sf::jobs
