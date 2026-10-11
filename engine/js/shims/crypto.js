// Sostituto del modulo 'crypto' di Node: appoggiato alla parte nativa (mbedTLS).
const N = globalThis.__nat;
const bytes = (d, enc) => (typeof d === 'string' ? Buffer.from(d, enc) : d);
export class Hash {
  constructor(alg, key) { this.h = N.hashNew(alg, key === undefined ? undefined : bytes(key)); }
  update(d, enc) { N.hashUpdate(this.h, bytes(d, enc)); return this; }
  digest(enc) { const b = Buffer.from(N.hashFinal(this.h)); return enc ? b.toString(enc) : b; }
}
export const createHash = (alg) => new Hash(alg);
export const createHmac = (alg, key) => new Hash(alg, key);
class Cipher {
  constructor(mode, enc, key, iv) { this.mode = mode; this.enc = enc; this.h = N.cipherNew(mode, enc, bytes(key), bytes(iv)); this.tag = null; }
  setAAD(b) { N.cipherAad(this.h, bytes(b)); return this; }
  setAuthTag(t) { N.cipherTag(this.h, bytes(t)); return this; }
  setAutoPadding() { return this; }
  update(d, inEnc, outEnc) { const r = Buffer.from(N.cipherUpdate(this.h, bytes(d, inEnc))); return outEnc ? r.toString(outEnc) : r; }
  final(outEnc) {
    const r = N.cipherFinal(this.h);
    if (this.mode === 'gcm' && this.enc) { this.tag = Buffer.from(r.tag); const d = Buffer.from(r.data); return outEnc ? d.toString(outEnc) : d; }
    const b = Buffer.from(r); return outEnc ? b.toString(outEnc) : b;
  }
  getAuthTag() { return this.tag; }
}
const modeOf = (alg) => { const m = /^aes-(?:128|256)-(cbc|ctr|gcm)$/.exec(String(alg).toLowerCase()); if (!m) throw new Error('Unknown cipher: ' + alg); return m[1]; };
export const createCipheriv = (alg, key, iv) => new Cipher(modeOf(alg), true, key, iv);
export const createDecipheriv = (alg, key, iv) => new Cipher(modeOf(alg), false, key, iv);
export const randomBytes = (n, cb) => { const b = Buffer.from(N.randomBytes(n)); if (cb) { setTimeout(() => cb(null, b), 0); return; } return b; };
export const randomFillSync = (buf) => { const r = new Uint8Array(N.randomBytes(buf.byteLength)); new Uint8Array(buf.buffer || buf, buf.byteOffset || 0, buf.byteLength).set(r); return buf; };
export const randomUUID = () => { const b = new Uint8Array(N.randomBytes(16)); b[6] = (b[6] & 15) | 64; b[8] = (b[8] & 63) | 128; const h = [...b].map((x) => x.toString(16).padStart(2, '0')).join(''); return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`; };
export const randomInt = (a, b) => { const [lo, hi] = b === undefined ? [0, a] : [a, b]; const x = new Uint32Array(N.randomBytes(4))[0]; return lo + (x % (hi - lo)); };
export const getRandomValues = (a) => randomFillSync(a);
export const webcrypto = { getRandomValues, subtle: {}, randomUUID };
export default { createHash, createHmac, createCipheriv, createDecipheriv, randomBytes, randomFillSync, randomUUID, randomInt, getRandomValues, webcrypto, Hash };
