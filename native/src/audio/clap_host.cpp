#include "audio/clap_host.h"

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

#include <clap/clap.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace sf::audio {

namespace {

constexpr uint32_t kMaxFrames = 4096;

// ---------------------------------------------------------------- library

struct Lib {
  void* handle = nullptr;
  const clap_plugin_entry_t* entry = nullptr;
  const clap_plugin_factory_t* factory = nullptr;
  QString path;

  ~Lib() {
    if (entry) entry->deinit();
#ifdef _WIN32
    if (handle) FreeLibrary(static_cast<HMODULE>(handle));
#endif
  }
};

std::shared_ptr<Lib> openLib(const QString& path, QString* error) {
  auto fail = [&](const QString& m) -> std::shared_ptr<Lib> {
    if (error) *error = m;
    return nullptr;
  };
#ifdef _WIN32
  const std::wstring wpath = QDir::toNativeSeparators(path).toStdWString();
  HMODULE h = LoadLibraryExW(wpath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!h) return fail(QStringLiteral("cannot load %1 (error %2)").arg(path).arg(GetLastError()));
  auto lib = std::make_shared<Lib>();
  lib->handle = h;
  lib->path = path;
  const auto* entry = reinterpret_cast<const clap_plugin_entry_t*>(GetProcAddress(h, "clap_entry"));
  if (!entry) return fail(QStringLiteral("%1 exports no clap_entry").arg(path));
  if (!clap_version_is_compatible(entry->clap_version)) return fail(QStringLiteral("%1 needs a newer CLAP version").arg(path));
  if (!entry->init(path.toUtf8().constData())) return fail(QStringLiteral("%1: clap_entry.init failed").arg(path));
  lib->entry = entry;
  lib->factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
  if (!lib->factory) return fail(QStringLiteral("%1 has no plugin factory").arg(path));
  return lib;
#else
  return fail(QStringLiteral("CLAP hosting is only implemented on Windows"));
#endif
}

// ---------------------------------------------------------------- host side

std::thread::id gMainThread; // set by the first ClapEffect created: the thread that loads plugins is "main"

bool CLAP_ABI hostIsMain(const clap_host_t*) { return std::this_thread::get_id() == gMainThread; }
bool CLAP_ABI hostIsAudio(const clap_host_t*) { return std::this_thread::get_id() != gMainThread; }
const clap_host_thread_check_t kThreadCheck{hostIsMain, hostIsAudio};

void CLAP_ABI hostParamsRescan(const clap_host_t*, clap_param_rescan_flags) {}
void CLAP_ABI hostParamsClear(const clap_host_t*, clap_id, clap_param_clear_flags) {}
void CLAP_ABI hostParamsFlush(const clap_host_t*) {}
const clap_host_params_t kHostParams{hostParamsRescan, hostParamsClear, hostParamsFlush};

void CLAP_ABI hostStateDirty(const clap_host_t*) {}
const clap_host_state_t kHostState{hostStateDirty};

const void* CLAP_ABI hostGetExtension(const clap_host_t*, const char* id) {
  if (!std::strcmp(id, CLAP_EXT_THREAD_CHECK)) return &kThreadCheck;
  if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kHostParams;
  if (!std::strcmp(id, CLAP_EXT_STATE)) return &kHostState;
  return nullptr;
}
void CLAP_ABI hostRequestRestart(const clap_host_t*) {}
void CLAP_ABI hostRequestProcess(const clap_host_t*) {}
void CLAP_ABI hostRequestCallback(const clap_host_t*) {}

const clap_host_t kHost{CLAP_VERSION,        nullptr,           "SplitFrame",       "SplitFrame",
                        "https://github.com", "0.1",             hostGetExtension,   hostRequestRestart,
                        hostRequestProcess,  hostRequestCallback};

// ---------------------------------------------------------------- the effect

struct InEvents {
  std::vector<clap_event_param_value_t> ev;
  clap_input_events_t iface{};
  InEvents() {
    iface.ctx = this;
    iface.size = [](const clap_input_events_t* l) { return static_cast<uint32_t>(static_cast<const InEvents*>(l->ctx)->ev.size()); };
    iface.get = [](const clap_input_events_t* l, uint32_t i) -> const clap_event_header_t* {
      const auto* self = static_cast<const InEvents*>(l->ctx);
      return i < self->ev.size() ? &self->ev[i].header : nullptr;
    };
  }
};

struct ParamInfo {
  clap_id id;
  double min, max, def;
};

class ClapEffect final : public Effect {
public:
  ClapEffect(std::shared_ptr<Lib> lib, const clap_plugin_t* plugin) : lib_(std::move(lib)), plugin_(plugin) {
    if (gMainThread == std::thread::id()) gMainThread = std::this_thread::get_id();
    in_.resize(kMaxFrames);
  }
  ~ClapEffect() override {
    if (processing_) plugin_->stop_processing(plugin_);
    if (active_) plugin_->deactivate(plugin_);
    plugin_->destroy(plugin_);
  }

