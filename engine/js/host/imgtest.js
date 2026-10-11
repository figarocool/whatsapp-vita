import './prelude.js';
import sharp from 'sharp';
const N = globalThis.__nat; const rd = (p) => Buffer.from(N.fsRead(p));
(async () => {
  for (const [f, w, h, fit, fmt] of [['big.jpg', 480, 480, 'inside', 'jpeg'], ['rot.jpg', 480, 480, 'inside', 'jpeg'], ['alpha.png', 192, 192, 'inside', 'png'], ['sticker.webp', 192, 192, 'inside', 'png'], ['anim.webp', 192, 192, 'inside', 'png'], ['photo.png', 640, 640, 'cover', 'jpeg'], ['small.jpg', 480, 480, 'inside', 'jpeg']]) {
    const buf = rd('/tmp/imgtest/' + f); const t0 = N.mono();
    try { const meta = await sharp(buf).metadata(); let s = sharp(buf).rotate().resize(w, h, { fit, withoutEnlargement: true }); s = fmt === 'png' ? s.png() : s.jpeg({ quality: 80 }); const out = await s.toBuffer(); N.fsWrite('/tmp/imgtest/out_' + f + '.' + (fmt === 'png' ? 'png' : 'jpg'), out); const m2 = await sharp(out).metadata(); console.log(f, meta.format, meta.width + 'x' + meta.height, '->', m2.format, m2.width + 'x' + m2.height, out.length, 'byte', (N.mono() - t0).toFixed(0), 'ms'); }
    catch (e) { console.log(f, 'ERRORE', e.message); }
  }
})();
