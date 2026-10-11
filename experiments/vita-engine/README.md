# Esperimento: motore WhatsApp dentro la Vita (senza PC)

Stato: **solo esplorazione**, niente di questo e' nell'app pubblicata.

Idea: impacchettare Baileys in un solo file JavaScript e farlo girare su QuickJS dentro l'app, con una parte nativa in C
(cifratura, WebSocket sicuro, file, timer) al posto dei moduli di Node.

Risultati finora (misurati sul PC, non ancora sulla Vita):
- `build.mjs` impacchetta Baileys (3,4 MB minificato) con esbuild, sostituendo pino, la parte WebAssembly (whatsapp-rust-bridge),
  la curva crittografica e i moduli di Node con finti segnaposto.
- In QuickJS il file si carica in ~0,2 s con ~35 MB di memoria (`t1.mjs`).
- Mancava solo `globalThis.crypto` (WebCrypto): segnaposto aggiunto.

Da fare: crittografia nativa (AES-GCM/CTR/CBC, HMAC, SHA, MD5, curve25519/ed25519), WebSocket TLS, zlib, file, timer,
riscrivere 4 funzioni di whatsapp-rust-bridge (md5, hkdf, LTHash, expandAppStateKeys), poi test su Vita.

Uso: `npm i esbuild events util buffer path-browserify` in questa cartella, poi `node build.mjs --min`.
