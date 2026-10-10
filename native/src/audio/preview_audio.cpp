#include "audio/preview_audio.h"

#include "audio/timebase.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace sf::audio {

namespace {
constexpr int kChunk = 1024;
constexpr int kTargetQueued = 9600; // ~200 ms: bounds how stale the ring is when an edit arrives
} // namespace

struct PreviewAudio::Impl {
  AudioOutput out;
  std::shared_ptr<AudioClockSource> clock = std::make_shared<AudioClockSource>();

  std::mutex m;
  std::condition_variable cv;
  std::thread th;
  bool quit = false;

  std::shared_ptr<const TimelineDoc> doc;
  std::shared_ptr<SourceProvider> sources;
  std::uint64_t sessionGen = 0;
  bool playing = false;
  Sample pos = 0;
  std::uint64_t streamGen = 0;
  Sample playStart = 0;
  std::int64_t contentAtPlay = 0;
  std::shared_ptr<AudioEngine> engine; // owned by the render thread, published for meters()

  std::int64_t fps() const { return doc ? std::max<std::int64_t>(1, doc->project.fps) : 30; }

  void run() {
    std::vector<float> buf(static_cast<size_t>(kChunk) * 2);
    std::uint64_t built = ~0ull;
    std::shared_ptr<AudioEngine> eng;
    for (;;) {
      std::shared_ptr<const TimelineDoc> dc;
      std::shared_ptr<SourceProvider> sp;
      std::uint64_t sg, stg;
      Sample p;
      {
        std::unique_lock<std::mutex> l(m);
        cv.wait_for(l, std::chrono::milliseconds(4), [&] { return quit; });
        if (quit) break;
        if (!playing || !out.running() || out.queuedFrames() >= kTargetQueued) continue;
        sg = sessionGen;
        stg = streamGen;
        p = pos;
        dc = doc;
        sp = sources;
      }
      if (sg != built) {
        eng = dc ? std::make_shared<AudioEngine>(dc, sp) : nullptr;
        if (eng) eng->enableLoudness(true);
        built = sg;
        std::lock_guard<std::mutex> l(m);
        engine = eng;
      }
      if (eng) eng->render(p, kChunk, buf.data());
      else std::fill(buf.begin(), buf.end(), 0.0f);
      std::lock_guard<std::mutex> l(m);
      if (stg != streamGen || !playing) continue; // a seek or pause overtook this chunk
      if (out.write(buf.data(), kChunk) == kChunk) pos += kChunk;
    }
    std::lock_guard<std::mutex> l(m);
    engine.reset();
  }
};

PreviewAudio::PreviewAudio() : d(std::make_unique<Impl>()) { d->th = std::thread([this] { d->run(); }); }

PreviewAudio::~PreviewAudio() {
  {
    std::lock_guard<std::mutex> l(d->m);
    d->quit = true;
  }
  d->cv.notify_all();
  d->th.join();
  d->out.stop();
}

bool PreviewAudio::openDevice(QString* error) {
  if (d->out.running()) return true;
  if (!d->out.start(error)) return false;
  d->clock->setOutput(&d->out);
  return true;
}

bool PreviewAudio::deviceOpen() const { return d->out.running(); }
std::shared_ptr<AudioClockSource> PreviewAudio::clockSource() const { return d->clock; }

void PreviewAudio::setSession(std::shared_ptr<const TimelineDoc> doc, std::shared_ptr<SourceProvider> sources) {
  std::lock_guard<std::mutex> l(d->m);
  d->doc = std::move(doc);
  d->sources = std::move(sources);
  ++d->sessionGen;
}

void PreviewAudio::play(Frame frame) {
  if (!d->out.running()) return;
  {
    std::lock_guard<std::mutex> l(d->m);
    d->pos = d->playStart = frameToSample(frame, d->fps());
    ++d->streamGen;
    d->playing = true;
    d->out.flush();
    d->contentAtPlay = d->out.contentFrames();
  }
  d->clock->setAudioMaster(true);
}

void PreviewAudio::pause() {
  d->clock->setAudioMaster(false);
  std::lock_guard<std::mutex> l(d->m);
  if (!d->playing) return;
  d->playing = false;
  ++d->streamGen;
  if (d->out.running()) d->out.flush();
}

void PreviewAudio::seek(Frame frame) {
  std::lock_guard<std::mutex> l(d->m);
  if (!d->playing) return;
  d->pos = d->playStart = frameToSample(frame, d->fps());
  ++d->streamGen;
  d->out.flush();
  d->contentAtPlay = d->out.contentFrames();
}

bool PreviewAudio::playing() const {
  std::lock_guard<std::mutex> l(d->m);
  return d->playing;
}

std::int64_t PreviewAudio::positionSamples() const {
  std::lock_guard<std::mutex> l(d->m);
  return d->playStart + (d->out.contentFrames() - d->contentAtPlay);
}

MeterSnapshot PreviewAudio::meters() const {
  std::shared_ptr<AudioEngine> e;
  {
    std::lock_guard<std::mutex> l(d->m);
    e = d->engine;
  }
  return e ? e->meters() : MeterSnapshot{};
}

AudioEngine::Loudness PreviewAudio::loudness() const {
  std::shared_ptr<AudioEngine> e;
  {
    std::lock_guard<std::mutex> l(d->m);
    e = d->engine;
  }
  return e ? e->loudness() : AudioEngine::Loudness{};
}

std::int64_t PreviewAudio::underruns() const { return d->out.underruns(); }

} // namespace sf::audio
