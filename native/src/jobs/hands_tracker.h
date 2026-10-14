#pragma once

// Hand tracking with the two MediaPipe models from the OpenCV zoo (Apache-2.0), run on ONNX Runtime:
//   palm_detection_mediapipe_2023feb.onnx     in  input_1  [1,192,192,3] float RGB 0..1, letterboxed (aspect kept, centred, black padding)
//                                             out Identity [1,2016,18]  per anchor: box delta cx,cy,w,h + 7 palm keypoints (x,y), all / 192
//                                             out Identity_1 [1,2016,1] score logit (sigmoid)
//   handpose_estimation_mediapipe_2023feb.onnx  in input_1 [1,224,224,3] float RGB 0..1
//                                             out Identity [1,63] 21 x (x,y,z) in crop pixels, Identity_1 [1,1] hand confidence,
//                                             Identity_2 [1,1] handedness (> 0.5 = Right), Identity_3 [1,63] world landmarks (unused)
// The pose model gets a ROTATED crop (as in the zoo's mp_handpose): the palm box is turned so wrist -> middle-finger base points up, moved
// 0.4 box heights toward the fingers and widened 2.6 times to a square, resampled bilinearly to 224 x 224. Landmarks are mapped back through
// the inverse of that crop and returned in normalized frame coordinates (x / width, y / height; z on the x scale, 0 = wrist, smaller = nearer).
// "Left" / "Right" is the model's label for the frame as given: a mirrored (selfie) camera swaps them.
//
// Not thread-safe: one tracker per job.

#include "jobs/job_scheduler.h"
#include "jobs/ort_runtime.h"

#include <QImage>
#include <QJsonObject>

#include <array>
#include <memory>
#include <vector>

namespace sf::jobs {

struct Hand {
  QString handedness; // Left | Right
  double score = 0;   // hand-presence confidence of the pose model
  double palmScore = 0;
  std::array<std::array<double, 3>, 21> landmarks{}; // normalized frame coordinates
  QJsonObject toJson() const;
};

struct HandsOptions {
  double palmScore = 0.6; // palm detector threshold (after the sigmoid)
  double handScore = 0.8; // pose-model confidence threshold
  double nmsIou = 0.3;
  int maxHands = 2;
};

class HandTracker {
public:
  // Null and *error when ORT or a model is missing / unreadable.
  static std::unique_ptr<HandTracker> create(int threads, QString* error);
  // "" when both models can be loaded in this build and models directory, else why not.
  static QString unavailableReason();

  std::vector<Hand> detect(const QImage& frame, const HandsOptions& options, QString* error = nullptr);

  // The 2016 SSD anchor centres (x, y in 0..1) of the palm detector. Exposed for tests.
  static std::vector<std::array<float, 2>> palmAnchors();

private:
  HandTracker() = default;
  std::unique_ptr<ort::Model> palm_, pose_;
};

std::shared_ptr<Analyzer> makeHandsAnalyzer();

} // namespace sf::jobs