  bool init(double sampleRate, QString* error) {
    if (!plugin_->init(plugin_)) {
      if (error) *error = QStringLiteral("plugin init failed");
      plugin_ = nullptr;
      return false;
    }
    params_ = static_cast<const clap_plugin_params_t*>(plugin_->get_extension(plugin_, CLAP_EXT_PARAMS));
    state_ = static_cast<const clap_plugin_state_t*>(plugin_->get_extension(plugin_, CLAP_EXT_STATE));
    latency_ = static_cast<const clap_plugin_latency_t*>(plugin_->get_extension(plugin_, CLAP_EXT_LATENCY));
    const auto* ports = static_cast<const clap_plugin_audio_ports_t*>(plugin_->get_extension(plugin_, CLAP_EXT_AUDIO_PORTS));
    if (ports) {
      inPorts_ = ports->count(plugin_, true);
      outPorts_ = ports->count(plugin_, false);
      clap_audio_port_info_t info;
      if (inPorts_ > 0 && ports->get(plugin_, 0, true, &info)) inCh_ = info.channel_count;
      if (outPorts_ > 0 && ports->get(plugin_, 0, false, &info)) outCh_ = info.channel_count;
      if (inPorts_ > 1 && ports->get(plugin_, 1, true, &info)) scCh_ = info.channel_count;
    }
    inCh_ = std::min<uint32_t>(2, std::max<uint32_t>(1, inCh_));
    outCh_ = std::min<uint32_t>(2, std::max<uint32_t>(1, outCh_));
    scCh_ = std::min<uint32_t>(2, std::max<uint32_t>(1, scCh_));

    if (params_) {
      const uint32_t n = params_->count(plugin_);
      names_.reserve(n);
      for (uint32_t i = 0; i < n; ++i) {
        clap_param_info_t pi;
        if (!params_->get_info(plugin_, i, &pi)) continue;
        if (pi.flags & (CLAP_PARAM_IS_HIDDEN | CLAP_PARAM_IS_READONLY)) continue;
        std::string name = pi.name;
        for (const std::string& other : names_)
          if (other == name) name += "#" + std::to_string(pi.id);
        names_.push_back(name);
        info_.push_back({pi.id, pi.min_value, pi.max_value, pi.default_value});
        defs_.push_back({names_.back().c_str(), pi.default_value, pi.min_value, pi.max_value});
        vals_.push_back(pi.default_value);
      }
    }
    return activate(sampleRate, error);
  }

  void prepare(double sr) override {
    if (sr != sr_) {
      QString err;
      activate(sr, &err);
    }
  }
  void reset() override {
    if (processing_) plugin_->reset(plugin_);
  }
  const std::vector<ParamDef>& paramDefs() const override { return defs_; }
  bool setParam(const QString& name, double v) override {
    const int i = find(name);
    if (i < 0) return false;
    const auto& pi = info_[static_cast<size_t>(i)];
    v = std::min(pi.max, std::max(pi.min, v));
    if (vals_[static_cast<size_t>(i)] == v) return true;
    vals_[static_cast<size_t>(i)] = v;
    clap_event_param_value_t e{};
    e.header.size = sizeof e;
    e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e.header.type = CLAP_EVENT_PARAM_VALUE;
    e.param_id = pi.id;
    e.cookie = nullptr;
    e.note_id = -1;
    e.port_index = -1;
    e.channel = -1;
    e.key = -1;
    e.value = v;
    pending_.push_back(e);
    return true;
  }
  bool setParamString(const QString&, const QString&) override { return false; }
  double param(const QString& name) const override {
    const int i = find(name);
    return i < 0 ? 0.0 : vals_[static_cast<size_t>(i)];
  }
  bool usesSidechain() const override { return inPorts_ > 1; }
  int latencySamples() const override { return latency_ ? static_cast<int>(latency_->get(plugin_)) : 0; }

  void process(float* io, int frames, const float* sidechain) override {
    if (!processing_) return;
    int done = 0;
    while (done < frames) {
      const int n = std::min<int>(frames - done, kMaxFrames);
      chunk(io + done * 2, n, sidechain ? sidechain + done * 2 : nullptr);
      done += n;
    }
  }

