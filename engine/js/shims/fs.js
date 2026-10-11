const N = globalThis.__nat;
const enc = (r, o) => { if (r === null) { const e = new Error('ENOENT: no such file or directory'); e.code = 'ENOENT'; throw e; } const b = Buffer.from(r); const e = typeof o === 'string' ? o : o && o.encoding; return e ? b.toString(e) : b; };
export const readFileSync = (p, o) => enc(N.fsRead(String(p)), o);
export const writeFileSync = (p, d, o) => { if (!N.fsWrite(String(p), typeof d === 'string' ? Buffer.from(d) : d)) throw new Error('scrittura fallita: ' + p); };
export const existsSync = (p) => N.fsExists(String(p));
export const mkdirSync = (p) => { N.fsMkdir(String(p)); };
export const readdirSync = (p) => N.fsList(String(p));
export const unlinkSync = (p) => { N.fsUnlink(String(p)); };
export const statSync = (p) => { if (!N.fsExists(String(p))) { const e = new Error('ENOENT'); e.code = 'ENOENT'; throw e; } return { isFile: () => true, isDirectory: () => false, size: 0 }; };
export const constants = { F_OK: 0, R_OK: 4, W_OK: 2, X_OK: 1 };
export const promises = {
  open: async () => { throw new Error('fs.open non disponibile su Vita'); },
  readFile: async (p, o) => readFileSync(p, o), writeFile: async (p, d, o) => writeFileSync(p, d, o), mkdir: async (p) => mkdirSync(p),
  readdir: async (p) => readdirSync(p), unlink: async (p) => unlinkSync(p), stat: async (p) => statSync(p), access: async (p) => { if (!existsSync(p)) { const e = new Error('ENOENT'); e.code = 'ENOENT'; throw e; } },
  rm: async (p) => unlinkSync(p), rename: async () => {}, copyFile: async () => {},
};
const nope = () => { throw new Error('stream di file non disponibili su Vita'); };
export const createReadStream = nope, createWriteStream = nope; export class WriteStream {}
export default { constants, readFileSync, writeFileSync, existsSync, mkdirSync, readdirSync, unlinkSync, statSync, promises, createReadStream, createWriteStream, WriteStream };
