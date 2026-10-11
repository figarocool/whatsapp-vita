const U = globalThis.URL, P = globalThis.URLSearchParams;
export { U as URL, P as URLSearchParams };
export const fileURLToPath = (u) => String(u).replace(/^file:\/\//, '');
export const pathToFileURL = (p) => ({ href: 'file://' + p, toString: () => 'file://' + p });
export const parse = (s) => { try { const u = new U(s); return { protocol: u.protocol, hostname: u.hostname, host: u.host, port: u.port, pathname: u.pathname, search: u.search, query: u.search.slice(1), hash: u.hash, href: String(s) }; } catch (_) { return {}; } };
export const format = (o) => (typeof o === 'string' ? o : String(o.href || ''));
export default { URL: U, URLSearchParams: P, fileURLToPath, pathToFileURL, parse, format };
