#pragma once

// ONNX Runtime, optional: with SF_HAVE_ORT (CMake finds SF_ORT_DIR, default C:/dev/onnxruntime-win-x64-1.30.0) models run on the
// CPU execution provider; without it every call reports "unavailable" and nothing else changes. No ORT header leaks out of this
// file's .cpp, so the rest of the editor never sees the dependency.
//
//   modelsDir()   $SF_MODELS_DIR, else the "jobs/modelsDir" setting (QSettings SplitFrame), else C:/dev/sf-models
//   findModel()   <modelsDir>/<relative path>; a missing file is a clean error, never a crash
//   Model         one session: float32 NHWC tensors in, float32 tensors out; run() is thread-safe per ORT, one model per job here

#include <QString>
#include <QStringList>

#include <cstdint>
#include <memory>
#include <vector>

namespace sf::jobs::ort {

bool compiledIn();
QString runtimeVersion(); // "1.30.0", or "" when not compiled in

QString modelsDir();
// Absolute path of a model below modelsDir(); empty and *error when it is not there.
QString findModel(const QString& relativePath, QString* error = nullptr);

struct Tensor {
  std::vector<int64_t> shape;
  std::vector<float> data;
};

class Model {
public:
  // Loads `path` with `intraThreads` threads inside an operator (inter-op is 1). Null and *error on any failure.
  static std::unique_ptr<Model> load(const QString& path, int intraThreads, QString* error = nullptr);
  ~Model();
  Model(const Model&) = delete;
  Model& operator=(const Model&) = delete;

  QString inputName() const;
  std::vector<int64_t> inputShape() const; // as the model declares it (-1 = dynamic)
  QStringList outputNames() const;
  int intraThreads() const;

  // `input` must hold the product of `shape` floats. Outputs come back in the model's output order.
  bool run(const std::vector<float>& input, const std::vector<int64_t>& shape, std::vector<Tensor>* outputs, QString* error = nullptr);

private:
  Model();
  struct Impl;
  std::unique_ptr<Impl> d;
};

} // namespace sf::jobs::ort
