// 'net': il canale grezzo del plugin non serve quando il motore e' dentro la Vita.
import EventEmitter from 'events';
export const createServer = () => { const s = new EventEmitter(); s.listen = (p, h, cb) => { if (typeof h === 'function') cb = h; if (cb) setTimeout(cb, 0); return s; }; s.close = () => {}; return s; };
export const Socket = class extends EventEmitter {}; export const isIP = () => 0;
export default { createServer, Socket, isIP };
