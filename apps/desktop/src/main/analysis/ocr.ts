import { spawn } from 'node:child_process';

/**
 * On-screen text via the OCR engine built into Windows 10/11 (Windows.Media.Ocr). Nothing to
 * download: Windows PowerShell 5.1 can load the WinRT types. The script reads image paths from
 * an environment variable (base64 JSON, avoiding quoting and encoding trouble) and writes
 * base64 JSON `[[path, text], ...]`. Returns null when OCR is unavailable (non-Windows, no
 * recognizer language installed, PowerShell failure) so callers just skip it.
 */

const OCR_SCRIPT = `
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
Add-Type -AssemblyName System.Runtime.WindowsRuntime
[void][Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType = WindowsRuntime]
[void][Windows.Graphics.Imaging.BitmapDecoder, Windows.Foundation, ContentType = WindowsRuntime]
[void][Windows.Storage.StorageFile, Windows.Foundation, ContentType = WindowsRuntime]
$asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
  $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation\`1'
} | Select-Object -First 1
function Await($op, $type) {
  $task = $asTask.MakeGenericMethod($type).Invoke($null, @($op))
  $task.Wait(-1) | Out-Null
  $task.Result
}
$engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
if ($null -eq $engine) { [Console]::Out.Write('NOENGINE'); exit 0 }
$json = [System.Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($env:SF_OCR_PATHS))
$paths = ConvertFrom-Json $json
$out = @()
foreach ($p in $paths) {
  $text = ''
  try {
    $file = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($p)) ([Windows.Storage.StorageFile])
    $stream = Await ($file.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
    $decoder = Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
    $bmp = Await ($decoder.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
    $res = Await ($engine.RecognizeAsync($bmp)) ([Windows.Media.Ocr.OcrResult])
    $text = $res.Text
    $stream.Dispose()
  } catch { $text = '' }
  $out += ,@($p, $text)
}
$result = ConvertTo-Json -InputObject $out -Compress -Depth 4
[Console]::Out.Write([Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($result)))
`;

/** Decodes the script's stdout. Null means "OCR unavailable" (no engine, or garbage output). */
export function parseOcrOutput(stdout: string): Map<string, string> | null {
  const trimmed = stdout.trim();
  if (!trimmed || trimmed === 'NOENGINE') return null;
  try {
    const rows = JSON.parse(Buffer.from(trimmed, 'base64').toString('utf8')) as unknown;
    if (!Array.isArray(rows)) return null;
    const out = new Map<string, string>();
    for (const row of rows) {
      if (Array.isArray(row) && typeof row[0] === 'string' && typeof row[1] === 'string') {
        out.set(row[0], cleanOcrText(row[1]));
      }
    }
    return out;
  } catch {
    return null;
  }
}

/** Collapses whitespace and drops results that are only stray symbols (OCR noise on photos). */
export function cleanOcrText(text: string): string {
  const flat = text.replace(/\s+/g, ' ').trim();
  const letters = flat.match(/[\p{L}\p{N}]/gu)?.length ?? 0;
  return letters >= 3 ? flat : '';
}

const BATCH = 60;
const TIMEOUT_MS = 60_000;

function runBatch(paths: string[], signal?: AbortSignal): Promise<Map<string, string> | null> {
  return new Promise((resolve) => {
    const encoded = Buffer.from(OCR_SCRIPT, 'utf16le').toString('base64');
    const child = spawn(
      'powershell.exe',
      ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', encoded],
      {
        windowsHide: true,
        stdio: ['ignore', 'pipe', 'ignore'],
        env: { ...process.env, SF_OCR_PATHS: Buffer.from(JSON.stringify(paths), 'utf8').toString('base64') },
      },
    );
    let stdout = '';
    const timer = setTimeout(() => child.kill(), TIMEOUT_MS);
    const onAbort = (): void => void child.kill();
    signal?.addEventListener('abort', onAbort);
    child.stdout.on('data', (d) => (stdout += String(d)));
    const done = (): void => {
      clearTimeout(timer);
      signal?.removeEventListener('abort', onAbort);
    };
    child.on('error', () => {
      done();
      resolve(null);
    });
    child.on('close', () => {
      done();
      resolve(parseOcrOutput(stdout));
    });
  });
}

/** Text per image path; null when OCR is unavailable on this machine. */
export async function ocrImages(paths: string[], signal?: AbortSignal): Promise<Map<string, string> | null> {
  if (process.platform !== 'win32' || paths.length === 0) return null;
  const all = new Map<string, string>();
  for (let i = 0; i < paths.length; i += BATCH) {
    if (signal?.aborted) return all.size > 0 ? all : null;
    const part = await runBatch(paths.slice(i, i + BATCH), signal);
    if (!part) return all.size > 0 ? all : null;
    for (const [k, v] of part) all.set(k, v);
  }
  return all;
}
