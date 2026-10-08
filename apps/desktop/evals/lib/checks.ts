import type { Item, TimelineDoc } from '@cutboard/schema';
import type { Check, CheckInput } from '../types.ts';
import type { Span } from './manifest.ts';
import { check, closeness, fmt, ramp, guard } from './score.ts';
import {
  coveredSec,
  docDurationSec,
  endFrame,
  itemsOfAsset,
  itemsOfType,
  keptFraction,
  meanVolume,
  timelineCoverage,
  tokens,
  type Interval,
} from './timeline.ts';

/** Check builders shared by several tasks (the compound task reuses most of them). */

const span = (s: Span): Interval => ({ start: s.startSec, end: s.endSec });

/** Pause residuals tolerated after a cut: transcript word edges are only accurate to ~0.15 s. */
const PAUSE_RESIDUAL_SEC = 0.45;

export function interviewId(input: CheckInput): string | undefined {
  return input.assetIds['interview.mp4'];
}

/** Share of the interview's long pauses (> minSec) that are gone from the timeline. */
export function pausesRemoved(input: CheckInput, minSec = 0.5, weight = 3): Check {
  const id = interviewId(input);
  const pauses = input.manifest.fixtures['interview.mp4'].pauses.filter((p) => p.durSec > minSec);
  const gone = pauses.filter((p) => {
    const kept = keptFraction(input.doc, id, span(p)) * p.durSec;
    return kept <= PAUSE_RESIDUAL_SEC;
  });
  return check(
    `pauses over ${minSec}s removed`,
    pauses.length ? gone.length / pauses.length : 1,
    `${gone.length}/${pauses.length} pauses cut down to <= ${PAUSE_RESIDUAL_SEC}s`,
    weight,
  );
}

/** The speech segments in `spans` should still be on the timeline (guards against over-cutting). */
export function speechKept(input: CheckInput, spans: Span[], name = 'speech preserved', weight = 2): Check {
  const id = interviewId(input);
  if (spans.length === 0) return guard(name, true, 'nothing to keep', weight);
  const total = spans.reduce((a, s) => a + (s.endSec - s.startSec), 0);
  const kept = spans.reduce((a, s) => a + keptFraction(input.doc, id, span(s)) * (s.endSec - s.startSec), 0);
  const frac = kept / total;
  return guard(name, ramp(frac, 0.6, 0.95), `${fmt(frac * 100, 0)}% of the wanted speech is still on the timeline`, weight);
}

/** Interview speech excluding fillers and the abandoned take: what a good cut keeps. */
export function wantedSpeech(input: CheckInput, opts: { dropFillers?: boolean; dropRetake?: boolean }): Span[] {
  const truth = input.manifest.fixtures['interview.mp4'];
  return truth.segments.filter((s) => {
    if (s.kind === 'pause') return false;
    if (s.kind === 'filler') return !opts.dropFillers;
    if (s.retake === 'abandoned') return !opts.dropRetake;
    return true;
  });
}

export function retakeRemoved(input: CheckInput, weight = 3): Check[] {
  const id = interviewId(input);
  const { abandoned, clean } = input.manifest.fixtures['interview.mp4'].retake;
  const keptBad = keptFraction(input.doc, id, span(abandoned));
  const keptGood = keptFraction(input.doc, id, span(clean));
  return [
    check('abandoned take removed', 1 - ramp(keptBad, 0.2, 0.9), `${fmt(keptBad * 100, 0)}% of the abandoned sentence remains`, weight),
    guard('clean take kept', ramp(keptGood, 0.5, 0.95), `${fmt(keptGood * 100, 0)}% of the clean sentence remains`, weight),
  ];
}

export function fillersRemoved(input: CheckInput, weight = 3): Check {
  const id = interviewId(input);
  const fillers = input.manifest.fixtures['interview.mp4'].fillers;
  const gone = fillers.filter((f) => keptFraction(input.doc, id, span(f)) <= 0.25);
  return check('filler words removed', fillers.length ? gone.length / fillers.length : 1, `${gone.length}/${fillers.length} fillers cut (${fillers.map((f) => f.word).join(', ')})`, weight);
}

// ---------------- captions ----------------

type CaptionItem = Extract<Item, { type: 'caption' }>;

export const captionItems = (doc: TimelineDoc): CaptionItem[] => itemsOfType(doc, 'caption') as CaptionItem[];

export function captionTokens(doc: TimelineDoc): Set<string> {
  const out = new Set<string>();
  for (const c of captionItems(doc)) for (const w of c.props.words) for (const t of tokens(w.w)) out.add(t);
  return out;
}

/** Share of the media's timeline length that has a caption card on screen. */
export function captionCoverage(doc: TimelineDoc): number {
  const media = timelineCoverage(doc, ['video', 'audio']);
  const mediaSec = media.reduce((a, m) => a + (m.end - m.start), 0);
  if (mediaSec <= 0) return 0;
  const caps = timelineCoverage(doc, ['caption']);
  return media.reduce((a, m) => a + coveredSec(m, caps), 0) / mediaSec;
}

