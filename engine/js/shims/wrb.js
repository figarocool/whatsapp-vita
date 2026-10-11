// Sostituto di 'whatsapp-rust-bridge' (WebAssembly): servono solo queste quattro funzioni, riscritte in JavaScript.
import { createHash, createHmac } from './crypto.js';
export const md5 = (b) => new Uint8Array(createHash('md5').update(b).digest());
const hmac = (key, data) => createHmac('sha256', key).update(data).digest();
export function hkdf(ikm, length, { salt, info } = {}) {
  const s = salt && salt.length ? Buffer.from(salt) : Buffer.alloc(32);
  const prk = hmac(s, Buffer.from(ikm));
  const inf = typeof info === 'string' ? Buffer.from(info, 'utf8') : Buffer.from(info || []);
  const out = []; let prev = Buffer.alloc(0), n = 0;
  for (let i = 1; n < length; i++) { prev = hmac(prk, Buffer.concat([prev, inf, Buffer.from([i])])); out.push(prev); n += prev.length; }
  return new Uint8Array(Buffer.concat(out).subarray(0, length));
}
export function expandAppStateKeys(keyData) {
  const e = Buffer.from(hkdf(keyData, 160, { info: 'WhatsApp Mutation Keys' }));
  return { indexKey: e.subarray(0, 32), valueEncryptionKey: e.subarray(32, 64), valueMacKey: e.subarray(64, 96), snapshotMacKey: e.subarray(96, 128), patchMacKey: e.subarray(128, 160) };
}
export class LTHashAntiTampering {
  _pointwise(a, b, op) { const out = Buffer.alloc(a.length); for (let i = 0; i < a.length; i += 2) out.writeUInt16LE(op(a.readUInt16LE(i), b.readUInt16LE(i)) & 0xffff, i); return out; }
  _expand(v) { return Buffer.from(hkdf(v, 128, { info: 'WhatsApp Patch Integrity' })); }
  subtractThenAdd(base, subtract, add) {
    let h = Buffer.from(base);
    for (const s of subtract) h = this._pointwise(h, this._expand(s), (x, y) => x - y);
    for (const a of add) h = this._pointwise(h, this._expand(a), (x, y) => x + y);
    return new Uint8Array(h);
  }
}
