import { describe, expect, it } from 'vitest';
import { cleanOcrText, parseOcrOutput } from './ocr.ts';

const b64 = (v: unknown): string => Buffer.from(JSON.stringify(v), 'utf8').toString('base64');

describe('parseOcrOutput', () => {
  it('decodes path/text pairs', () => {
    const out = parseOcrOutput(b64([['C:\\a.jpg', 'SplitFrame OCR  test\r\n2026'], ['C:\\b.jpg', '']]));
    expect(out?.get('C:\\a.jpg')).toBe('SplitFrame OCR test 2026');
    expect(out?.get('C:\\b.jpg')).toBe('');
  });

  it('keeps non-ASCII text intact', () => {
    expect(parseOcrOutput(b64([['x', 'Café ñandú 東京']]))?.get('x')).toBe('Café ñandú 東京');
  });

  it('reports unavailable when there is no engine or the output is garbage', () => {
    expect(parseOcrOutput('NOENGINE')).toBeNull();
    expect(parseOcrOutput('')).toBeNull();
    expect(parseOcrOutput('%%%not base64 json%%%')).toBeNull();
    expect(parseOcrOutput(b64({ not: 'an array' }))).toBeNull();
  });
});

describe('cleanOcrText', () => {
  it('drops noise made only of symbols or a couple of characters', () => {
    expect(cleanOcrText('  | - ~ . ')).toBe('');
    expect(cleanOcrText('a b')).toBe('');
    expect(cleanOcrText('Exit 12')).toBe('Exit 12');
  });
});
