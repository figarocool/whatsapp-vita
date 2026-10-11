// AsyncLocalStorage semplificato: QuickJS non ha hook asincroni, quindi il "contesto" vale finche' dura la promessa di run().
export class AsyncLocalStorage {
  constructor() { this._s = undefined; }
  getStore() { return this._s; }
  run(store, fn, ...args) {
    const prev = this._s; this._s = store;
    try { const r = fn(...args); if (r && typeof r.then === 'function') return r.finally(() => { this._s = prev; }); this._s = prev; return r; } catch (e) { this._s = prev; throw e; }
  }
  enterWith(store) { this._s = store; } exit(fn, ...args) { const p = this._s; this._s = undefined; try { return fn(...args); } finally { this._s = p; } } disable() { this._s = undefined; }
}
export class AsyncResource { constructor() {} runInAsyncScope(fn, thisArg, ...a) { return fn.apply(thisArg, a); } }
export const createHook = () => ({ enable() {}, disable() {} }), executionAsyncId = () => 0;
export default { AsyncLocalStorage, AsyncResource, createHook, executionAsyncId };
