// Sostituto del pacchetto 'ws': un WebSocket client appoggiato alla parte nativa (TLS su mbedTLS).
import EventEmitter from 'events';
const N = globalThis.__nat;
export default class WebSocket extends EventEmitter {
  static CONNECTING = 0; static OPEN = 1; static CLOSING = 2; static CLOSED = 3;
  constructor(url, opts = {}) {
    super(); this.url = url; this.readyState = 0; this._id = -1;
    const headers = { ...(opts.headers || {}) }; if (opts.origin) headers.Origin = opts.origin;
    setTimeout(() => {
      try { this._id = N.wsOpen(String(url), headers, (ev, data, bin) => this._ev(ev, data, bin)); this.readyState = 1; this.emit('open'); }
      catch (e) { console.log('ws errore apertura:', String(url), e && e.message); this.readyState = 3; this.emit('error', e); this.emit('close', 1006, ''); }
    }, 0);
  }
  _ev(ev, data, bin) {
    if (ev === 'message') this.emit('message', bin ? Buffer.from(data) : Buffer.from(data).toString('utf8'), bin);
    else if (ev === 'close') { this.readyState = 3; this.emit('close', 1000, ''); }
  }
  send(data, opts, cb) {
    if (typeof opts === 'function') cb = opts;
    const ok = this.readyState === 1 && N.wsSend(this._id, typeof data === 'string' ? Buffer.from(data) : data, typeof data !== 'string');
    if (cb) cb(ok ? undefined : new Error('invio WebSocket fallito'));
  }
  close() { if (this.readyState === 1) { this.readyState = 2; N.wsClose(this._id); } else if (this.readyState === 0) this.readyState = 3; }
  terminate() { this.close(); } ping() {} pong() {} setMaxListeners() { return this; }
}
export const WebSocketServer = class {}; export { WebSocket };
