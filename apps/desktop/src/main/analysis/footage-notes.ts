import { runFfmpeg, runWithHwDecode } from '../ffmpeg.ts';
import {
  parseBlackIntervals,
  parseFreezeIntervals,
  parseIntegratedLoudness,
  parseLoudnessSamples,
  parseSilenceIntervals,
  parseVideoSamples,
} from './footage-notes-parse.ts';
import {
  aggregateSegment,
  FOOTAGE_NOTES_VERSION,
  planSegments,
  scoreSegment,
  type SegmentNotes,
} from './footage-quality.ts';
import { ocrImages } from './ocr.ts';

/**
 * Deterministic per-scene "notes" computed once at import with the bundled ffmpeg (no model):
 * one decode pass for brightness/contrast/blur/motion/black/frozen, one audio pass for
 * loudness and silence, plus Windows OCR on the scene keyframes. Standalone: nothing here is
 * read by the agent.
 */

/** Analysis rate and size. Everything is measured on a 320 px wide, 4 fps copy: cheap, and
 * the sharpness/shake calibration in footage-quality.ts assumes exactly this. */
export const SAMPLE_FPS = 4;
export const SAMPLE_WIDTH = 320;

export interface FootageSegmentNotes extends SegmentNotes {
  sceneId?: string;
}

export interface FootageNotes {
  version: number;
  assetId: string;
  computedAt: string;
  /** wall-clock time the analysis took, ms */
  analysisMs: number;
  durationMs: number;
  hasAudio: boolean;
  /** whole-file integrated loudness (LUFS), null if no audio or silent */
  integratedLufs: number | null;
  /** false when OCR could not run (non-Windows, no recognizer language); text fields are then absent */
  ocr: boolean;
  segments: FootageSegmentNotes[];
}

export interface NotesScene {
  id?: string;
  startMs: number;
  endMs: number;
  /** keyframe image used for OCR */
  keyframePath?: string;
}

export function videoFilterChain(): string {
  return [
    `fps=${SAMPLE_FPS}`,
    `scale=${SAMPLE_WIDTH}:-2`,
    'signalstats',
    'blurdetect',
    'siti',
    'blackdetect=d=0.5:pix_th=0.1',
    'freezedetect=n=0.003:d=1',
    'metadata=mode=print:file=-',
  ].join(',');
}

export function audioFilterChain(): string {
  return [
    'ebur128=metadata=1:peak=none',
    'ametadata=mode=print:key=lavfi.r128.M:file=-',
    'silencedetect=n=-45dB:d=0.5',
  ].join(',');
}

export async function computeFootageNotes(
  assetId: string,
  mediaPath: string,
  input: { durationMs: number; hasAudio: boolean; scenes: NotesScene[]; ocr?: boolean; signal?: AbortSignal },
): Promise<FootageNotes> {
  const started = Date.now();
  const { signal } = input;
  const totalSec = input.durationMs / 1000;

  const videoPass = runWithHwDecode(
    (hw) => [...hw, '-i', mediaPath, '-an', '-vf', videoFilterChain(), '-f', 'null', '-'],
    { signal },
  );
  const audioPass = input.hasAudio
    ? runFfmpeg(['-i', mediaPath, '-vn', '-af', audioFilterChain(), '-f', 'null', '-'], { signal })
    : Promise.resolve(null);
  const ocrPass =
    input.ocr === false
      ? Promise.resolve(null)
      : ocrImages(input.scenes.map((s) => s.keyframePath).filter((p): p is string => Boolean(p)), signal);
  const [video, audio, text] = await Promise.all([videoPass, audioPass, ocrPass]);
  if (video.code !== 0) throw new Error(`footage analysis failed: ${video.stderr.trim().split('\n').slice(-2).join(' | ')}`);

  const samples = parseVideoSamples(video.stdout);
  const black = parseBlackIntervals(video.stderr);
  const frozen = parseFreezeIntervals(video.stderr, totalSec);
  const audioOk = audio !== null && audio.code === 0;
  const loudness = audioOk ? parseLoudnessSamples(audio.stdout) : undefined;
  const silence = audioOk ? parseSilenceIntervals(audio.stderr, totalSec) : undefined;

  const specs = planSegments(input.scenes);
  const segments: FootageSegmentNotes[] = specs.map((spec) => {
    const metrics = aggregateSegment({
      startMs: spec.startMs,
      endMs: spec.endMs,
      video: samples,
      black,
      frozen,
      loudness,
      silence,
    });
    const scene = input.scenes[spec.sceneIndex]!;
    const note: FootageSegmentNotes = { ...scoreSegment(spec, metrics), sceneId: scene.id };
    // the keyframe sits in the middle of the scene, so attach its text to the window that contains it
    const mid = (scene.startMs + scene.endMs) / 2;
    const t = scene.keyframePath ? text?.get(scene.keyframePath) : undefined;
    if (t && mid >= spec.startMs && mid < spec.endMs) note.text = t;
    return note;
  });

  return {
    version: FOOTAGE_NOTES_VERSION,
    assetId,
    computedAt: new Date().toISOString(),
    analysisMs: Date.now() - started,
    durationMs: input.durationMs,
    hasAudio: audioOk,
    integratedLufs: audioOk ? parseIntegratedLoudness(audio.stderr) : null,
    ocr: text !== null,
    segments,
  };
}
