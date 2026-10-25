// Airtime mark + shared helpers for every art script in this folder.
//
// Mark: a white play triangle (rounded joins) with two broadcast arcs to its right in the app accent, the outer arc
// dimmer. Drawn in a 100x100 unit box centred at 0,0 (the group is optically centred: bbox centre nudged right
// because the solid triangle weighs more than the thin arcs).
// At <= 32 px the outer arc and the thin strokes would turn to mush, so the small variant uses one heavier arc.
//
// The scripts run from any directory where `npm install @resvg/resvg-js` was done (deps are resolved from the current
// directory, nothing is installed in the repo). Title text uses Inter Display SemiBold (SIL OFL), not committed:
// set ART_FONT=/path/InterDisplay-SemiBold.ttf or put the Inter release zip's tree in the current directory
// (./inter/extras/ttf/InterDisplay-SemiBold.ttf). docs/building.md "Art" has the full command.
import { createRequire } from 'module';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const require = createRequire(path.join(process.cwd(), 'noop.js'));
export const { Resvg } = require('@resvg/resvg-js');

export const HERE = path.dirname(fileURLToPath(import.meta.url));
export const REPO = path.resolve(HERE, '../../../..');
export const ART = path.resolve(HERE, '..');            // docs/store/art
export const ASSETS = path.join(REPO, 'app', 'Assets');

export const PAL = {
  bg: '#07070B',      // plate (near black, matches the app's idle background)
  lift: '#14141B',    // very subtle top lift on large art (= PanelSolidBrush)
  fg: '#FFFFFF',      // triangle
  title: '#F5F5F7',   // TextBrush
  accent: '#8EA8FF',  // AccentBrush
  ink: '#0B0B10',     // light-theme unplated (taskbar on a light background)
  inkAccent: '#3F5BD9',
};

// Geometry in the original 0..100 drawing space; G() maps it to the centred unit box.
// Optical centring: the solid triangle outweighs the thin arcs (ink centroid ~6.6u left of the bbox centre), so the
// bbox centre sits right of the canvas centre by a quarter of that offset (perceived centre = bbox + 0.25 * offset).
// Measured with a bbox/centroid probe; re-measure if the geometry changes.
const OX = 51.05, OY = 50;   // full: bbox x 18.0..82.0 -> centre lands at +1.65u
const SX = 48.55;            // small: bbox x 17.8..80.2 -> centre lands at +1.15u
const deg = (a) => (a * Math.PI) / 180;
const arc = (cx, cy, r, a) => {
  const x = (cx + r * Math.cos(deg(a))).toFixed(2), y0 = (cy - r * Math.sin(deg(a))).toFixed(2), y1 = (cy + r * Math.sin(deg(a))).toFixed(2);
  return `M${x} ${y0} A${r} ${r} 0 0 1 ${x} ${y1}`;
};

// variant: 'full' (two arcs) | 'small' (one arc, heavier strokes). col/acc override colours (light unplated).
export function markBody({ variant = 'full', col = PAL.fg, acc = PAL.accent } = {}) {
  if (variant === 'small') {
    // One arc, everything ~1.5x heavier; re-centred on its own bbox.
    const tri = `<path d="M22 30 L22 70 L54 50 Z" fill="${col}" stroke="${col}" stroke-width="7" stroke-linejoin="round"/>`;
    const a1 = `<path d="${arc(50, 50, 26, 46)}" fill="none" stroke="${acc}" stroke-width="10" stroke-linecap="round"/>`;
    return `<g transform="translate(${-SX} ${-OY})">${tri}${a1}</g>`;
  }
  const tri = `<path d="M23 31 L23 69 L54 50 Z" fill="${col}" stroke="${col}" stroke-width="4.5" stroke-linejoin="round"/>`;
  const a1 = `<path d="${arc(50.5, 50, 18, 45)}" fill="none" stroke="${acc}" stroke-width="6.5" stroke-linecap="round"/>`;
  const a2 = `<path d="${arc(50.5, 50, 31, 45)}" fill="none" stroke="${acc}" stroke-width="6.5" stroke-linecap="round" opacity=".6"/>`;
  return `<g transform="translate(${-OX} ${-OY})">${tri}${a1}${a2}</g>`;
}

