// Review sheet docs/store/art/concepts.png: the mark at 300 / 44 / 24 px (plated + unplated, 1:1 and 4x
// nearest-neighbour zoom), the light-theme taskbar icon, the wide tile, the poster and the super-hero art.
// Run after build.mjs and store.mjs (it reads their outputs).
//   node docs/store/art/src/concepts.mjs
import fs from 'fs';
import path from 'path';
import { ART, ASSETS, PAL, icon, png } from './marks.mjs';

const emb = (b) => 'data:image/png;base64,' + Buffer.from(b).toString('base64');
const file = (p) => emb(fs.readFileSync(p));
const W = 2200, H = 1300;
const lbl = (x, y, t, size = 22, fill = '#9a9aa6') => `<text x="${x}" y="${y}" fill="${fill}" font-family="Inter Display" font-size="${size}" font-weight="600">${t}</text>`;
const img = (x, y, w, h, href, pix = false) => `<image x="${x}" y="${y}" width="${w}" height="${h}" href="${href}"${pix ? ' image-rendering="optimizeSpeed"' : ''}/>`;

let b = `<rect width="${W}" height="${H}" fill="#2a2a30"/>` + lbl(40, 56, 'Airtime  mark, package icons and listing art', 30, PAL.title);
// Row 1: sizes.
const i300 = file(path.join(ART, 'app-tile-icon-300x300.png'));
const i44 = file(path.join(ASSETS, 'Square44x44Logo.scale-100.png'));
const i24 = file(path.join(ASSETS, 'Square44x44Logo.targetsize-24.png'));
const u24 = file(path.join(ASSETS, 'Square44x44Logo.targetsize-24_altform-unplated.png'));
const l24 = file(path.join(ASSETS, 'Square44x44Logo.targetsize-24_altform-lightunplated.png'));
const u48 = file(path.join(ASSETS, 'Square44x44Logo.targetsize-48_altform-unplated.png'));
b += img(40, 100, 300, 300, i300) + lbl(40, 430, '300 (app tile icon)');
b += img(380, 100, 44, 44, i44) + img(440, 100, 24, 24, i24) + lbl(380, 180, '44, 24 at 1:1');
b += img(380, 210, 176, 176, i44, true) + img(576, 210, 96, 96, i24, true) + lbl(380, 430, '44 and 24, 4x zoom');
b += `<rect x="710" y="100" width="300" height="140" fill="#202024"/>` + img(730, 120, 48, 48, u48) + img(800, 132, 24, 24, u24) + img(840, 120, 96, 96, u24, true) + lbl(710, 270, 'taskbar, dark (48, 24, 4x)');
b += `<rect x="710" y="290" width="300" height="120" fill="#f3f3f3"/>` + img(730, 326, 24, 24, l24) + img(780, 302, 96, 96, l24, true) + lbl(710, 440, 'taskbar, light theme');
b += img(1050, 100, 620, 300, file(path.join(ASSETS, 'Wide310x150Logo.scale-200.png'))) + lbl(1050, 430, 'Wide310x150 (scale-200)');
b += img(1710, 100, 300, 300, file(path.join(ASSETS, 'SmallTile.scale-400.png'))) + lbl(1710, 430, 'SmallTile (scale-400)');
// Row 2: listing art.
b += img(40, 480, 520, 780, file(path.join(ART, 'poster-art-2x3-720x1080.png'))) + `<line x1="40" y1="${480 + 520}" x2="560" y2="${480 + 520}" stroke="#ff5a5a" stroke-dasharray="8 8" stroke-width="2"/>` + lbl(40, 1290, 'poster 2:3 (dashed: bottom third, overlays may cover it)');
b += img(600, 480, 1386, 780, file(path.join(ART, 'super-hero-art-16x9-1920x1080.png'))) + lbl(600, 1290, 'super hero 16:9 (no text)');

fs.writeFileSync(path.join(ART, 'concepts.png'), png(`<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${H}" viewBox="0 0 ${W} ${H}">${b}</svg>`, { text: true }));
console.log(path.join(ART, 'concepts.png'));
