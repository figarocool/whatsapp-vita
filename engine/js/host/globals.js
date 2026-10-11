// Oggetti globali che nel browser/Node esistono e in QuickJS no.
if (!globalThis.global) { globalThis.global = globalThis; globalThis.self = globalThis; }
import { Buffer } from 'buffer';
globalThis.Buffer = Buffer;
// la versione npm di Buffer non conosce 'base64url' (Node si': lo aggiungiamo)
{
  const ts = Buffer.prototype.toString, fr = Buffer.from, isEnc = Buffer.isEncoding;
  Buffer.prototype.toString = function (enc, a, b) { if (enc === 'base64url') return ts.call(this, 'base64', a, b).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, ''); return ts.call(this, enc, a, b); };
  Buffer.from = function (v, enc, len) { if (typeof v === 'string' && enc === 'base64url') { let t = v.replace(/-/g, '+').replace(/_/g, '/'); while (t.length % 4) t += '='; return fr.call(Buffer, t, 'base64'); } return fr.call(Buffer, v, enc, len); };
  Buffer.isEncoding = (e) => e === 'base64url' || isEnc.call(Buffer, e);
}
if (!globalThis.process) globalThis.process = { env: {}, platform: 'linux', versions: { node: null }, version: 'v20.0.0', nextTick: (f, ...a) => Promise.resolve().then(() => f(...a)), emitWarning() {}, on() {}, off() {}, cwd: () => '/', hrtime: Object.assign(() => { const t = globalThis.__nat.mono(); return [Math.floor(t / 1000), (t % 1000) * 1e6]; }, { bigint: () => BigInt(Math.floor(globalThis.__nat.mono() * 1e6)) }) };
if (!Error.captureStackTrace) { Error.captureStackTrace = (o) => { try { o.stack = new Error().stack; } catch (_) {} }; Error.stackTraceLimit = 10; }
// i timer di Node sono oggetti (con unref/ref/refresh): avvolgiamo quelli nativi
{
  const st = globalThis.setTimeout, si = globalThis.setInterval, ct = globalThis.clearTimeout;
  class Timeout { constructor(id, fn, ms, rep) { this._id = id; this._fn = fn; this._ms = ms; this._rep = rep; } unref() { return this; } ref() { return this; } hasRef() { return true; } refresh() { ct(this._id); this._id = (this._rep ? si : st)(this._fn, this._ms); return this; } [Symbol.toPrimitive]() { return this._id; } }
  globalThis.setTimeout = (fn, ms, ...a) => { const f = a.length ? () => fn(...a) : fn; return new Timeout(st(f, ms), f, ms, false); };
  globalThis.setInterval = (fn, ms, ...a) => { const f = a.length ? () => fn(...a) : fn; return new Timeout(si(f, ms), f, ms, true); };
  globalThis.clearTimeout = globalThis.clearInterval = (t) => { if (t == null) return; ct(typeof t === 'object' ? t._id : t); };
}
process.stdout = process.stdout || { write: (t) => { console.log(String(t).replace(/\n$/, '')); return true; } }; process.stderr = process.stderr || process.stdout; process.argv = process.argv || ['engine']; process.exit = process.exit || (() => {}); process.hrtime = process.hrtime;
if (!globalThis.queueMicrotask) globalThis.queueMicrotask = (f) => Promise.resolve().then(f);
if (!globalThis.setImmediate) { globalThis.setImmediate = (f, ...a) => setTimeout(() => f(...a), 0); globalThis.clearImmediate = clearTimeout; }
if (!globalThis.performance) globalThis.performance = { now: () => globalThis.__nat.mono(), timeOrigin: globalThis.__nat.now() };
if (!globalThis.TextEncoder) globalThis.TextEncoder = class TextEncoder { get encoding() { return 'utf-8'; } encode(s = '') { return new Uint8Array(Buffer.from(String(s), 'utf8')); } };
if (!globalThis.TextDecoder) globalThis.TextDecoder = class TextDecoder { constructor(enc = 'utf-8') { this.encoding = enc; } decode(b) { return b ? Buffer.from(b.buffer || b, b.byteOffset || 0, b.byteLength).toString('utf8') : ''; } };
if (!globalThis.atob) { globalThis.atob = (s) => Buffer.from(s, 'base64').toString('latin1'); globalThis.btoa = (s) => Buffer.from(s, 'latin1').toString('base64'); }
if (!globalThis.structuredClone) globalThis.structuredClone = (v) => (v === undefined ? v : JSON.parse(JSON.stringify(v)));
if (!globalThis.crypto || !globalThis.crypto.subtle) { const N = globalThis.__nat; globalThis.crypto = { subtle: {}, getRandomValues: (a) => { new Uint8Array(a.buffer, a.byteOffset, a.byteLength).set(new Uint8Array(N.randomBytes(a.byteLength))); return a; }, randomUUID: () => { const b = new Uint8Array(N.randomBytes(16)); b[6] = (b[6] & 15) | 64; b[8] = (b[8] & 63) | 128; const h = [...b].map((x) => x.toString(16).padStart(2, '0')).join(''); return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`; } }; }
if (!globalThis.AbortController) {
  class AbortSignal { constructor() { this.aborted = false; this.reason = undefined; this._l = []; } addEventListener(t, f) { if (t === 'abort') this._l.push(f); } removeEventListener(t, f) { this._l = this._l.filter((x) => x !== f); } throwIfAborted() { if (this.aborted) throw this.reason; } static timeout(ms) { const c = new AbortController(); setTimeout(() => c.abort(new Error('timeout')), ms); return c.signal; } }
  globalThis.AbortSignal = AbortSignal;
  globalThis.AbortController = class AbortController { constructor() { this.signal = new AbortSignal(); } abort(r) { if (this.signal.aborted) return; this.signal.aborted = true; this.signal.reason = r || new Error('aborted'); for (const f of this.signal._l) f({ type: 'abort' }); if (this.signal.onabort) this.signal.onabort({ type: 'abort' }); } };
}
if (!globalThis.URLSearchParams) {
  globalThis.URLSearchParams = class URLSearchParams {
    constructor(init = '') { this._p = []; if (typeof init === 'string') { for (const kv of init.replace(/^\?/, '').split('&')) if (kv) { const [k, v = ''] = kv.split('='); this._p.push([decodeURIComponent(k), decodeURIComponent(v)]); } } else if (init) for (const [k, v] of Object.entries(init)) this._p.push([k, String(v)]); }
    append(k, v) { this._p.push([k, String(v)]); } get(k) { const e = this._p.find((x) => x[0] === k); return e ? e[1] : null; } has(k) { return this._p.some((x) => x[0] === k); } set(k, v) { this._p = this._p.filter((x) => x[0] !== k); this.append(k, v); } delete(k) { this._p = this._p.filter((x) => x[0] !== k); }
    toString() { return this._p.map(([k, v]) => encodeURIComponent(k) + '=' + encodeURIComponent(v)).join('&'); } [Symbol.iterator]() { return this._p[Symbol.iterator](); }
  };
}
if (!globalThis.URL) {
  const norm = (path) => { const out = []; for (const seg of path.split('/')) { if (seg === '..') { if (out.length > 1) out.pop(); } else if (seg !== '.') out.push(seg); } let r = out.join('/'); if (/\/\.\.?$/.test(path) && !r.endsWith('/')) r += '/'; return r || '/'; };
  globalThis.URL = class URL {
    constructor(u, base) {
      const re = /^([a-z][a-z0-9+.-]*:)?(?:\/\/([^/?#]*))?([^?#]*)(\?[^#]*)?(#.*)?$/i; u = String(u); let m = re.exec(u);
      if (!m) throw new TypeError('Invalid URL: ' + u);
      let [, scheme, auth, path, search, hash] = m;
      if (!scheme) {
        if (base === undefined) throw new TypeError('Invalid URL: ' + u);
        const b = base instanceof URL ? base : new URL(String(base)); scheme = b.protocol;
        if (auth === undefined) { auth = b.host; if (path === '') { path = b.pathname; if (search === undefined) search = b.search; } else if (path[0] !== '/') path = b.pathname.replace(/[^/]*$/, '') + path; }
      }
      if (auth === undefined) throw new TypeError('Invalid URL: ' + u);
      const at = auth.lastIndexOf('@'); const hp = at >= 0 ? auth.slice(at + 1) : auth; const pm = /^(.*?)(?::(\d*))?$/.exec(hp);
      this.protocol = scheme.toLowerCase(); this.hostname = pm[1].toLowerCase(); this.port = pm[2] || ''; this.pathname = norm(path || '/'); this.search = search && search !== '?' ? search : ''; this.hash = hash && hash !== '#' ? hash : '';
      this.host = this.hostname + (this.port ? ':' + this.port : ''); this.origin = this.protocol + '//' + this.host; this.username = ''; this.password = ''; this.searchParams = new URLSearchParams(this.search);
    }
    get href() { return this.toString(); }
    toString() { const q = this.searchParams.toString(); return this.origin + this.pathname + (q ? '?' + q : '') + this.hash; }
    toJSON() { return this.toString(); }
  };
}
