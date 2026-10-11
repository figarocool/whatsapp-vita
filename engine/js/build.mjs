// Impacchetta Baileys + i nostri sostituti di Node in un solo file per il motore QuickJS.
import esbuild from 'esbuild';
import { createRequire } from 'module';
import path from 'path';
import { fileURLToPath } from 'url';
const here = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);
const NM = path.join(here, 'node_modules') + '/', BR = path.join(here, '../../bridge/node_modules') + '/';
const S = (n) => path.join(here, 'shims', n);
const real = { events: NM + 'events/events.js', util: NM + 'util/util.js', buffer: NM + 'buffer/index.js', path: NM + 'path-browserify/index.js',
  crypto: S('crypto.js'), zlib: S('zlib.js'), fs: S('fs.js'), 'fs/promises': S('fs_promises.js'), os: S('os.js'), ws: S('ws.js'), async_hooks: S('async_hooks.js'), url: S('url.js') };
const stubbed = ['stream','stream/promises','child_process','http','https','net','tls','dns','readline','worker_threads','perf_hooks','string_decoder','timers','timers/promises','assert','constants','querystring','vm','module','process','tty','diagnostics_channel','inspector','v8','cluster','dgram','http2','punycode','repl','sys','trace_events','wasi'];
const stub = (name) => {
  let names = []; try { names = Object.keys(require('node:' + name)); } catch (_) {}
  names = names.filter((n) => /^[A-Za-z_$][\w$]*$/.test(n) && n !== 'default');
  return `const f = new Proxy(function(){}, { get: (t, k) => (k === Symbol.toPrimitive ? () => '' : k === 'prototype' ? t.prototype : f), apply: () => f, construct: () => ({}) });\n` +
    names.map((n) => (/^[A-Z]/.test(n) ? `export class ${n} { constructor(){ } static [Symbol.hasInstance]() { return false; } }` : `export const ${n} = f;`)).join('\n') + `\nexport default { ${names.join(', ')} };\n`;
};
const plugin = { name: 'vita-node', setup(b) {
  b.onResolve({ filter: /^(node:)?[a-z_]+(\/[a-z_]+)?$/ }, (a) => { const n = a.path.replace(/^node:/, ''); if (real[n]) return { path: real[n] }; if (stubbed.includes(n) || a.path.startsWith('node:')) return { path: n, namespace: 'stub' }; });
  b.onLoad({ filter: /.*/, namespace: 'stub' }, (a) => ({ contents: stub(a.path), loader: 'js' }));
  b.onResolve({ filter: /^whatsapp-rust-bridge$/ }, () => ({ path: S('wrb.js') }));
  b.onResolve({ filter: /^pino$/ }, () => ({ path: 'pino', namespace: 'pinostub' }));
  b.onLoad({ filter: /.*/, namespace: 'pinostub' }, () => ({ contents: 'const mk = () => { const l = { level: "silent", child: () => mk(), trace(){}, debug(){}, info(){}, warn(){}, error(){}, fatal(){} }; return l; }; export default function pino() { return mk(); }; export const levels = {};', loader: 'js' }));
  b.onResolve({ filter: /^@whiskeysockets\/baileys$/ }, () => ({ path: BR + '@whiskeysockets/baileys/lib/index.js' }));
} };
const out = path.join(here, process.argv[2] || 'engine.bundle.js');
const r = await esbuild.build({ entryPoints: [path.join(here, 'host/main.js')], bundle: true, format: 'esm', platform: 'neutral', mainFields: ['module', 'main'], conditions: ['import', 'node'],
  outfile: out, plugins: [plugin], minify: process.argv.includes('--min'), logLevel: 'warning', define: { 'process.env.NODE_ENV': '"production"' },
  nodePaths: [NM, BR], external: ['sharp', 'jimp', 'link-preview-js', 'audio-decode', 'fluent-ffmpeg'] });
console.log('creato', out);
