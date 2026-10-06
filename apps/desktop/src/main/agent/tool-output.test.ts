import { describe, expect, it } from 'vitest';
import { splitImageResult, stripImageData } from './tool-output.ts';

const frame = { format: 'image/png', frame: 12, base64: 'A'.repeat(4096) };

describe('splitImageResult', () => {
  it('separates the image bytes from the rest of a captureFrame result', () => {
    const { image, rest } = splitImageResult(frame);
    expect(image).toEqual({ data: frame.base64, mimeType: 'image/png' });
    expect(rest).toEqual({ format: 'image/png', frame: 12 });
  });

  it('leaves ordinary results alone', () => {
    const result = { applied: true, items: [] };
    expect(splitImageResult(result)).toEqual({ image: null, rest: result });
    expect(splitImageResult('text').image).toBeNull();
    expect(splitImageResult(null).image).toBeNull();
  });
});

describe('stripImageData', () => {
  it('replaces the base64 payload with a short description for the UI', () => {
    const out = stripImageData(frame) as Record<string, unknown>;
    expect(out['base64']).toBeUndefined();
    expect(out['image']).toBe('[image/png, 3 KB]');
    expect(JSON.stringify(out).length).toBeLessThan(200);
  });
});
