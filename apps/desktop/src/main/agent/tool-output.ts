/**
 * Tool results that carry an image (captureFrame) must reach the model/MCP client as an
 * image, not as megabytes of base64 inside a JSON string.
 */

export interface ImageResult {
  data: string;
  mimeType: string;
}

export function splitImageResult(result: unknown): { image: ImageResult | null; rest: unknown } {
  if (result && typeof result === 'object') {
    const r = result as Record<string, unknown>;
    if (typeof r['base64'] === 'string' && typeof r['format'] === 'string' && r['format'].startsWith('image/')) {
      const { base64, ...rest } = r;
      return { image: { data: base64 as string, mimeType: r['format'] }, rest };
    }
  }
  return { image: null, rest: result };
}

/** What the UI needs to show a tool result: the same object without the image bytes. */
export function stripImageData(result: unknown): unknown {
  const { image, rest } = splitImageResult(result);
  return image ? { ...(rest as Record<string, unknown>), image: `[${image.mimeType}, ${Math.round((image.data.length * 3) / 4 / 1024)} KB]` } : result;
}
