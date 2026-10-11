import esbuild from 'esbuild';
import { createRequire } from 'module';
import fs from 'fs';
const require = createRequire(import.meta.url);
const BASE = '../../bridge/node_modules';
// moduli di Node: eventi/util/buffer/path veri (npm), il resto finto (per la prova di peso)
const D = './node_modules/';
const real = { events: D + 'events/events.js', util: D + 'util/util.js', buffer: D + 'buffer/index.js', path: D + 'path-browserify/index.js' };
const builtins = ['crypto','zlib','fs','fs/promises','os','url','stream','stream/promises','async_hooks','child_process','http','https','net','tls','dns','readline','worker_threads','perf_hooks','string_decoder','timers','timers/promises','assert','constants','querystring','vm','module','process','tty','diagnostics_channel','inspector','v8','cluster','dgram','http2','punycode','repl','sys','trace_events','wasi'];
const stub = (name) => {
  let names = []; try { names = Object.keys(require('node:' + name)); } catch (_) {}
  names = names.filter((n) => /^[A-Za-z_$][\w$]*$/.test(n) && n !== 'default');
  return `const f = new Proxy(function(){}, { get: (t, k) => (k === Symbol.toPrimitive ? () => '' : k === 'prototype' ? t.prototype : f), apply: () => f, construct: () => ({}) });\n` +
    names.map((n) => /^[A-Z]/.test(n) ? `export class ${n} { constructor(){ } static [Symbol.hasInstance]() { return false; } }` : `export const ${n} = f;`).join('\n') + `\nexport default { ${names.join(', ')} };\n`;
};
const plugin = {
  name: 'node-stubs',
  setup(b) {
    b.onResolve({ filter: /^(node:)?[a-z_]+(\/[a-z_]+)?$/ }, (a) => {
      const n = a.path.replace(/^node:/, '');
      if (real[n]) return { path: real[n] };
      if (builtins.includes(n) || a.path.startsWith('node:')) return { path: n, namespace: 'stub' };
    });
    b.onLoad({ filter: /.*/, namespace: 'stub' }, (a) => ({ contents: stub(a.path), loader: 'js' }));
    b.onResolve({ filter: /libsignal\/src\/curve(\.js)?$/ }, () => ({ path: 'curve', namespace: 'curvestub' }));
    b.onLoad({ filter: /.*/, namespace: 'curvestub' }, () => ({ contents: 'const f = () => { throw new Error("curve: non implementato"); }; export const generateKeyPair = f, calculateAgreement = f, calculateSignature = f, verifySignature = f, getPublicFromPrivateKey = f, createKeyPair = f, privToPub = f; export default {};', loader: 'js' }));
    b.onResolve({ filter: /^pino$/ }, () => ({ path: 'pino', namespace: 'pinostub' }));
    b.onLoad({ filter: /.*/, namespace: 'pinostub' }, () => ({ contents: 'const mk = () => { const l = { level: "silent", child: () => mk(), trace(){}, debug(){}, info(){}, warn(){}, error(){}, fatal(){} }; return l; }; export default function pino() { return mk(); }; export const levels = {};', loader: 'js' }));
    // WebAssembly: whatsapp-rust-bridge si sostituisce con funzioni scritte a mano (prova di peso: segnaposto)
    b.onResolve({ filter: /^whatsapp-rust-bridge$/ }, () => ({ path: 'wrb', namespace: 'wrb' }));
    b.onLoad({ filter: /.*/, namespace: 'wrb' }, () => ({ contents: "export const LTHashAntiTampering = class {}; export const expandAppStateKeys = () => {}; export const md5 = () => {}; export const hkdf = () => {};", loader: 'js' }));
  },
};
const r = await esbuild.build({
  entryPoints: [BASE + '/@whiskeysockets/baileys/lib/index.js'],
  bundle: true, format: 'esm', platform: 'neutral', mainFields: ['module', 'main'], conditions: ['import', 'node'],
  outfile: 'baileys.bundle.js', inject: ['./shim.js'], plugins: [plugin], minify: process.argv.includes('--min'), logLevel: 'warning',
  define: { 'process.env.NODE_ENV': '"production"' }, banner: { js: "if (!globalThis.crypto) globalThis.crypto = { subtle: {}, getRandomValues: (a) => a, randomUUID: () => '00000000-0000-4000-8000-000000000000' }; var process = globalThis.process || { env: {}, platform: 'vita', versions: {}, nextTick: (f, ...a) => Promise.resolve().then(() => f(...a)), emitWarning() {}, on() {} };" },
  external: ['sharp', 'jimp', 'link-preview-js', 'audio-decode', 'fluent-ffmpeg'],
});
console.log('ok', fs.statSync('baileys.bundle.js').size, 'byte');
