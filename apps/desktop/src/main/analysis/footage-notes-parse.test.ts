import { describe, expect, it } from 'vitest';
import {
  parseBlackIntervals,
  parseFreezeIntervals,
  parseIntegratedLoudness,
  parseLoudnessSamples,
  parseSilenceIntervals,
  parseVideoSamples,
} from './footage-notes-parse.ts';

// captured from the bundled ffmpeg build
const VIDEO_STDOUT = `frame:0    pts:0       pts_time:0
lavfi.signalstats.YMIN=7
lavfi.signalstats.YLOW=41
lavfi.signalstats.YAVG=126.03
lavfi.signalstats.YHIGH=210
lavfi.signalstats.YMAX=255
lavfi.blur=3.964447
lavfi.siti.si=113.75
lavfi.siti.ti=0.00
frame:1    pts:1       pts_time:0.25
lavfi.signalstats.YLOW=41
lavfi.signalstats.YAVG=126.008
lavfi.signalstats.YHIGH=210
lavfi.blur=4.1
lavfi.siti.si=112
lavfi.siti.ti=0.04
`;

describe('parseVideoSamples', () => {
  it('reads per-frame stats keyed by pts_time', () => {
    const s = parseVideoSamples(VIDEO_STDOUT);
    expect(s).toHaveLength(2);
    expect(s[0]).toEqual({ t: 0, yAvg: 126.03, yLow: 41, yHigh: 210, blur: 3.964447, si: 113.75, ti: 0 });
    expect(s[1]!.t).toBe(0.25);
    expect(s[1]!.ti).toBeCloseTo(0.04);
  });

  it('merges repeated headers for the same frame and ignores junk', () => {
    const s = parseVideoSamples(
      'frame:0 pts:0 pts_time:1\nlavfi.blur=2\nframe:0 pts:0 pts_time:1\nlavfi.siti.ti=3\nnoise\nlavfi.blur=nan\n',
    );
    expect(s).toEqual([{ t: 1, blur: 2, ti: 3 }]);
  });

  it('returns nothing for empty output', () => {
    expect(parseVideoSamples('')).toEqual([]);
  });
});

describe('black and freeze intervals', () => {
  const STDERR = `[Parsed_freezedetect_3 @ 0000008010d51700] lavfi.freezedetect.freeze_start: 3
[Parsed_blackdetect_2 @ 0000008010d51f80] black_start:3 black_end:5.25 black_duration:2.25
[Parsed_freezedetect_3 @ 0000008010d51700] lavfi.freezedetect.freeze_duration: 2
[Parsed_freezedetect_3 @ 0000008010d51700] lavfi.freezedetect.freeze_end: 5
`;
  it('parses blackdetect lines', () => {
    expect(parseBlackIntervals(STDERR)).toEqual([{ startSec: 3, endSec: 5.25 }]);
    expect(parseBlackIntervals('nothing')).toEqual([]);
  });
  it('pairs freeze start and end', () => {
    expect(parseFreezeIntervals(STDERR, 8)).toEqual([{ startSec: 3, endSec: 5 }]);
  });
  it('closes an unterminated freeze at the end of the media', () => {
    expect(parseFreezeIntervals('lavfi.freezedetect.freeze_start: 6.5', 10)).toEqual([{ startSec: 6.5, endSec: 10 }]);
  });
});

describe('audio parsers', () => {
  it('reads momentary loudness samples', () => {
    const out = `frame:0    pts:0       pts_time:0
lavfi.r128.M=-120.691
frame:1    pts:4410    pts_time:0.1
lavfi.r128.M=-21.731
`;
    expect(parseLoudnessSamples(out)).toEqual([
      { t: 0, m: -120.691 },
      { t: 0.1, m: -21.731 },
    ]);
  });

  it('parses silence intervals, including one left open at the end', () => {
    const err = `[Parsed_silencedetect_2 @ 000000f201f0f780] silence_start: 5
[Parsed_silencedetect_2 @ 000000f201f0f780] silence_end: 8.010884 | silence_duration: 3.010884
[Parsed_silencedetect_2 @ 000000f201f0f780] silence_start: 9`;
    expect(parseSilenceIntervals(err, 12)).toEqual([
      { startSec: 5, endSec: 8.010884 },
      { startSec: 9, endSec: 12 },
    ]);
  });

  it('reads integrated loudness from the ebur128 summary', () => {
    const err = `[Parsed_ebur128_0 @ 000000800054e400] Summary:

  Integrated loudness:
    I:         -21.9 LUFS
    Threshold: -32.0 LUFS
`;
    expect(parseIntegratedLoudness(err)).toBe(-21.9);
    expect(parseIntegratedLoudness('no summary')).toBeNull();
  });

  it('treats digital silence as no loudness', () => {
    expect(parseIntegratedLoudness('Integrated loudness:\n    I:         -120.7 LUFS\n')).toBeNull();
  });
});
