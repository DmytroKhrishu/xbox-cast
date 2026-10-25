// Package art: re-renders every PNG in app/Assets at its EXISTING pixel size and file name (MRT picks the qualified
// variants; the manifest references the unqualified names). Adds the light-theme unplated taskbar icons
// (Square44x44Logo.targetsize-N_altform-lightunplated.png) next to the existing unplated ones.
//   node docs/store/art/src/build.mjs
import fs from 'fs';
import path from 'path';
import { ASSETS, PAL, icon, png, pngSize } from './marks.mjs';

const small = (n) => n <= 32;
const opts = (f, w, h) => {
  const n = Math.min(w, h);
  if (f.startsWith('Square44x44Logo')) {
    if (f.includes('lightunplated')) return { plate: false, col: PAL.ink, acc: PAL.inkAccent, markFrac: small(n) ? 0.94 : 0.86 };
    if (f.includes('unplated')) return { plate: false, markFrac: small(n) ? 0.94 : 0.86 };
    return { markFrac: small(n) ? 0.74 : 0.68 };
  }
  if (f.startsWith('SplashScreen')) return { plate: false, markFrac: 0.4 };   // on the manifest splash colour (#07070B)
  if (f.startsWith('Wide310x150')) return { markFrac: 0.48 };
  if (f.startsWith('LargeTile')) return { markFrac: 0.44 };
  if (f.startsWith('SmallTile')) return { markFrac: 0.6 };
  if (f.startsWith('StoreLogo')) return { markFrac: 0.66 };
  return { markFrac: 0.5 };                                                // Square150x150
};

// Light-theme unplated variants: one per existing unplated target size.
for (const f of fs.readdirSync(ASSETS)) {
  const m = f.match(/^Square44x44Logo\.targetsize-(\d+)_altform-unplated\.png$/);
  const light = m && `Square44x44Logo.targetsize-${m[1]}_altform-lightunplated.png`;
  if (light && !fs.existsSync(path.join(ASSETS, light))) fs.copyFileSync(path.join(ASSETS, f), path.join(ASSETS, light));
}

const out = [];
let bad = 0;
for (const f of fs.readdirSync(ASSETS).filter((f) => f.endsWith('.png')).sort()) {
  const p = path.join(ASSETS, f);
  const [w, h] = pngSize(fs.readFileSync(p));
  fs.writeFileSync(p, png(icon(w, h, opts(f, w, h))));
  const [w2, h2] = pngSize(fs.readFileSync(p));
  if (w2 !== w || h2 !== h) bad++;
  out.push(`${f} ${w2}x${h2}${w2 !== w || h2 !== h ? ' SIZE MISMATCH' : ''}`);
}
console.log(out.join('\n'));
console.log(`${out.length} files in ${ASSETS}`);
if (bad) process.exit(1);
