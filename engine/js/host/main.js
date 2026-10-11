// Programma di avvio: costruisce la sessione WhatsApp (Baileys) dentro il motore e stampa gli eventi.
import './prelude.js';
import makeWASocket, { DisconnectReason, initAuthCreds, BufferJSON, proto } from '@whiskeysockets/baileys';
import { readFileSync, writeFileSync, existsSync, mkdirSync, readdirSync, unlinkSync } from 'fs';

const DIR = (globalThis.__nat.dataDir || '.') + '/auth';
mkdirSync(DIR);
const fname = (n) => n.replace(/\//g, '__').replace(/:/g, '-');
const readData = (n) => { const p = `${DIR}/${fname(n)}.json`; if (!existsSync(p)) return null; try { return JSON.parse(readFileSync(p, 'utf8'), BufferJSON.reviver); } catch (_) { return null; } };
const writeData = (n, d) => writeFileSync(`${DIR}/${fname(n)}.json`, JSON.stringify(d, BufferJSON.replacer));
const removeData = (n) => { try { unlinkSync(`${DIR}/${fname(n)}.json`); } catch (_) {} };
const creds = readData('creds') || initAuthCreds();
const state = { creds, keys: {
  get: async (type, ids) => { const data = {}; for (const id of ids) { let v = readData(`${type}-${id}`); if (type === 'app-state-sync-key' && v) v = proto.Message.AppStateSyncKeyData.fromObject(v); data[id] = v; } return data; },
  set: async (data) => { for (const cat in data) for (const id in data[cat]) { const v = data[cat][id]; if (v) writeData(`${cat}-${id}`, v); else removeData(`${cat}-${id}`); } },
} };
const saveCreds = () => writeData('creds', creds);
const logger = (() => { const l = { level: 'info', child: () => l, trace() {}, debug() {}, info(o, m) { console.log('[wa]', typeof o === 'string' ? o : (m || '')); }, warn(o, m) { console.log('[wa-warn]', typeof o === 'string' ? o : (m || '')); }, error(o, m) { console.log('[wa-errore]', typeof o === 'string' ? o : (m || ''), o && o.err ? (o.err.message || o.err) : ''); }, fatal() {} }; return l; })();

function start() {
  const sock = makeWASocket({ auth: state, logger, browser: ['PS Vita', 'Chrome', '1.0'], printQRInTerminal: false, syncFullHistory: false, markOnlineOnConnect: false, generateHighQualityLinkPreview: false });
  sock.ev.on('creds.update', saveCreds);
  sock.ev.on('connection.update', (u) => {
    if (u.qr) console.log('QR:' + u.qr);
    if (u.connection) console.log('connessione:', u.connection);
    if (u.connection === 'close') { const code = u.lastDisconnect?.error?.output?.statusCode; console.log('chiusa, codice', code, u.lastDisconnect?.error?.message); if (code !== DisconnectReason.loggedOut) setTimeout(start, 2000); }
  });
  sock.ev.on('messages.upsert', ({ messages }) => { for (const m of messages) console.log('MESSAGGIO da', m.key.remoteJid, ':', m.message?.conversation || m.message?.extendedTextMessage?.text || Object.keys(m.message || {})[0]); });
  sock.ev.on('messaging-history.set', ({ chats, messages, progress }) => console.log('cronologia: chat', (chats || []).length, 'messaggi', (messages || []).length, 'progresso', progress));
  globalThis.__sock = sock;
  // prova: se esiste send.txt ("jid|testo") il motore invia il messaggio e cancella il file
  setInterval(async () => { const f = (globalThis.__nat.dataDir || '.') + '/send.txt'; if (!existsSync(f)) return; const t = readFileSync(f, 'utf8'); try { unlinkSync(f); } catch (_) {} const i = t.indexOf('|'); if (i < 0) return; try { const r = await sock.sendMessage(t.slice(0, i), { text: t.slice(i + 1).trim() }); console.log('INVIATO id', r && r.key && r.key.id); } catch (e) { console.log('INVIO FALLITO:', e && e.message, e && e.stack ? String(e.stack).split('\n').slice(0, 5).join(' | ') : ''); } }, 1000);
}
start();
