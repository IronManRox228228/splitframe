import { readFileSync } from 'node:fs';

/** Ground truth written by fixtures/generate.mjs (all times in seconds of the fixture's own media). */

export interface Segment {
  kind: 'speech' | 'filler' | 'pause';
  text?: string;
  startSec: number;
  endSec: number;
  retake?: 'abandoned' | 'clean';
  topic?: string;
}

export interface Span {
  startSec: number;
  endSec: number;
}

export interface InterviewTruth {
  durationSec: number;
  kind: 'speech';
  segments: Segment[];
  pauses: (Span & { durSec: number })[];
  totalPauseSec: number;
  fillers: (Span & { word: string })[];
  retake: { abandoned: Span & { text: string }; clean: Span & { text: string } };
}

export interface TalkTruth {
  durationSec: number;
  kind: 'speech';
  segments: Segment[];
  topics: (Span & { id: string; title: string })[];
}

export interface BrollTruth {
  durationSec: number;
  kind: 'broll';
  label: string;
}

export interface MusicTruth {
  durationSec: number;
  kind: 'music';
  bpm: number;
  beatPeriodSec: number;
  firstBeatSec: number;
  barEveryBeats: number;
}

export interface Manifest {
  generatedAt: string;
  voice: string;
  fixtures: {
    'interview.mp4': InterviewTruth;
    'talk.mp4': TalkTruth;
    'broll-a.mp4': BrollTruth;
    'broll-b.mp4': BrollTruth;
    'broll-c.mp4': BrollTruth;
    'music.mp3': MusicTruth;
  };
}

export type FixtureName = keyof Manifest['fixtures'];

export const FIXTURE_NAMES: FixtureName[] = ['interview.mp4', 'talk.mp4', 'broll-a.mp4', 'broll-b.mp4', 'broll-c.mp4', 'music.mp3'];

export function loadManifest(path: string): Manifest {
  return JSON.parse(readFileSync(path, 'utf8')) as Manifest;
}
