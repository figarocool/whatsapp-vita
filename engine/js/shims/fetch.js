// fetch() appoggiato al client HTTP/HTTPS nativo: la risposta arriva a pezzi in un Readable.
import { Readable } from 'stream';
const N = globalThis.__nat;
class H { constructor(o) { this._h = o || {}; } get(k) { const v = this._h[String(k).toLowerCase()]; return v === undefined ? null : v; } has(k) { return String(k).toLowerCase() in this._h; } forEach(f) { for (const k in this._h) f(this._h[k], k, this); } entries() { return Object.entries(this._h)[Symbol.iterator](); } [Symbol.iterator]() { return this.entries(); } }
class Response {
  constructor(status, headers, body, url) { this.status = status; this.ok = status >= 200 && status < 300; this.statusText = ''; this.headers = new H(headers); this.body = body; this.url = url; this.bodyUsed = false; }
  async _all() { this.bodyUsed = true; const parts = []; for await (const c of this.body) parts.push(Buffer.from(c)); return Buffer.concat(parts); }
  async arrayBuffer() { const b = await this._all(); return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength); }
  async text() { return (await this._all()).toString('utf8'); }
  async json() { return JSON.parse(await this.text()); }
  async blob() { return this.arrayBuffer(); }
}
async function toBytes(body) {
  if (body == null) return null;
  if (typeof body === 'string') return Buffer.from(body);
  if (body instanceof ArrayBuffer) return Buffer.from(body);
  if (ArrayBuffer.isView(body)) return Buffer.from(body.buffer, body.byteOffset, body.byteLength);
  if (typeof body.on === 'function' || body[Symbol.asyncIterator]) { const parts = []; for await (const c of body) parts.push(Buffer.from(c)); return Buffer.concat(parts); }
  return Buffer.from(String(body));
}
// al massimo 4 richieste alla volta (la Vita ha poca memoria): le altre aspettano
let active = 0; const waiting = [];
const acquire = () => new Promise((res) => { if (active < 4) { active++; res(); } else waiting.push(res); });
const release = () => { const n = waiting.shift(); if (n) n(); else active--; };
export default async function fetch(input, opts = {}, hops = 0) {
  await acquire(); let released = false; const rel = () => { if (!released) { released = true; release(); } };
  try { return await fetchOne(input, opts, hops, rel); } catch (e) { rel(); throw e; }
}
function fetchOne(input, opts, hops, rel) {
  return fetchInner(input, opts, hops, rel);
}
async function fetchInner(input, opts = {}, hops = 0, rel = () => {}) {
  const url = String(input && input.url ? input.url : input), method = (opts.method || 'GET').toUpperCase();
  const headers = {}; const oh = opts.headers; if (oh) { if (typeof oh.forEach === 'function' && !Array.isArray(oh)) oh.forEach((v, k) => { headers[k] = v; }); else for (const k in oh) headers[k] = oh[k]; }
  const body = await toBytes(opts.body);
  return new Promise((resolve, reject) => {
    const rs = new Readable({ read() {} }); let done = false, id = -1;
    if (opts.signal) opts.signal.addEventListener('abort', () => { if (id >= 0) N.httpAbort(id); const e = opts.signal.reason || new Error('aborted'); if (!done) { done = true; reject(e); } else rs.destroy(e); });
    try {
      id = N.httpRequest(method, url, headers, body, (ev, a, b) => {
        if (ev === 'headers') {
          if ([301, 302, 303, 307, 308].includes(a) && b.location && hops < 5) { done = true; N.httpAbort(id); const next = new URL(b.location, url).toString(); rel(); return resolve(fetch(next, (a === 303 ? { ...opts, method: 'GET', body: undefined } : opts), hops + 1)); }
          if (a >= 400) console.log('fetch: stato', a, url.slice(0, 90));
          done = true; resolve(new Response(a, b, rs, url));
        } else if (ev === 'data') rs.push(Buffer.from(a));
        else if (ev === 'end') { rs.push(null); rel(); }
        else if (ev === 'error') { rel(); const e = new Error(String(a)); if (!done) { done = true; reject(e); } else rs.destroy(e); }
      });
    } catch (e) { done = true; reject(e); }
  });
}
export { fetch, Response, H as Headers };
