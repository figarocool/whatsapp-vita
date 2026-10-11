// Sostituto del modulo 'fs' di Node, appoggiato alla parte nativa.
import { Readable, Writable } from 'stream';
const N = globalThis.__nat;
const err = (code, msg, p) => { const e = new Error(`${code}: ${msg}${p ? ", '" + p + "'" : ''}`); e.code = code; return e; };
const dec = (r, o, p) => { if (r === null) throw err('ENOENT', 'no such file or directory', p); const b = Buffer.from(r); const e = typeof o === 'string' ? o : o && o.encoding; return e ? b.toString(e) : b; };
export const constants = { F_OK: 0, R_OK: 4, W_OK: 2, X_OK: 1 };
export const readFileSync = (p, o) => dec(N.fsRead(String(p)), o, p);
export const writeFileSync = (p, d) => { if (!N.fsWrite(String(p), typeof d === 'string' ? Buffer.from(d) : d)) throw err('EIO', 'scrittura fallita', p); };
export const appendFileSync = (p, d) => { if (!N.fsAppend(String(p), typeof d === 'string' ? Buffer.from(d) : d)) throw err('EIO', 'append fallito', p); };
export const existsSync = (p) => N.fsExists(String(p));
export const mkdirSync = (p) => { N.fsMkdir(String(p)); };
export const readdirSync = (p) => N.fsList(String(p));
export const unlinkSync = (p) => { if (!N.fsUnlink(String(p))) throw err('ENOENT', 'no such file or directory', p); };
export const rmSync = (p, o) => { N.fsUnlink(String(p)); };
export const renameSync = (a, b) => { if (!N.fsRename(String(a), String(b))) throw err('EIO', 'rename fallito', a); };
export const statSync = (p) => { const s = N.fsStat(String(p)); if (!s) throw err('ENOENT', 'no such file or directory', p); return { size: s.size, mtimeMs: s.mtimeMs, mtime: new Date(s.mtimeMs), isFile: () => !s.isDir, isDirectory: () => s.isDir }; };
export const copyFileSync = (a, b) => writeFileSync(b, readFileSync(a));
export function createReadStream(p, o = {}) {
  let off = o.start || 0; const end = o.end === undefined ? Infinity : o.end + 1; const CH = o.highWaterMark || 65536; const path = String(p);
  return new Readable({ read() { const n = Math.min(CH, end - off); if (n <= 0) return this.push(null); const b = N.fsReadRange(path, off, n); if (b === null) return this.destroy(err('ENOENT', 'no such file or directory', path)); if (!b.byteLength) return this.push(null); off += b.byteLength; this.push(Buffer.from(b)); } });
}
export function createWriteStream(p, o = {}) {
  const path = String(p); if (!(o.flags && String(o.flags).startsWith('a'))) N.fsWrite(path, Buffer.alloc(0));
  const w = new Writable({ write(chunk, enc, cb) { if (!N.fsAppend(path, typeof chunk === 'string' ? Buffer.from(chunk, enc) : chunk)) return cb(err('EIO', 'scrittura fallita', path)); cb(); } });
  w.path = path; w.bytesWritten = 0; return w;
}
export class WriteStream {}
const cb = (f) => async (...a) => f(...a);
export const promises = {
  open: async () => { throw err('ENOSYS', 'fs.open non disponibile'); },
  readFile: cb(readFileSync), writeFile: cb(writeFileSync), appendFile: cb(appendFileSync), mkdir: cb(mkdirSync), readdir: cb(readdirSync), unlink: cb(unlinkSync),
  stat: cb(statSync), rename: cb(renameSync), copyFile: cb(copyFileSync), rm: async (p) => { N.fsUnlink(String(p)); },
  access: async (p) => { if (!existsSync(p)) throw err('ENOENT', 'no such file or directory', p); },
};
export default { constants, readFileSync, writeFileSync, appendFileSync, existsSync, mkdirSync, readdirSync, unlinkSync, rmSync, renameSync, statSync, copyFileSync, createReadStream, createWriteStream, WriteStream, promises };
