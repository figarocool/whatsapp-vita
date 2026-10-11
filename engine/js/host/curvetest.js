import './prelude.js';
import * as J from '../../../bridge/node_modules/curve25519-js/lib/index.js';
const N = globalThis.__nat; const U8 = (ab) => new Uint8Array(ab);
const rnd = (n) => new Uint8Array(N.randomBytes(n)); const eq = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
let bad = 0; const t = { js: [0, 0, 0, 0], nat: [0, 0, 0, 0] };
const time = (k, i, f) => { const t0 = N.mono(); const r = f(); t[k][i] += N.mono() - t0; return r; };
for (let i = 0; i < 30; i++) {
  const seed = rnd(32), seed2 = rnd(32);
  const kj = time('js', 0, () => J.generateKeyPair(seed)), kn = time('nat', 0, () => N.curveKeypair(seed));
  if (!eq(kj.public, U8(kn.public)) || !eq(kj.private, U8(kn.private))) { bad++; console.log('keypair DIVERSA', i); }
  const k2 = J.generateKeyPair(seed2);
  const sj = time('js', 1, () => J.sharedKey(kj.private, k2.public)), sn = time('nat', 1, () => N.curveShared(kj.private, k2.public));
  if (!eq(sj, U8(sn))) { bad++; console.log('shared DIVERSO', i); }
  const msg = rnd(1 + (i * 7) % 90);
  const gj = time('js', 2, () => J.sign(kj.private, msg)), gn = time('nat', 2, () => N.curveSign(kj.private, msg));
  if (!eq(gj, U8(gn))) { bad++; console.log('firma DIVERSA', i); }
  const r64 = rnd(64); const gj2 = J.sign(kj.private, msg, r64), gn2 = N.curveSign(kj.private, msg, r64);
  if (!eq(gj2, U8(gn2))) { bad++; console.log('firma con random DIVERSA', i); }
  const v1 = time('js', 3, () => J.verify(kj.public, msg, gj)), v2 = time('nat', 3, () => N.curveVerify(kj.public, msg, gj));
  const tamper = Uint8Array.from(msg); tamper[0] ^= 1;
  if (!(v1 && v2) || N.curveVerify(kj.public, tamper, gj) || J.verify(kj.public, tamper, gj) || !N.curveVerify(kj.public, msg, U8(gn2)) || !J.verify(kj.public, msg, U8(gn2))) { bad++; console.log('verifica ANOMALA', i, v1, v2); }
}
console.log(bad === 0 ? 'TUTTI I CONFRONTI UGUALI (30 giri x 5 prove)' : 'DIFFERENZE: ' + bad);
const f = (a) => (a / 30).toFixed(2) + ' ms';
console.log('ms medi  chiave | segreto | firma | verifica');
console.log('JS      ', t.js.map(f).join(' | '));
console.log('nativo  ', t.nat.map(f).join(' | '));
