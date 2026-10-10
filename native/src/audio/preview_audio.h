#pragma once

// Preview playback through the audio engine: a render thread keeps the output ring full from an
// AudioEngine built on the current document, and the PlaybackClock follows the sound card (audio
// master). Everything the user hears in preview is what export will render.
//
// Threads: the control calls (setSession, play, pause, seek, ...) come from the UI thread; the engine is
// built and run on the internal render thread, so opening media never blocks the UI.

#include "audio/audio_output.h"
#include "audio/engine.h"

#include <memory>

namespace sf::audio {

class PreviewAudio {
public:
  PreviewAudio();
  ~PreviewAudio();
  PreviewAudio(const PreviewAudio&) = delete;
  PreviewAudio& operator=(const PreviewAudio&) = delete;

  // Opens the default output device (once). False when there is none: everything else keeps working
  // silently and the clock stays on the system clock.
  bool openDevice(QString* error = nullptr);
  bool deviceOpen() const;

  // Hand this to PlaybackClock's constructor.
  std::shared_ptr<AudioClockSource> clockSource() const;

  // The document and media the engine mixes; the playback position is kept. Cheap to call on every edit
  // (the engine is rebuilt on the render thread). Null doc stops the sound.
  void setSession(std::shared_ptr<const TimelineDoc> doc, std::shared_ptr<SourceProvider> sources);

  // Sound from timeline frame `frame` on, at 1x. Switches the clock source to the sound card.
  void play(Frame frame);
  // Silence; the clock source goes back to the system clock (so paused/other-rate playback still works).
  void pause();
  // While playing: restart the stream at `frame`.
  void seek(Frame frame);
  bool playing() const;
  // Timeline sample the card is playing (audio master position)
  std::int64_t positionSamples() const;

  MeterSnapshot meters() const;
  AudioEngine::Loudness loudness() const;
  std::int64_t underruns() const;

  struct Impl;

private:
  std::unique_ptr<Impl> d;
};

} // namespace sf::audio
