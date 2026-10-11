// Sostituto veloce di 'qrcode': il bridge usa solo toBuffer(testo, { width, margin }). Genera un PNG in scala di grigi leggero:
// la libreria originale (pngjs) in JavaScript sulla CPU della Vita ci metteva troppo.
const core = require('../../../bridge/node_modules/qrcode/lib/core/qrcode.js');
const N = globalThis.__nat;
const be32 = (n) => Buffer.from([(n >>> 24) & 255, (n >>> 16) & 255, (n >>> 8) & 255, n & 255]);
const chunk = (type, data) => { const t = Buffer.from(type, 'latin1'); const body = Buffer.concat([t, data]); return Buffer.concat([be32(data.length), body, be32(N.crc32(body))]); };
function toBuffer(text, opts = {}) {
  const qr = core.create(text, {}); const size = qr.modules.size, margin = opts.margin === undefined ? 4 : opts.margin;
  const total = size + 2 * margin, scale = Math.max(1, Math.floor((opts.width || 200) / total)), w = total * scale;
  const raw = Buffer.alloc((w + 1) * w, 255); const row = Buffer.alloc(w + 1, 255); row[0] = 0;
  for (let r = 0; r < total; r++) {
    row.fill(255, 1);
    for (let c = 0; c < total; c++) if (r >= margin && r < margin + size && c >= margin && c < margin + size && qr.modules.get(r - margin, c - margin)) row.fill(0, 1 + c * scale, 1 + (c + 1) * scale);
    for (let k = 0; k < scale; k++) row.copy(raw, (r * scale + k) * (w + 1));
  }
  const ihdr = Buffer.concat([be32(w), be32(w), Buffer.from([8, 0, 0, 0, 0])]);   // 8 bit, scala di grigi
  return Promise.resolve(Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', Buffer.from(N.deflate(raw))), chunk('IEND', Buffer.alloc(0))]));
}
module.exports = { toBuffer, toDataURL: async (t, o) => 'data:image/png;base64,' + (await toBuffer(t, o)).toString('base64') };
