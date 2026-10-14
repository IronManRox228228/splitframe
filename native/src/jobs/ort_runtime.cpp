#include "jobs/ort_runtime.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

#ifdef SF_HAVE_ORT
#include <onnxruntime_cxx_api.h>
#include <exception>
#endif

namespace sf::jobs::ort {

QString modelsDir() {
  const QString env = qEnvironmentVariable("SF_MODELS_DIR");
  if (!env.isEmpty()) return QDir::cleanPath(env);
  const QString setting = QSettings(QStringLiteral("SplitFrame"), QStringLiteral("SplitFrame")).value(QStringLiteral("jobs/modelsDir")).toString();
  if (!setting.isEmpty()) return QDir::cleanPath(setting);
  return QStringLiteral("C:/dev/sf-models");
}

QString findModel(const QString& relativePath, QString* error) {
  const QFileInfo fi(QDir(modelsDir()).filePath(relativePath));
  if (fi.exists() && fi.isFile()) return fi.absoluteFilePath();
  if (error) *error = QStringLiteral("Model %1 not found in %2 (set SF_MODELS_DIR).").arg(relativePath, modelsDir());
  return {};
}

#ifdef SF_HAVE_ORT

bool compiledIn() { return true; }
QString runtimeVersion() { return QString::fromLatin1(OrtGetApiBase()->GetVersionString()); }

struct Model::Impl {
  Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "splitframe"};
  std::unique_ptr<Ort::Session> session;
  std::string inName;
  std::vector<std::string> outNames;
  std::vector<int64_t> inShape;
  int threads = 1;
};

Model::Model() : d(std::make_unique<Impl>()) {}
Model::~Model() = default;

std::unique_ptr<Model> Model::load(const QString& path, int intraThreads, QString* error) {
  if (!QFileInfo::exists(path)) {
    if (error) *error = QStringLiteral("Model file %1 does not exist.").arg(path);
    return nullptr;
  }
  try {
    std::unique_ptr<Model> m(new Model);
    Ort::SessionOptions so;
    m->d->threads = intraThreads < 1 ? 1 : intraThreads;
    so.SetIntraOpNumThreads(m->d->threads);
    so.SetInterOpNumThreads(1);
    so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    m->d->session = std::make_unique<Ort::Session>(m->d->env, path.toStdWString().c_str(), so);
    Ort::AllocatorWithDefaultOptions alloc;
    Ort::Session& s = *m->d->session;
    if (s.GetInputCount() < 1) {
      if (error) *error = QStringLiteral("Model %1 has no input.").arg(path);
      return nullptr;
    }
    m->d->inName = s.GetInputNameAllocated(0, alloc).get();
    m->d->inShape = s.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    for (size_t i = 0; i < s.GetOutputCount(); ++i) m->d->outNames.emplace_back(s.GetOutputNameAllocated(i, alloc).get());
    return m;
  } catch (const std::exception& e) {
    if (error) *error = QStringLiteral("ONNX Runtime could not load %1: %2").arg(path, QString::fromUtf8(e.what()));
    return nullptr;
  }
}

QString Model::inputName() const { return QString::fromStdString(d->inName); }
std::vector<int64_t> Model::inputShape() const { return d->inShape; }
QStringList Model::outputNames() const {
  QStringList l;
  for (const std::string& n : d->outNames) l << QString::fromStdString(n);
  return l;
}
int Model::intraThreads() const { return d->threads; }

bool Model::run(const std::vector<float>& input, const std::vector<int64_t>& shape, std::vector<Tensor>* outputs, QString* error) {
  try {
    int64_t count = 1;
    for (int64_t v : shape) count *= v;
    if (count != static_cast<int64_t>(input.size())) {
      if (error) *error = QStringLiteral("Input has %1 values, the shape needs %2.").arg(input.size()).arg(count);
      return false;
    }
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value in = Ort::Value::CreateTensor<float>(mem, const_cast<float*>(input.data()), input.size(), shape.data(), shape.size());
    std::vector<const char*> inNames{d->inName.c_str()};
    std::vector<const char*> outNames;
    for (const std::string& n : d->outNames) outNames.push_back(n.c_str());
    std::vector<Ort::Value> out = d->session->Run(Ort::RunOptions{nullptr}, inNames.data(), &in, 1, outNames.data(), outNames.size());
    outputs->clear();
    for (Ort::Value& v : out) {
      Tensor t;
      const auto info = v.GetTensorTypeAndShapeInfo();
      t.shape = info.GetShape();
      const float* p = v.GetTensorData<float>();
      t.data.assign(p, p + info.GetElementCount());
      outputs->push_back(std::move(t));
    }
    return true;
  } catch (const std::exception& e) {
    if (error) *error = QStringLiteral("ONNX Runtime failed: %1").arg(QString::fromUtf8(e.what()));
    return false;
  }
}

#else

bool compiledIn() { return false; }
QString runtimeVersion() { return {}; }

struct Model::Impl {};
Model::Model() = default;
Model::~Model() = default;
std::unique_ptr<Model> Model::load(const QString&, int, QString* error) {
  if (error) *error = QStringLiteral("ONNX Runtime is not available in this build (configure with SF_ORT_DIR).");
  return nullptr;
}
QString Model::inputName() const { return {}; }
std::vector<int64_t> Model::inputShape() const { return {}; }
QStringList Model::outputNames() const { return {}; }
int Model::intraThreads() const { return 0; }
bool Model::run(const std::vector<float>&, const std::vector<int64_t>&, std::vector<Tensor>*, QString* error) {
  if (error) *error = QStringLiteral("ONNX Runtime is not available in this build.");
  return false;
}

#endif

} // namespace sf::jobs::ort
