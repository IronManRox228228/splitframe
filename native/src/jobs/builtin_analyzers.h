#pragma once

// The analyzers that ship with the editor: `hands` (ML, ONNX Runtime) and `waveform` (plain decoding, proves the scheduler is generic).

#include "jobs/job_scheduler.h"

namespace sf::jobs {

// Audio peaks of a file: params {buckets (8..4000, default 200)}; result {sampleRate, totalSamples, durationSec, buckets, peak, min[], max[]}.
std::shared_ptr<Analyzer> makeWaveformAnalyzer();

void registerBuiltinAnalyzers(JobScheduler& scheduler);

} // namespace sf::jobs