// Visible width of each variant in units (stroke included); used so callers size the mark by what you see.
export const MARK_W = { full: 64.0, small: 62.4 };
// Below this rendered width (px) the two arcs smear into one blur (1 px strokes, 1 px gap): use the one-arc variant.
// 32 px icons and smaller get it; 44 px and up keep both arcs.
export const SMALL_BELOW = 28;
export const pickVariant = (widthPx) => (widthPx < SMALL_BELOW ? 'small' : 'full');

// The mark centred at (cx, cy), `width` px wide (visible extent).
export function placeMark(cx, cy, width, o = {}) {
  const variant = o.variant || pickVariant(width);
  return `<g transform="translate(${cx} ${cy}) scale(${(width / MARK_W[variant]).toFixed(5)})">${markBody({ ...o, variant })}</g>`;
}

// A complete plated (or transparent) icon. markFrac = visible mark width as a fraction of min(w, h).
export function icon(w, h, { markFrac = 0.5, plate = true, variant, col, acc, rounded = 0 } = {}) {
  const bg = plate ? `<rect width="${w}" height="${h}" rx="${rounded}" fill="${PAL.bg}"/>` : '';
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}" viewBox="0 0 ${w} ${h}">${bg}${placeMark(w / 2, h / 2, Math.min(w, h) * markFrac, { variant, col, acc })}</svg>`;
}

// Large-canvas background: flat plate, a barely visible top lift and a faint accent halo at (hx, hy) radius hr.
export function backdrop(w, h, { hx = w / 2, hy = h / 2, hr = Math.min(w, h) * 0.5, halo = 0.10 } = {}) {
  return `<defs><linearGradient id="lift" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${PAL.lift}"/><stop offset="0.65" stop-color="${PAL.bg}"/></linearGradient>
<radialGradient id="halo" gradientUnits="userSpaceOnUse" cx="${hx}" cy="${hy}" r="${hr}"><stop offset="0" stop-color="${PAL.accent}" stop-opacity="${halo}"/><stop offset="1" stop-color="${PAL.accent}" stop-opacity="0"/></radialGradient></defs>
<rect width="${w}" height="${h}" fill="url(#lift)"/><rect width="${w}" height="${h}" fill="url(#halo)"/>`;
}

export const NAME = 'Airtime';
export const title = (x, y, size, { anchor = 'middle', text = NAME, fill = PAL.title } = {}) =>
  `<text x="${x}" y="${y}" text-anchor="${anchor}" fill="${fill}" font-family="Inter Display" font-size="${size}" font-weight="600" letter-spacing="${(-size * 0.02).toFixed(2)}">${text}</text>`;

export function fontFile() {
  const f = process.env.ART_FONT || path.join(process.cwd(), 'inter/extras/ttf/InterDisplay-SemiBold.ttf');
  if (!fs.existsSync(f)) {
    console.error(`title font not found: ${f}\nDownload Inter (github.com/rsms/inter/releases, SIL OFL) and set ART_FONT to InterDisplay-SemiBold.ttf`);
    process.exit(1);
  }
  return f;
}

// Render an SVG string to PNG bytes. Text only renders with the Inter file (no system font fallback, so a missing
// font can't silently change the title's look).
export function png(svg, { text = false, fitWidth } = {}) {
  const opts = { font: text ? { fontFiles: [fontFile()], loadSystemFonts: false, defaultFontFamily: 'Inter Display' } : { loadSystemFonts: false } };
  if (fitWidth) opts.fitTo = { mode: 'width', value: fitWidth };
  return new Resvg(svg, opts).render().asPng();
}

export const pngSize = (buf) => [buf.readUInt32BE(16), buf.readUInt32BE(20)];
