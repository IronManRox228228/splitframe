#!/usr/bin/env node
/**
 * Generate the Cutboard brand assets (vector logo package).
 *
 * Design: monoline geometric capitals ("CUT|BOARD" with an amber cut slash), rounded
 * terminals, near-black ink on transparent + amber mark with a sliced play glyph.
 * Pure paths — no font dependencies, renders identically everywhere.
 *
 * Outputs to assets/logo/: icon-mark.svg, wordmark-{dark,light}.svg, banner.svg,
 * banner-dark-bg.svg, icon-app.svg (+1024px PNG for the electron-builder icon).
 */
import { mkdirSync, writeFileSync, existsSync, readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { execSync } from 'node:child_process';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const outDir = join(root, 'assets', 'logo');
mkdirSync(outDir, { recursive: true });

const INK = '#16161a';
const INK_ON_AMBER = '#0b0b0d';
const AMBER = '#fbbf24';
const SW = 15; // monoline stroke width (cap height 100)

// ---------- sliced play glyph ----------
// Triangle A(0.25,0.15) B(0.25,0.85) C(0.78,0.50) cut from Q1(0.25,0.581) to Q2(0.654,0.417);
// halves offset along the cut normal to open a clean gap.
function playHalves(cx, cy, scale, gap) {
  const S = (x, y) => [cx + (x - 0.5) * scale, cy + (y - 0.5) * scale];
  const n = [0.377, 0.926]; // unit normal of the cut direction (0.86, -0.35)
  const off = (pts, sign) =>
    pts.map(([x, y]) => [x + n[0] * gap * sign, y + n[1] * gap * sign]);
  const A = [0.25, 0.15], C = [0.78, 0.5], B = [0.25, 0.85];
  const Q1 = [0.25, 0.581], Q2 = [0.654, 0.417];
  const upper = [A, Q2, Q1].map(([x, y]) => S(x, y));
  const lower = [Q1, B, C, Q2].map(([x, y]) => S(x, y));
  const poly = (pts) => pts.map(([x, y]) => `${x.toFixed(1)},${y.toFixed(1)}`).join(' ');
  return { top: poly(off(upper, -0.5)), bottom: poly(off(lower, 0.5)) };
}

/** Amber rounded square + sliced play glyph, size s at (x,y). */
function markGlyph(x, y, s = 168, { rounded = true } = {}) {
  const { top, bottom } = playHalves(x + s / 2, y + s / 2, s * 0.62, s * 0.018);
  return `  <g>
    <rect x="${x}" y="${y}" width="${s}" height="${s}"${rounded ? ` rx="${(s * 0.22).toFixed(0)}"` : ''} fill="${AMBER}"/>
    <polygon points="${top}" fill="${INK_ON_AMBER}"/>
    <polygon points="${bottom}" fill="${INK_ON_AMBER}"/>
  </g>`;
}

// ---------- wordmark: Montserrat SemiBold converted to paths (no runtime font needed) ----------
import opentype from 'opentype.js';
const FONT_PATH = join(root, 'scripts', 'fonts', 'Montserrat-SemiBold.ttf');
const fontBuf = readFileSync(FONT_PATH);
const font = opentype.parse(
  fontBuf.buffer.slice(fontBuf.byteOffset, fontBuf.byteOffset + fontBuf.byteLength),
);

/** "Cut" + amber "/" + "Board" as SVG path data. Returns { path, accentPath, width, capHeight }. */
function typesetWordmark(fontSize = 100) {
  const scale = fontSize / font.unitsPerEm;
  const word = (text, x0) => {
    const p = font.getPath(text, x0, 0, fontSize, { kerning: true });
    return { path: p.toPathData(2), width: font.getAdvanceWidth(text, fontSize, { kerning: true }) };
  };
  const cut = word('Cut', 0);
  const slashWidth = fontSize * 0.42;
  const board = word('Board', cut.width + slashWidth + fontSize * 0.06);
  // amber slash between the words
  const sx = cut.width + slashWidth / 2;
  const slash = `M ${sx + fontSize * 0.10} ${-fontSize * 0.72} L ${sx - fontSize * 0.10} ${fontSize * 0.14}`;
  return {
    path: `${cut.path} ${board.path}`,
    accentPath: slash,
    width: cut.width + slashWidth + board.width,
    baseline: 0,
    ascent: fontSize * 0.72,
    descent: fontSize * 0.14,
  };
}

const wordmark = typesetWordmark(100);

// ---------- outputs ----------
const F = 128; // wordmark font size
const wm = typesetWordmark(F);
const wmCap = F * 0.7; // Montserrat cap height ≈ 0.7em

const wordGroup = (ink, x, y) =>
  `  <g transform="translate(${x}, ${y})">
    <path d="${wm.path}" fill="${ink}"/>
    <path d="${wm.accentPath}" fill="none" stroke="${AMBER}" stroke-width="${(F * 0.085).toFixed(1)}" stroke-linecap="round"/>
  </g>`;

const PAD = 24;
const MARK = 168;
const GAP = 56;
const BW = PAD * 2 + MARK + GAP + Math.ceil(wm.width);
const BH = PAD * 2 + MARK;
const baseline = PAD + MARK / 2 + wmCap / 2;

writeFileSync(
  join(outDir, 'icon-mark.svg'),
  `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 168 168" width="168" height="168" role="img" aria-label="Cutboard">\n${markGlyph(0, 0)}\n</svg>\n`,
);

function wordmarkSvg(ink) {
  const w = Math.ceil(wm.width + 16);
  const h = Math.ceil(wmCap + wm.descent + 8);
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${w} ${h}" width="${w}" height="${h}" role="img" aria-label="Cutboard">
  <g transform="translate(8, ${wmCap + 4})">
    <path d="${wm.path}" fill="${ink}"/>
    <path d="${wm.accentPath}" fill="none" stroke="${AMBER}" stroke-width="${(F * 0.085).toFixed(1)}" stroke-linecap="round"/>
  </g>
</svg>
`;
}
writeFileSync(join(outDir, 'wordmark-dark.svg'), wordmarkSvg(INK));
writeFileSync(join(outDir, 'wordmark-light.svg'), wordmarkSvg('#f5f5f4'));

const banner = (ink) => `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${BW} ${BH}" width="${BW}" height="${BH}" role="img" aria-label="Cutboard">
  <title>Cutboard — chat-to-edit video editor</title>
${markGlyph(PAD, PAD, MARK)}
${wordGroup(ink, PAD + MARK + GAP, baseline)}
</svg>`;
writeFileSync(join(outDir, 'banner.svg'), banner(INK));
writeFileSync(join(outDir, 'banner-dark-bg.svg'), banner('#f5f5f4'));

// 1024px app icon (rounded-square amber, glyph)
const ICON = 1024;
const { top, bottom } = playHalves(ICON / 2, ICON / 2, ICON * 0.58, ICON * 0.016);
writeFileSync(
  join(outDir, 'icon-app.svg'),
  `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${ICON} ${ICON}" width="${ICON}" height="${ICON}">
  <rect width="${ICON}" height="${ICON}" rx="${(ICON * 0.225).toFixed(0)}" fill="${AMBER}"/>
  <polygon points="${top}" fill="${INK_ON_AMBER}"/>
  <polygon points="${bottom}" fill="${INK_ON_AMBER}"/>
</svg>
`,
);

console.log(`✓ wrote assets/logo/ (banner ${BW}x${BH})`);

// PNG render via QuickLook (macOS) for the electron-builder icon
if (process.platform === 'darwin') {
  try {
    execSync(`qlmanage -t -s 1024 -o "${outDir}" "${join(outDir, 'icon-app.svg')}" 2>/dev/null`);
    const png = join(outDir, 'icon-app.svg.png');
    if (existsSync(png)) {
      const buildDir = join(root, 'apps', 'desktop', 'build');
      mkdirSync(buildDir, { recursive: true });
      writeFileSync(join(buildDir, 'icon.png'), readFileSync(png));
      console.log('✓ apps/desktop/build/icon.png (electron-builder app icon)');
    }
  } catch (err) {
    console.log(`(PNG render skipped: ${err.message})`);
  }
}
