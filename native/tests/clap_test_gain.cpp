// A minimal CLAP gain plugin the host tests load: one parameter (linear gain), stereo in/out, state.
#include <clap/clap.h>

#include <cstring>

namespace {

struct Gain {
  clap_plugin_t plugin;
  const clap_host_t* host;
  double gain = 1.0;
};

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
const clap_plugin_descriptor_t kDesc = {CLAP_VERSION, "org.splitframe.test.gain", "Test Gain", "SplitFrame", "", "", "", "1.0", "test", kFeatures};

Gain* self(const clap_plugin_t* p) { return static_cast<Gain*>(p->plugin_data); }

uint32_t paramsCount(const clap_plugin_t*) { return 1; }
bool paramsInfo(const clap_plugin_t*, uint32_t i, clap_param_info_t* info) {
  if (i != 0) return false;
  std::memset(info, 0, sizeof *info);
  info->id = 7;
  std::strcpy(info->name, "Gain");
  info->min_value = 0;
  info->max_value = 4;
  info->default_value = 1;
  info->flags = CLAP_PARAM_IS_AUTOMATABLE;
  return true;
}
bool paramsValue(const clap_plugin_t* p, clap_id id, double* v) {
  if (id != 7) return false;
  *v = self(p)->gain;
  return true;
}
bool paramsText(const clap_plugin_t*, clap_id, double, char*, uint32_t) { return false; }
bool paramsFromText(const clap_plugin_t*, clap_id, const char*, double*) { return false; }
void handle(Gain* g, const clap_event_header_t* h) {
  if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) {
    const auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
    if (e->param_id == 7) g->gain = e->value;
  }
}
void paramsFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*) {
  for (uint32_t i = 0; i < in->size(in); ++i) handle(self(p), in->get(in, i));
}
const clap_plugin_params_t kParams = {paramsCount, paramsInfo, paramsValue, paramsText, paramsFromText, paramsFlush};

bool stateSave(const clap_plugin_t* p, const clap_ostream_t* s) {
  const double g = self(p)->gain;
  return s->write(s, &g, sizeof g) == static_cast<int64_t>(sizeof g);
}
bool stateLoad(const clap_plugin_t* p, const clap_istream_t* s) {
  double g = 0;
  if (s->read(s, &g, sizeof g) != static_cast<int64_t>(sizeof g)) return false;
  self(p)->gain = g;
  return true;
}
const clap_plugin_state_t kState = {stateSave, stateLoad};

uint32_t portsCount(const clap_plugin_t*, bool) { return 1; }
bool portsGet(const clap_plugin_t*, uint32_t i, bool, clap_audio_port_info_t* info) {
  if (i != 0) return false;
  std::memset(info, 0, sizeof *info);
  info->id = 0;
  std::strcpy(info->name, "main");
  info->flags = CLAP_AUDIO_PORT_IS_MAIN;
  info->channel_count = 2;
  info->port_type = CLAP_PORT_STEREO;
  info->in_place_pair = CLAP_INVALID_ID;
  return true;
}
const clap_plugin_audio_ports_t kPorts = {portsCount, portsGet};

bool init(const clap_plugin_t*) { return true; }
void destroy(const clap_plugin_t* p) { delete self(p); }
bool activate(const clap_plugin_t*, double, uint32_t, uint32_t) { return true; }
void deactivate(const clap_plugin_t*) {}
bool startProcessing(const clap_plugin_t*) { return true; }
void stopProcessing(const clap_plugin_t*) {}
void reset(const clap_plugin_t*) {}
clap_process_status process(const clap_plugin_t* p, const clap_process_t* pr) {
  Gain* g = self(p);
  const uint32_t n = pr->frames_count;
  uint32_t ev = 0;
  const uint32_t evCount = pr->in_events->size(pr->in_events);
  for (uint32_t i = 0; i < n; ++i) {
    while (ev < evCount) {
      const clap_event_header_t* h = pr->in_events->get(pr->in_events, ev);
      if (h->time > i) break;
      handle(g, h);
      ++ev;
    }
    for (uint32_t c = 0; c < 2; ++c) pr->audio_outputs[0].data32[c][i] = static_cast<float>(pr->audio_inputs[0].data32[c][i] * g->gain);
  }
  return CLAP_PROCESS_CONTINUE;
}
const void* getExtension(const clap_plugin_t*, const char* id) {
  if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
  if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
  if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kPorts;
  return nullptr;
}
void onMainThread(const clap_plugin_t*) {}

uint32_t factoryCount(const clap_plugin_factory_t*) { return 1; }
const clap_plugin_descriptor_t* factoryDesc(const clap_plugin_factory_t*, uint32_t i) { return i == 0 ? &kDesc : nullptr; }
const clap_plugin_t* factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
  if (std::strcmp(id, kDesc.id) != 0) return nullptr;
  Gain* g = new Gain;
  g->host = host;
  g->plugin = {&kDesc, g, init, destroy, activate, deactivate, startProcessing, stopProcessing, reset, process, getExtension, onMainThread};
  return &g->plugin;
}
const clap_plugin_factory_t kFactory = {factoryCount, factoryDesc, factoryCreate};

bool entryInit(const char*) { return true; }
void entryDeinit() {}
const void* entryFactory(const char* id) { return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : nullptr; }

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION, entryInit, entryDeinit, entryFactory};