  QByteArray saveState() override {
    QByteArray data;
    if (!state_) return data;
    clap_ostream_t os{};
    os.ctx = &data;
    os.write = [](const clap_ostream_t* s, const void* buf, uint64_t size) -> int64_t {
      static_cast<QByteArray*>(s->ctx)->append(static_cast<const char*>(buf), static_cast<qsizetype>(size));
      return static_cast<int64_t>(size);
    };
    if (!state_->save(plugin_, &os)) data.clear();
    return data;
  }
  bool loadState(const QByteArray& data) override {
    if (!state_ || data.isEmpty()) return false;
    struct Src {
      const QByteArray* d;
      qsizetype pos;
    } src{&data, 0};
    clap_istream_t is{};
    is.ctx = &src;
    is.read = [](const clap_istream_t* s, void* buf, uint64_t size) -> int64_t {
      auto* r = static_cast<Src*>(s->ctx);
      const qsizetype n = std::min<qsizetype>(static_cast<qsizetype>(size), r->d->size() - r->pos);
      if (n <= 0) return 0;
      std::memcpy(buf, r->d->constData() + r->pos, static_cast<size_t>(n));
      r->pos += n;
      return n;
    };
    const bool ok = state_->load(plugin_, &is);
    if (ok && params_) // the plugin's values moved: read them back so setParam compares against the truth
      for (size_t i = 0; i < info_.size(); ++i) {
        double v;
        if (params_->get_value(plugin_, info_[i].id, &v)) vals_[i] = v;
      }
    return ok;
  }

private:
  int find(const QString& name) const {
    for (size_t i = 0; i < names_.size(); ++i)
      if (name == QLatin1StringView(names_[i].c_str())) return static_cast<int>(i);
    return -1;
  }

  bool activate(double sr, QString* error) {
    if (processing_) plugin_->stop_processing(plugin_);
    processing_ = false;
    if (active_) plugin_->deactivate(plugin_);
    active_ = false;
    if (!plugin_->activate(plugin_, sr, 1, kMaxFrames)) {
      if (error) *error = QStringLiteral("plugin activate failed");
      return false;
    }
    active_ = true;
    sr_ = sr;
    if (!plugin_->start_processing(plugin_)) {
      if (error) *error = QStringLiteral("plugin start_processing failed");
      return false;
    }
    processing_ = true;
    return true;
  }

  void chunk(float* io, int n, const float* sc) {
    float* inP[2] = {inL_.data(), inR_.data()};
    for (int i = 0; i < n; ++i) {
      inL_[static_cast<size_t>(i)] = io[i * 2];
      inR_[static_cast<size_t>(i)] = io[i * 2 + 1];
    }
    if (inCh_ == 1)
      for (int i = 0; i < n; ++i) inL_[static_cast<size_t>(i)] = 0.5f * (io[i * 2] + io[i * 2 + 1]);
    float* scP[2] = {scL_.data(), scR_.data()};
    for (int i = 0; i < n; ++i) {
      scL_[static_cast<size_t>(i)] = sc ? sc[i * 2] : 0.0f;
      scR_[static_cast<size_t>(i)] = sc ? sc[i * 2 + 1] : 0.0f;
    }
    if (scCh_ == 1 && sc)
      for (int i = 0; i < n; ++i) scL_[static_cast<size_t>(i)] = 0.5f * (sc[i * 2] + sc[i * 2 + 1]);
    float* outP[2] = {outL_.data(), outR_.data()};

    clap_audio_buffer_t ins[2];
    ins[0] = {inP, nullptr, inCh_, 0};
    ins[1] = {scP, nullptr, scCh_, 0};
    clap_audio_buffer_t outs[1] = {{outP, nullptr, outCh_, 0}};

    events_.ev = std::move(pending_);
    pending_.clear();
    clap_output_events_t outEvents{};
    outEvents.ctx = nullptr;
    outEvents.try_push = [](const clap_output_events_t*, const clap_event_header_t*) { return true; };

    clap_process_t pr{};
    pr.steady_time = -1;
    pr.frames_count = static_cast<uint32_t>(n);
    pr.transport = nullptr;
    pr.audio_inputs = ins;
    pr.audio_inputs_count = inPorts_ > 1 ? 2 : 1;
    pr.audio_outputs = outs;
    pr.audio_outputs_count = 1;
    pr.in_events = &events_.iface;
    pr.out_events = &outEvents;
    const clap_process_status st = plugin_->process(plugin_, &pr);
    events_.ev.clear();
    if (st == CLAP_PROCESS_ERROR) return; // leave the signal untouched
    for (int i = 0; i < n; ++i) {
      const float l = outL_[static_cast<size_t>(i)];
      io[i * 2] = l;
      io[i * 2 + 1] = outCh_ == 1 ? l : outR_[static_cast<size_t>(i)];
    }
  }