export function captionChecks(input: CheckInput, referenceText: string, doc = input.doc): Check[] {
  const caps = captionItems(doc);
  if (caps.length === 0) return [check('caption items exist', false, 'no caption items on the timeline', 3)];
  const have = captionTokens(doc);
  const want = [...new Set(tokens(referenceText))];
  const recall = want.length ? want.filter((t) => have.has(t)).length / want.length : 1;
  const cov = captionCoverage(doc);
  const textTrack = new Set(doc.tracks.filter((t) => t.kind === 'text').map((t) => t.id));
  const offTrack = caps.filter((c) => !textTrack.has(c.trackId)).length;
  const mediaEnd = Math.max(0, ...doc.items.filter((i) => i.type === 'video' || i.type === 'audio').map(endFrame)) / doc.project.fps;
  const overrun = Math.max(0, ...caps.map((c) => endFrame(c) / doc.project.fps - mediaEnd));
  return [
    check('caption items exist', true, `${caps.length} caption cards`, 1),
    check('captions cover the speech', ramp(cov, 0.25, 0.65), `captions on screen for ${fmt(cov * 100, 0)}% of the media`, 3),
    check('caption words match what was said', ramp(recall, 0.3, 0.85), `${fmt(recall * 100, 0)}% of the spoken vocabulary appears in captions`, 2),
    check('captions on a text track', offTrack === 0, offTrack ? `${offTrack} cards on a non-text track` : 'all on text tracks', 1),
    check('captions do not run past the media', overrun <= 0.5, `overrun ${fmt(overrun)}s`, 1),
  ];
}

export function captionFontSize(doc: TimelineDoc): number | undefined {
  const first = captionItems(doc)[0];
  return first?.props.style.fontSize;
}

// ---------------- title ----------------

type TextItem = Extract<Item, { type: 'text' }>;

export function titleChecks(input: CheckInput, text: string, startSec: number, durSec: number, weight = 1): Check[] {
  const fps = input.fps;
  const want = tokens(text).join(' ');
  const texts = (itemsOfType(input.doc, 'text') as TextItem[]).filter((t) => tokens(t.props.text).join(' ').includes(want));
  const hit = texts[0];
  if (!hit) return [check(`title "${text}" exists`, false, `no text item containing "${text}"`, 3 * weight)];
  const startErr = Math.abs(hit.startFrame / fps - startSec);
  const dur = hit.durationFrames / fps;
  const textTrack = input.doc.tracks.find((t) => t.id === hit.trackId)?.kind === 'text';
  return [
    check(`title "${text}" exists`, true, `text item "${hit.props.text}"`, 3 * weight),
    check('title starts at the right time', closeness(startErr, 0, 0.2, 1.5), `starts at ${fmt(hit.startFrame / fps)}s, wanted ${startSec}s`, 2 * weight),
    check('title lasts the right time', closeness(dur, durSec, 0.2, 1.5), `lasts ${fmt(dur)}s, wanted ${durSec}s`, 2 * weight),
    check('title on a text track', textTrack, `track kind ${input.doc.tracks.find((t) => t.id === hit.trackId)?.kind ?? 'missing'}`, weight),
  ];
}

// ---------------- music ----------------

type AudioItem = Extract<Item, { type: 'audio' }>;

export function musicItems(input: CheckInput): AudioItem[] {
  return itemsOfAsset(input.doc, input.assetIds['music.mp3']).filter((i) => i.type === 'audio') as AudioItem[];
}

/**
 * Music under the whole piece: present, starts at 0, long enough, quiet, and (optionally)
 * ducked: quieter during speech than during the pauses between it.
 */
export function musicChecks(input: CheckInput, opts: { speech?: Interval[]; quiet: number; duck: boolean; coverSec: number }): Check[] {
  const music = musicItems(input);
  const m = music[0];
  if (!m) return [check('music on the timeline', false, 'no audio item from music.mp3', 3)];
  const fps = input.fps;
  const checks: Check[] = [check('music on the timeline', true, `${music.length} item(s)`, 1)];
  const startErr = m.startFrame / fps;
  checks.push(check('music starts at the beginning', startErr <= 0.25, `starts at ${fmt(startErr)}s`, 1));
  const covered = coveredSec({ start: 0, end: opts.coverSec }, music.map((i) => ({ start: i.startFrame / fps, end: endFrame(i) / fps })));
  checks.push(check('music covers the piece', ramp(covered / opts.coverSec, 0.5, 0.95), `covers ${fmt(covered)}s of ${fmt(opts.coverSec)}s`, 1));
  const overall = meanVolume(m, { start: 0, end: opts.coverSec }, fps);
  checks.push(check('music sits quietly', overall <= opts.quiet, `mean volume ${fmt(overall)} (max ${opts.quiet})`, 2));
  if (opts.duck) {
    const speech = opts.speech ?? [];
    const gaps: Interval[] = [];
    let cursor = 0;
    for (const s of [...speech].sort((a, b) => a.start - b.start)) {
      if (s.start - cursor > 0.8) gaps.push({ start: cursor + 0.3, end: s.start - 0.3 });
      cursor = Math.max(cursor, s.end);
    }
    const avg = (spans: Interval[]) => (spans.length ? spans.reduce((a, s) => a + meanVolume(m, s, fps), 0) / spans.length : 0);
    const under = avg(speech.filter((s) => s.end - s.start > 1));
    const between = avg(gaps);
    const hasKeys = (m.keyframes?.['volume'] ?? []).length > 0;
    checks.push(check('music has a volume envelope', hasKeys, hasKeys ? `${m.keyframes['volume']!.length} volume keyframes` : 'no volume keyframes', 2));
    // ducking = clearly quieter under speech than in the gaps; credit grows with the gap
    const ratio = between > 0 ? under / between : 1;
    checks.push(check('music is lowered under speech', hasKeys ? ramp(1 - ratio, 0.05, 0.4) : 0, `under speech ${fmt(under)} vs gaps ${fmt(between)}`, 3));
  }
  return checks;
}

/** Total sourced duration check used by tasks that must reach a target length. */
export function durationCheck(doc: TimelineDoc, targetSec: number, tol: number, zeroAt: number, name = 'timeline length', weight = 2): Check {
  const d = docDurationSec(doc);
  return check(name, closeness(d, targetSec, tol, zeroAt), `timeline is ${fmt(d)}s, wanted ${targetSec}s +-${tol}`, weight);
}
