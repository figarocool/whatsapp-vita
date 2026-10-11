const N = globalThis.__nat;
const wrap = (f) => (buf, opts, cb) => { if (typeof opts === 'function') cb = opts; let r, err = null; try { r = Buffer.from(f(buf)); } catch (e) { err = e; } if (cb) setTimeout(() => cb(err, r), 0); else if (err) throw err; else return r; };
export const inflate = wrap((b) => N.inflate(b)); export const inflateSync = (b) => Buffer.from(N.inflate(b));
export const deflate = wrap((b) => N.deflate(b)); export const deflateSync = (b) => Buffer.from(N.deflate(b));
import EventEmitter from 'events';
// flusso di decompressione minimale: accumula quello che riceve e decomprime alla fine
class ZStream extends EventEmitter { constructor(fn) { super(); this.fn = fn; this.parts = []; } write(c) { this.parts.push(Buffer.from(c)); return true; } end(c) { if (c) this.write(c); try { this.emit('data', Buffer.from(this.fn(Buffer.concat(this.parts)))); this.emit('end'); this.emit('close'); } catch (e) { this.emit('error', e); } } pipe(d) { this.on('data', (x) => d.write(x)); this.on('end', () => d.end()); return d; } }
export const createInflate = () => new ZStream((b) => N.inflate(b)); export const createDeflate = () => new ZStream((b) => N.deflate(b));
export const gunzip = inflate, gzip = deflate;
export default { createInflate, createDeflate, inflate, inflateSync, deflate, deflateSync, gunzip, gzip };
