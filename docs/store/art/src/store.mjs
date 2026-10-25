// Store listing art (Partner Center > Store listing > Store logos / Additional assets) into docs/store/art/.
//   app-tile-icon 300x300            plated mark, no text
//   box-art 1:1 1080 + 2160          Xbox requires the title, in the top two-thirds
//   poster 2:3 720x1080 + 1440x2160  REQUIRED for Xbox, title in the top two-thirds (bottom third may get overlays)
//   super-hero 16:9 1920 + 3840      NO text; mark right of centre (the Store lays text over the left side)
// Also writes mark.svg (plated master) and mark-unplated.svg.
//   node docs/store/art/src/store.mjs
import fs from 'fs';
import path from 'path';
import { ART, PAL, icon, placeMark, backdrop, title, png } from './marks.mjs';

const svg = (w, h, body) => `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}" viewBox="0 0 ${w} ${h}">${body}</svg>`;

// Mark above the title, the pair centred on `cy` (fraction of h). k = canvas unit (min side).
const titled = (w, h, { cy, markW, size }) => {
  const k = Math.min(w, h);
  const mw = k * markW, fs_ = k * size;
  const markH = mw * 50.3 / 64;            // visible mark height (marks.mjs bbox)
  const gap = k * 0.07;
  const capH = fs_ * 0.73;                  // Inter cap height
  const total = markH + gap + capH;
  const top = h * cy - total / 2;
  const my = top + markH / 2;
  const base = top + markH + gap + capH;
  return backdrop(w, h, { hx: w / 2, hy: my, hr: k * 0.6 }) + placeMark(w / 2, my, mw) + title(w / 2, base, fs_);
};

export const jobs = [
  ['app-tile-icon-300x300.png', 300, 300, () => icon(300, 300, { markFrac: 0.56 }), false],
  ...[1080, 2160].map((n) => [`box-art-1x1-${n}x${n}.png`, n, n, () => svg(n, n, titled(n, n, { cy: 0.4, markW: 0.42, size: 0.12 })), true]),
  ...[[720, 1080], [1440, 2160]].map(([w, h]) => [`poster-art-2x3-${w}x${h}.png`, w, h, () => svg(w, h, titled(w, h, { cy: 0.38, markW: 0.5, size: 0.16 })), true]),
  ...[[1920, 1080], [3840, 2160]].map(([w, h]) => [`super-hero-art-16x9-${w}x${h}.png`, w, h,
    () => svg(w, h, backdrop(w, h, { hx: w * 0.66, hy: h * 0.5, hr: h * 0.75, halo: 0.12 }) + placeMark(w * 0.66, h * 0.5, h * 0.42)), false]),
];

if (process.argv[1] && path.resolve(process.argv[1]) === new URL(import.meta.url).pathname) {
  for (const [f, , , make, text] of jobs) {
    fs.writeFileSync(path.join(ART, f), png(make(), { text }));
    console.log(path.join(ART, f));
  }
  fs.writeFileSync(path.join(ART, 'mark.svg'), icon(1024, 1024, { markFrac: 0.56 }) + '\n');
  fs.writeFileSync(path.join(ART, 'mark-unplated.svg'), icon(1024, 1024, { markFrac: 0.94, plate: false }) + '\n');
  console.log(path.join(ART, 'mark.svg'));
}
export { PAL };
