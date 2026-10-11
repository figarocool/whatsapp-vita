import './prelude.js';
import sharp from 'sharp';
const N = globalThis.__nat;
(async () => { const buf = Buffer.from(N.fsRead('/tmp/imgtest/th.jpg'));
  for (const [w, h] of [[320, null], [320, 0], [0, 0]]) { try { const o = await sharp(buf).resize(w, h).jpeg({ quality: 80 }).toBuffer(); console.log('resize', w, h, '->', o.length, 'byte'); } catch (e) { console.log('resize', w, h, 'ERRORE', e.message); } }
})();
