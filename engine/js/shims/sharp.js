// Sostituto di 'sharp' (CommonJS: il bridge lo chiama come funzione). Decodifica/ridimensiona/ricodifica in C (libjpeg, libpng, libwebp).
const N = globalThis.__nat;
function sharp(input) {
  const st = { input, w: 0, h: 0, fit: 0, fmt: 0, q: 80, rot: 0 };
  const c = {
    rotate() { st.rot = 1; return c; },
    resize(w, h, o) { if (w && typeof w === 'object') { o = w; h = w.height; w = w.width; } st.w = w || 0; st.h = h || 0; st.fit = o && o.fit === 'cover' ? 1 : 0; return c; },
    jpeg(o) { st.fmt = 0; st.q = (o && o.quality) || 80; return c; }, png() { st.fmt = 1; return c; }, webp(o) { st.fmt = 0; st.q = (o && o.quality) || 80; return c; },
    async toBuffer() { const r = N.imgConvert(st.input, st.w, st.h, st.fit, st.fmt, st.q, st.rot); if (!r) throw new Error('Input buffer contains unsupported image format'); return Buffer.from(r.data); },
    async metadata() { const i = N.imgInfo(st.input); if (!i) throw new Error('Input buffer contains unsupported image format'); return i; },
  };
  return c;
}
module.exports = sharp; module.exports.default = sharp;
