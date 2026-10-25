// Xbox listing extras (Partner Center > Store listing > Xbox images, "recommended"; supplying them avoids blank tiles):
//   xbox-branded-key-art-584x800      mark + title
//   xbox-titled-hero-art-1920x1080    mark + title, title inside the top three-quarters
//   xbox-featured-square-1080x1080    mark only, no title
//   node docs/store/art/src/xbox-extras.mjs
import fs from 'fs';
import path from 'path';
import { ART, placeMark, backdrop, title, png } from './marks.mjs';

const svg = (w, h, body) => `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}" viewBox="0 0 ${w} ${h}">${body}</svg>`;

export const jobs = [
  ['xbox-branded-key-art-584x800.png', 584, 800, () => svg(584, 800,
    backdrop(584, 800, { hx: 292, hy: 300, hr: 420 }) + placeMark(292, 300, 300) + title(292, 548, 92)), true],
  ['xbox-titled-hero-art-1920x1080.png', 1920, 1080, () => svg(1920, 1080,
    backdrop(1920, 1080, { hx: 960, hy: 360, hr: 700 }) + placeMark(960, 360, 380) + title(960, 680, 150)), true],
  ['xbox-featured-square-1080x1080.png', 1080, 1080, () => svg(1080, 1080,
    backdrop(1080, 1080, { hx: 540, hy: 540, hr: 620, halo: 0.12 }) + placeMark(540, 540, 520)), false],
];

if (process.argv[1] && path.resolve(process.argv[1]) === new URL(import.meta.url).pathname) {
  for (const [f, , , make, text] of jobs) {
    fs.writeFileSync(path.join(ART, f), png(make(), { text }));
    console.log(path.join(ART, f));
  }
}
