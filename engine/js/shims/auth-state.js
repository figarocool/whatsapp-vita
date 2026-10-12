// Sostituto di useMultiFileAuthState: le chiavi stanno in memoria e si salvano a gruppi (16 file per tipo) invece di un file per chiave.
// Dopo l'associazione WhatsApp genera 812 chiavi: un file ciascuna, sulla memoria della Vita, bloccava il motore per minuti.
import { proto } from '../../WAProto/index.js';
import { initAuthCreds } from './auth-utils.js';
import { BufferJSON } from './generics.js';
import { readFileSync, writeFileSync, existsSync, mkdirSync, unlinkSync } from 'fs';

const SHARDS = 16, FLUSH_MS = 1500;
const shardOf = (id) => { let h = 0; for (let i = 0; i < id.length; i++) h = (h * 31 + id.charCodeAt(i)) >>> 0; return h % SHARDS; };
const fixName = (f) => f.replace(/\//g, '__').replace(/:/g, '-');

let GEN = 0;   // ogni sessione ha un numero: dopo una disconnessione la vecchia non puo' piu' riscrivere chiavi e credenziali
export const useMultiFileAuthState = async (folder) => {
  const my = ++GEN; globalThis.__authDead = () => { GEN++; };
  mkdirSync(folder);
  const load = (name) => { try { const p = `${folder}/${name}`; if (!existsSync(p)) return null; return JSON.parse(readFileSync(p, 'utf8'), BufferJSON.reviver); } catch (_) { return null; } };
  const save = (name, obj) => my !== GEN ? undefined : writeFileSync(`${folder}/${name}`, JSON.stringify(obj, BufferJSON.replacer));
  const creds = load('creds.json') || initAuthCreds();
  const mem = new Map();            // tipo -> Map(gruppo -> { id: valore })
  const dirty = new Set();          // "tipo|gruppo"
  let timer = null;
  const shard = (type, sh) => { let t = mem.get(type); if (!t) { t = new Map(); mem.set(type, t); } let s = t.get(sh); if (!s) { s = load(`k-${fixName(type)}-${sh}.json`) || {}; t.set(sh, s); } return s; };
  const flush = () => {
    timer = null; if (my !== GEN) { dirty.clear(); return; }
    for (const k of dirty) { const [type, sh] = [k.slice(0, k.lastIndexOf('|')), Number(k.slice(k.lastIndexOf('|') + 1))]; try { save(`k-${fixName(type)}-${sh}.json`, shard(type, sh)); } catch (e) { console.log('salvataggio chiavi fallito:', e && e.message); } }
    dirty.clear();
  };
  const schedule = () => { if (!timer) timer = setTimeout(flush, FLUSH_MS); };
  globalThis.__flushAuth = () => { if (timer) { clearTimeout(timer); } flush(); };
  const legacy = (type, id) => load(`${fixName(`${type}-${id}`)}.json`);   // vecchio formato: un file per chiave
  return {
    state: {
      creds,
      keys: {
        get: async (type, ids) => {
          const data = {};
          for (const id of ids) {
            const s = shard(type, shardOf(String(id))); let value = s[id];
            if (value === undefined) { value = legacy(type, id); if (value) { s[id] = value; dirty.add(`${type}|${shardOf(String(id))}`); schedule(); } }
            if (type === 'app-state-sync-key' && value) value = proto.Message.AppStateSyncKeyData.fromObject(value);
            data[id] = value || null;
          }
          return data;
        },
        set: async (data) => {
          for (const category in data) for (const id in data[category]) {
            const value = data[category][id], sh = shardOf(String(id)), s = shard(category, sh);
            if (value) s[id] = value; else delete s[id];
            dirty.add(`${category}|${sh}`);
          }
          schedule();
        },
      },
    },
    saveCreds: async () => { save('creds.json', creds); },
  };
};
