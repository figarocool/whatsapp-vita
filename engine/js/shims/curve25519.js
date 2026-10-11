// Sostituto di 'curve25519-js': stessa interfaccia, calcoli in C (mbedTLS per SHA-512).
const N = globalThis.__nat; const u8 = (ab) => new Uint8Array(ab);
exports.generateKeyPair = (seed) => { const k = N.curveKeypair(seed); return { public: u8(k.public), private: u8(k.private) }; };
exports.sharedKey = (sk, pk) => u8(N.curveShared(sk, pk));
exports.sign = (sk, msg, rnd) => u8(N.curveSign(sk, msg, rnd));
exports.verify = (pk, msg, sig) => N.curveVerify(pk, msg, sig);
exports.signMessage = (sk, msg, rnd) => { const s = u8(N.curveSign(sk, msg, rnd)); const out = new Uint8Array(64 + msg.length); out.set(s); out.set(msg, 64); return out; };
exports.openMessage = (pk, sm) => (N.curveVerify(pk, sm.subarray(64), sm.subarray(0, 64)) ? sm.slice(64) : null);
exports.default = exports;
