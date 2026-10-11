// 'http' di Node: createServer() collega il gestore al motore (le richieste arrivano dalla app o dal socket di prova).
import EventEmitter from 'events';
import { Readable, Writable } from 'stream';
const N = globalThis.__nat;
class IncomingMessage extends Readable { constructor(id, method, url, headers, body) { super({ read() {} }); this.method = method; this.url = url; this.headers = headers || {}; this.socket = { remoteAddress: '127.0.0.1', destroy() {} }; this.connection = this.socket; this._body = body; setTimeout(() => { if (this._body && this._body.byteLength) this.push(Buffer.from(this._body)); this.push(null); }, 0); } }
class ServerResponse extends Writable {
  constructor(id) { super(); this._id = id; this.statusCode = 200; this._h = {}; this.headersSent = false; this.finished = false; this.socket = { destroy() {} }; }
  setHeader(k, v) { this._h[String(k).toLowerCase()] = String(v); } getHeader(k) { return this._h[String(k).toLowerCase()]; } removeHeader(k) { delete this._h[String(k).toLowerCase()]; }
  writeHead(status, headers) { this.statusCode = status; if (headers) for (const k in headers) this.setHeader(k, headers[k]); return this; }
  _send(chunk, last) { const first = !this.headersSent; this.headersSent = true; N.respond(this._id, this.statusCode, first ? JSON.stringify(this._h) : null, chunk, last); }
  _write(chunk, enc, cb) { this._send(typeof chunk === 'string' ? Buffer.from(chunk, enc) : chunk, false); cb(); }
  _final(cb) { this._send(null, true); this.finished = true; cb(); }
  end(chunk, enc, cb) { if (typeof chunk === 'function') { cb = chunk; chunk = undefined; } if (chunk) super.write(chunk, enc); return super.end(cb); }
}
export function createServer(opts, handler) {
  if (typeof opts === 'function') handler = opts;
  const srv = new EventEmitter();
  srv.listen = (port, host, cb) => { if (typeof host === 'function') cb = host; N.setHandler((id, method, url, headers, body) => { const req = new IncomingMessage(id, method, url, headers, body), res = new ServerResponse(id); try { const r = handler(req, res); if (r && r.catch) r.catch((e) => { console.log('errore nel gestore HTTP:', e && e.stack || e); try { if (!res.headersSent) res.writeHead(500); res.end(); } catch (_) {} }); } catch (e) { console.log('errore nel gestore HTTP:', e && e.stack || e); try { res.writeHead(500); res.end(); } catch (_) {} } }); if (cb) setTimeout(cb, 0); return srv; };
  srv.close = (cb) => { if (cb) cb(); }; srv.address = () => ({ port: 0 }); srv.setTimeout = () => {};
  return srv;
}
export default { createServer, IncomingMessage, ServerResponse };
export { IncomingMessage, ServerResponse };