  std::shared_ptr<Lib> lib_;
  const clap_plugin_t* plugin_ = nullptr;
  const clap_plugin_params_t* params_ = nullptr;
  const clap_plugin_state_t* state_ = nullptr;
  const clap_plugin_latency_t* latency_ = nullptr;
  uint32_t inPorts_ = 1, outPorts_ = 1, inCh_ = 2, outCh_ = 2, scCh_ = 2;
  double sr_ = 0;
  bool active_ = false, processing_ = false;
  std::vector<std::string> names_;
  std::vector<ParamInfo> info_;
  std::vector<ParamDef> defs_;
  std::vector<double> vals_;
  std::vector<clap_event_param_value_t> pending_;
  InEvents events_;
  std::vector<float> in_; // unused scratch kept for layout symmetry
  std::vector<float> inL_ = std::vector<float>(kMaxFrames), inR_ = std::vector<float>(kMaxFrames);
  std::vector<float> scL_ = std::vector<float>(kMaxFrames), scR_ = std::vector<float>(kMaxFrames);
  std::vector<float> outL_ = std::vector<float>(kMaxFrames), outR_ = std::vector<float>(kMaxFrames);
};

} // namespace

QStringList defaultClapSearchPaths() {
  QStringList out;
#ifdef _WIN32
  const QString common = qEnvironmentVariable("COMMONPROGRAMFILES");
  if (!common.isEmpty()) out << common + QStringLiteral("/CLAP");
  const QString local = qEnvironmentVariable("LOCALAPPDATA");
  if (!local.isEmpty()) out << local + QStringLiteral("/Programs/Common/CLAP");
#endif
  const QString env = qEnvironmentVariable("CLAP_PATH");
  if (!env.isEmpty())
    for (const QString& p : env.split(QDir::listSeparator(), Qt::SkipEmptyParts)) out << p;
  return out;
}

std::vector<ClapPluginInfo> readClapFile(const QString& path, QString* error) {
  std::vector<ClapPluginInfo> out;
  const auto lib = openLib(path, error);
  if (!lib) return out;
  const uint32_t n = lib->factory->get_plugin_count(lib->factory);
  for (uint32_t i = 0; i < n; ++i) {
    const clap_plugin_descriptor_t* d = lib->factory->get_plugin_descriptor(lib->factory, i);
    if (!d || !d->id) continue;
    ClapPluginInfo info;
    info.path = path;
    info.id = QString::fromUtf8(d->id);
    info.name = QString::fromUtf8(d->name ? d->name : d->id);
    info.vendor = QString::fromUtf8(d->vendor ? d->vendor : "");
    info.version = QString::fromUtf8(d->version ? d->version : "");
    for (const char* const* f = d->features; f && *f; ++f) info.features << QString::fromUtf8(*f);
    out.push_back(std::move(info));
  }
  return out;
}

std::vector<ClapPluginInfo> scanClapPlugins(const QStringList& paths) {
  std::vector<ClapPluginInfo> out;
  const QStringList roots = paths.isEmpty() ? defaultClapSearchPaths() : paths;
  for (const QString& root : roots) {
    if (!QFileInfo(root).isDir()) continue;
    QDirIterator it(root, {QStringLiteral("*.clap")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
      const auto found = readClapFile(it.next());
      out.insert(out.end(), found.begin(), found.end());
    }
  }
  return out;
}

std::unique_ptr<Effect> loadClapPlugin(const QString& path, const QString& id, double sampleRate, QString* error) {
  const auto lib = openLib(path, error);
  if (!lib) return nullptr;
  const clap_plugin_t* p = lib->factory->create_plugin(lib->factory, &kHost, id.toUtf8().constData());
  if (!p) {
    if (error) *error = QStringLiteral("%1 has no plugin %2").arg(path, id);
    return nullptr;
  }
  auto fx = std::make_unique<ClapEffect>(lib, p);
  if (!fx->init(sampleRate, error)) return nullptr;
  return fx;
}

std::unique_ptr<Effect> makeClapEffect(const MixInsert& insert, double sampleRate) {
  const auto get = [&](const char* key) {
    const auto it = insert.params.find(QLatin1String(key));
    return it != insert.params.end() && std::holds_alternative<QString>(it->second) ? std::get<QString>(it->second) : QString();
  };
  const QString path = get("pluginPath"), id = get("pluginId");
  if (path.isEmpty() || id.isEmpty()) return nullptr;
  auto fx = loadClapPlugin(path, id, sampleRate);
  if (!fx) return nullptr;
  if (insert.state) fx->loadState(QByteArray::fromBase64(insert.state->toLatin1()));
  // numeric params are plugin parameters (by display name)
  for (const auto& [k, v] : insert.params)
    if (const double* d = std::get_if<double>(&v)) fx->setParam(k, *d);
  return fx;
}

} // namespace sf::audio
