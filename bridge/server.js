// Bridge WhatsApp (Baileys) -> API HTTP semplice per il client PS Vita.
// Stessa libreria/versione di eletron-app. Auth in ./data
const http = require('http');
const fs = require('fs');
const path = require('path');
const pino = require('pino');
const QRCode = require('qrcode');
const sharp = require('sharp');

const PORT = parseInt(process.env.PORT || '8787', 10);
const DATA = path.join(__dirname, 'data');
const AUTH = path.join(DATA, 'auth');
fs.mkdirSync(AUTH, { recursive: true });
const logger = pino({ level: 'silent' });
const LOG = path.join(DATA, 'bridge.log');
function log(...a) {
  const line = `[${new Date().toISOString()}] ${a.join(' ')}\n`;
  try { if (fs.existsSync(LOG) && fs.statSync(LOG).size > 500000) fs.renameSync(LOG, LOG + '.old'); fs.appendFileSync(LOG, line); } catch (_) {}
  process.stdout.write(line);
}
process.on('uncaughtException', (e) => log('ERRORE non gestito:', e && e.stack || e));
process.on('unhandledRejection', (e) => log('PROMISE rifiutata:', e && e.stack || e));
log('=== bridge avviato ===');

const state = { status: 'starting', qr: null, me: null };
const sync = { active: false, progress: 0, t: 0 };
let B = null;                 // modulo baileys (import dinamico)
let sock;
const chats = new Map();      // jid -> {jid,name,last,ts,unread}
const ENGINE = typeof globalThis.__nat !== 'undefined';   // dentro la Vita: tutto piu' lento, quindi niente lavori inutili all'avvio
// Le chat si leggono dal disco solo quando servono (aprire la chat, scaricare un media...): all'avvio si caricano solo i metadati
const lazyChats = new Set();
class LazyRaws extends Map {
  get(k) { if (lazyChats.has(k)) loadChatFile(k); return super.get(k); }
  has(k) { return lazyChats.has(k) || super.has(k); }
  *[Symbol.iterator]() { loadAllLazy(); yield* super[Symbol.iterator](); }
}
const raws = new LazyRaws();       // jid -> Map(id -> WAMessage)
const reacts = new Map();     // `${jid}|${id}` -> Map(sender -> emoji)
const names = new Map();      // jid -> name
const presence = new Map();   // jid -> {state, ts}
const subscribed = new Set();
const events = [];
const blocked = new Set();
const calls = [];   // registro chiamate (anche su disco): arrivano in diretta e dalle voci 'registro chiamate' che manda il telefono
const CALLS_FILE = path.join(DATA, 'calls.json');
let callsLoaded = false;
function callsLoad() { if (callsLoaded) return; callsLoaded = true; try { calls.push(...JSON.parse(fs.readFileSync(CALLS_FILE, 'utf8'))); } catch (_) {} }
function callsSave() { calls.sort((a, b) => b.ts - a.ts); if (calls.length > 200) calls.length = 200; try { fs.writeFileSync(CALLS_FILE, JSON.stringify(calls)); } catch (_) {} }
function callAdd(c) { callsLoad(); const i = calls.findIndex((x) => x.id === c.id); if (i >= 0) calls[i] = { ...calls[i], ...c }; else calls.push(c); callsSave(); }
let commCache = { ts: 0, data: [] };
let evSeq = 0;
const statusViewers = new Map();  // id storia mia -> Map(jid -> ora)
const chMeta = new Map();         // metadati canali (nome, iscritti)
const chCache = new Map();        // jid canale -> {ts, msgs}
const pnames = new Map();      // nomi pubblici (push name) per jid
const alias = new Map();       // jid <-> altro jid dello stesso contatto (numero <-> lid)
const mediaCache = new Map();
const lastPoll = { ip: null, t: 0, n: 0 };   // ultima richiesta del plugin delle notifiche
const avatarCache = new Map();
const norm = (j) => (typeof j === 'string' ? j.replace(/:\d+@/, '@') : j);   // toglie il suffisso dispositivo (:0)
function link(a, b) { a = norm(a); b = norm(b); if (a && b && a !== b) { alias.set(a, b); alias.set(b, a); } }
const pendingLid = new Set();
async function resolveLids() {
  const lm = sock?.signalRepository?.lidMapping; if (!lm || !B) return; let n = 0;
  for (const jid of [...pendingLid].slice(0, 60)) {
    pendingLid.delete(jid);
    try { const pn = await lm.getPNForLID(jid); if (pn) { link(jid, pn); n++; } } catch (_) {}
  }
  if (n) { dirtyMeta = true; log('lid risolti in numeri:', n); }
}
function chMetaFor(jid) {
  if (!chMeta.has(jid)) { chMeta.set(jid, null); sock.newsletterMetadata('jid', jid).then((m) => chMeta.set(jid, { name: m?.name || '', subscribers: m?.subscribers || 0 })).catch(() => chMeta.delete(jid)); }
  return chMeta.get(jid);
}
function nameFor(jid) {
  jid = norm(jid); if (!jid) return null; if (jid.endsWith('@lid') && !alias.has(jid)) pendingLid.add(jid);
  const al = alias.get(jid);
  return names.get(jid) || (al && names.get(al)) || pnames.get(jid) || (al && pnames.get(al)) || null;
}
function nameOf(jid) {
  jid = norm(jid) || jid; const n = nameFor(jid); if (n) return n;
  const cn = chats.get(jid)?.name; if (cn && !/^\d+$/.test(cn)) return cn;
  const num = jid.split('@')[0].split(':')[0];
  return jid.endsWith('@s.whatsapp.net') ? '+' + num : 'Utente ...' + num.slice(-4);
}
function learnContact(c) {
  if (!c || !c.id) return;
  const ids = [c.id, c.lid, c.phoneNumber].filter(Boolean).map(norm);
  link(c.id, c.lid); link(c.id, c.phoneNumber); link(c.lid, c.phoneNumber);
  const nm = c.name || c.verifiedName || c.username;
  if (nm) for (const i of ids) names.set(i, nm);
  if (c.notify) for (const i of ids) pnames.set(i, c.notify);
  dirtyMeta = true;
} // key -> {buf, type}

// ---------- utilità messaggi ----------
function unwrap(m) { return B ? (B.normalizeMessageContent(m.message) || {}) : (m.message || {}); }
const num = (v) => {
  if (v && typeof v === 'object' && 'low' in v) return (v.high >>> 0) * 4294967296 + (v.low >>> 0); // Long dopo JSON
  return Number(v?.toString?.() ?? v ?? 0);
};

function describe(m) {
  const x = unwrap(m);
  if (x.conversation) return { kind: 'text', text: x.conversation };
  if (x.extendedTextMessage) return { kind: 'text', text: x.extendedTextMessage.text || '' };
  if (x.imageMessage) return { kind: 'image', text: x.imageMessage.caption || '', w: x.imageMessage.width, h: x.imageMessage.height };
  if (x.videoMessage) return { kind: 'video', text: x.videoMessage.caption || '', w: x.videoMessage.width, h: x.videoMessage.height, dur: num(x.videoMessage.seconds) };
  if (x.audioMessage) return { kind: x.audioMessage.ptt ? 'voice' : 'audio', text: '', dur: num(x.audioMessage.seconds) };
  if (x.documentMessage) return { kind: 'doc', text: x.documentMessage.caption || '', fname: x.documentMessage.fileName || 'documento', size: num(x.documentMessage.fileLength) };
  if (x.stickerMessage) return { kind: 'sticker', text: '', w: x.stickerMessage.width, h: x.stickerMessage.height };
  if (x.contactMessage) return { kind: 'text', text: '\u{1F464} ' + (x.contactMessage.displayName || 'Contatto') };
  if (x.locationMessage) return { kind: 'text', text: '\u{1F4CD} Posizione' };
  return null;
}
const preview = (d) => d.text || ({ image: '\u{1F4F7} Foto', video: '\u{1F3A5} Video', voice: '\u{1F3A4} Messaggio vocale', audio: '\u{1F3B5} Audio', doc: '\u{1F4C4} ' + (d.fname || ''), sticker: 'Sticker' }[d.kind] || '');

function quotedOf(m) {
  const x = unwrap(m);
  const ci = (x.extendedTextMessage || x.imageMessage || x.videoMessage || x.documentMessage || x.audioMessage || {}).contextInfo;
  if (!ci || !ci.quotedMessage) return null;
  const q = B.normalizeMessageContent(ci.quotedMessage) || {};
  const d = describe({ message: q });
  const who = ci.participant ? nameOf(ci.participant) : '';
  return { text: d ? preview(d) : '', name: ci.participant && ci.participant === sock?.user?.id ? 'Tu' : who };
}

function view(m) {
  const jid = m.key.remoteJid;
  if (m.__deleted) return { id: m.key.id, from_me: !!m.key.fromMe, kind: 'deleted', text: '', ts: num(m.messageTimestamp), name: '' };
  const d = describe(m) || { kind: 'text', text: '' };
  const r = reacts.get(jid + '|' + m.key.id);
  const counts = {};
  if (r) for (const e of r.values()) counts[e] = (counts[e] || 0) + 1;
  return {
    id: m.key.id, from_me: !!m.key.fromMe, ts: num(m.messageTimestamp), name: m.key.fromMe ? '' : (nameFor(m.key.participant || m.key.remoteJid) || m.pushName || ''),
    status: m.key.fromMe ? (m.status ?? 2) : 0, edited: !!m.__edited, quoted: quotedOf(m),
    reactions: Object.entries(counts).map(([e, n]) => ({ e, n })), ...d,
  };
}

function rawList(jid) { if (lazyChats.has(jid)) loadChatFile(jid); if (!raws.has(jid)) raws.set(jid, new Map()); return raws.get(jid); }

function handleProtocol(m) {
  const x = m.message || {};
  const p = x.protocolMessage || x.editedMessage?.message?.protocolMessage;
  if (!p) return false;
  const jid = m.key.remoteJid;
  const list = raws.get(jid);
  if (p.type === 0 && p.key && list?.has(p.key.id)) { list.get(p.key.id).__deleted = true; refreshLast(jid); }
  else if (p.type === 14 && p.key && list?.has(p.key.id) && p.editedMessage) {
    const t = list.get(p.key.id); t.message = p.editedMessage; t.__edited = true; refreshLast(jid);
  }
  return true;
}

function refreshLast(jid) {
  dirtyMeta = true; dirtyChats.add(jid);
  const list = raws.get(jid); const c = chats.get(jid); if (!list || !c) return;
  const last = [...list.values()].sort((a, b) => num(a.messageTimestamp) - num(b.messageTimestamp)).pop();
  if (last) { const v = view(last); c.last = v.kind === 'deleted' ? '\u{1F6AB} Messaggio eliminato' : preview(v); }
}

function handleReaction(m) {
  const rm = unwrap(m).reactionMessage; if (!rm) return false;
  const jid = m.key.remoteJid; const k = jid + '|' + rm.key.id;
  if (!reacts.has(k)) reacts.set(k, new Map());
  const sender = m.key.participant || (m.key.fromMe ? 'me' : jid);
  if (rm.text) reacts.get(k).set(sender, rm.text); else reacts.get(k).delete(sender);
  dirtyMeta = true; dirtyChats.add(jid); return true;
}

function addMessage(m, live) {
  const jid = m.key.remoteJid;
  if (!jid || !m.message) return;
  if (jid === 'status@broadcast') {
    if (!describe(m)) return;
    m.messageTimestamp = num(m.messageTimestamp);
    rawList(jid).set(m.key.id, m); dirtyChats.add(jid);
    if (live && describe(m).kind === 'video' && !m.key.fromMe) bgPrep(jid, m.key.id);
    const sender = m.key.participant || m.participant; if (sender && m.pushName && !m.key.fromMe) pnames.set(norm(sender), m.pushName);
    return;
  }
  { const cl = m.message?.callLogMesssage;   // il telefono manda le chiamate come messaggi di servizio nella chat del contatto
    if (cl) {
      const oc = { 0: 'accept', 1: 'timeout', 2: 'timeout', 3: 'reject', 4: 'accept', 5: 'accept', 6: 'timeout', 7: 'timeout' }[Number(cl.callOutcome)] || 'accept';
      callAdd({ id: 'log-' + m.key.id, jid, video: !!cl.isVideo, status: oc, ts: num(m.messageTimestamp), out: !!m.key.fromMe, dur: Number(cl.durationSecs || 0) });
      log('chiamata registrata dal telefono:', jid, 'esito', cl.callOutcome, 'durata', Number(cl.durationSecs || 0));
      return;
    } }
  if (handleReaction(m) || handleProtocol(m)) return;
  const d = describe(m); if (!d) return;
  if (live && (d.kind === 'image' || d.kind === 'sticker')) prefetchMedia(jid, m.key.id);
  if (live && d.kind === 'video') bgPrep(jid, m.key.id);
  const ts = num(m.messageTimestamp);
  const fromMe = !!m.key.fromMe;
  link(m.key.participant, m.key.participantAlt); link(jid, m.key.remoteJidAlt);
  if (m.pushName && !fromMe) { pnames.set(norm(m.key.participant || jid), m.pushName); dirtyMeta = true; }
  const list = rawList(jid);
  if (list.has(m.key.id)) { Object.assign(list.get(m.key.id), { status: m.status ?? list.get(m.key.id).status }); return; }
  m.messageTimestamp = ts; dirtyMeta = true; dirtyChats.add(jid);
  list.set(m.key.id, m);
  if (list.size > 600) { let ok = null, ot = Infinity; for (const [k, x] of list) { const t = num(x.messageTimestamp); if (t < ot) { ot = t; ok = k; } } if (ok) list.delete(ok); }
  const c = chats.get(jid) || { jid, name: nameFor(jid) || jid.split('@')[0], unread: 0 };
  if (ts >= (c.ts || 0)) { c.last = preview(d); c.ts = ts; }
  if (live && !fromMe) {
    c.unread = (c.unread || 0) + 1;
    if (!(c.muted && c.muted > Date.now()) && !c.archived) events.push({ seq: ++evSeq, jid, title: c.name, text: preview(d), ts });
    if (events.length > 100) events.shift();
  }
  chats.set(jid, c);
}

// ---------- persistenza su disco, a blocchi: un file per chat, scritti solo se cambiati ----------
const STORE_OLD = path.join(DATA, 'store.json');
const STORE_DIR = path.join(DATA, 'store'); fs.mkdirSync(STORE_DIR, { recursive: true });
const META = path.join(STORE_DIR, 'meta.json');
const oldAsked = new Map();
const dirtyChats = new Set(); let dirtyMeta = false, lastMetaSave = 0;
const chatFile = (jid) => path.join(STORE_DIR, 'c_' + Buffer.from(jid).toString('base64url') + '.json');
function writeAtomic(file, text) { fs.writeFileSync(file + '.tmp', text); fs.renameSync(file + '.tmp', file); }
function saveStore(all = false) {
  if (!B) return; const t0 = Date.now(); let n = 0;
  try {
    if (dirtyMeta && (all || !ENGINE || Date.now() - lastMetaSave > 60000)) { dirtyMeta = false; lastMetaSave = Date.now(); writeAtomic(META, JSON.stringify({ chats: [...chats.values()], names: [...names.entries()], pnames: [...pnames.entries()], alias: [...alias.entries()], viewers: [...statusViewers.entries()].map(([k, v]) => [k, [...v.entries()]]) })); }
    for (const jid of [...dirtyChats]) {
      if (!all && n >= 15) break;               // al massimo 15 chat per giro: niente picchi di memoria/CPU
      dirtyChats.delete(jid); const list = raws.get(jid);
      if (!list || list.size === 0) { try { fs.unlinkSync(chatFile(jid)); } catch (_) {} continue; }
      const rs = [...reacts.entries()].filter(([k]) => k.startsWith(jid + '|')).map(([k, v]) => [k, [...v.entries()]]);
      writeAtomic(chatFile(jid), JSON.stringify({ jid, msgs: [...list.values()], reacts: rs }, B.BufferJSON.replacer)); n++;
    }
    if (Date.now() - t0 > 1500) log('salvataggio lento:', Date.now() - t0, 'ms,', n, 'chat');
  } catch (e) { log('salvataggio fallito:', e.message); }
}
function loadChatFile(jid) {
  lazyChats.delete(jid);
  try {
    const p = chatFile(jid); if (!fs.existsSync(p)) return;
    const o = JSON.parse(fs.readFileSync(p, 'utf8'), B.BufferJSON.reviver); const m = new Map(); Map.prototype.set.call(raws, jid, m);
    for (const x of o.msgs || []) m.set(x.key.id, x); for (const [k, v] of o.reacts || []) reacts.set(k, new Map(v));
  } catch (e) { log('file chat illeggibile', jid, e.message); }
}
function loadAllLazy() { for (const jid of [...lazyChats]) loadChatFile(jid); }
function loadStore() {
  try {
    if (fs.existsSync(STORE_OLD) && !fs.existsSync(META)) {      // migrazione dal vecchio file unico
      const o = JSON.parse(fs.readFileSync(STORE_OLD, 'utf8'), B.BufferJSON.reviver);
      for (const c of o.chats || []) chats.set(c.jid, c);
      for (const [k, v] of o.names || []) names.set(k, v);
      for (const [j, list] of Object.entries(o.raws || {})) { const m = rawList(j); for (const x of list) m.set(x.key.id, x); dirtyChats.add(j); }
      for (const [k, v] of o.reacts || []) reacts.set(k, new Map(v));
      dirtyMeta = true; saveStore(true); fs.renameSync(STORE_OLD, STORE_OLD + '.migrato'); log('store migrato a blocchi:', chats.size, 'chat'); return;
    }
    if (fs.existsSync(META)) { const o = JSON.parse(fs.readFileSync(META, 'utf8')); for (const c of o.chats || []) chats.set(c.jid, c); for (const [k, v] of o.names || []) names.set(norm(k), v); for (const [k, v] of o.pnames || []) pnames.set(k, v); for (const [k, v] of o.alias || []) alias.set(norm(k), norm(v)); for (const [k, v] of o.viewers || []) statusViewers.set(k, new Map(v)); }
    let files = 0;
    for (const f of fs.readdirSync(STORE_DIR)) {
      if (!f.startsWith('c_') || !f.endsWith('.json')) continue;
      try { lazyChats.add(Buffer.from(f.slice(2, -5), 'base64url').toString('utf8')); files++; } catch (_) {}
    }
    log('store caricato:', chats.size, 'chat,', files, 'file');
  } catch (e) { log('lettura store fallita:', e.message); }
}
setInterval(() => saveStore(false), 5000);
setInterval(() => { if (pendingLid.size) resolveLids(); }, 4000);
for (const sig of ['SIGINT', 'SIGTERM']) process.on(sig, () => { saveStore(true); process.exit(0); });


// ---------- file per il plugin delle notifiche (solo dentro la Vita): credenziali del client leggero, nomi delle chat, ultima notifica ----------
const LITE_DIR = 'ur0:tai/';
function writeLiteFiles(force) {
  if (!ENGINE || !sock || !B) return;
  try {
    const creds = sock.authState?.creds; if (!creds?.me?.id || !creds.noiseKey) return;
    if (force || !writeLiteFiles.done) {
      const cfg = { ...B.DEFAULT_CONNECTION_CONFIG, browser: [process.env.WA_DEVICE_NAME || 'PSVita', 'Chrome', '1.0.0'], syncFullHistory: false, countryCode: 'US' };
      const node = B.generateLoginNode(creds.me.id, cfg); const payload = Buffer.from(B.proto.ClientPayload.encode(node).finish());
      const ips = Buffer.from(globalThis.__nat.resolve4('web.whatsapp.com') || []);
      const me = [creds.me.id, creds.me.lid].filter(Boolean).map((x) => String(x).split(/[:@]/)[0]).join(','), meb = Buffer.from(me);   // numero e lid: i messaggi scritti da me, da un altro dispositivo, arrivano con uno dei due
      const head = Buffer.alloc(70); head.write('WAL2', 0); Buffer.from(creds.noiseKey.private).copy(head, 4); Buffer.from(creds.noiseKey.public).copy(head, 36); head.writeUInt16LE(payload.length, 68);
      const seed = Buffer.from(globalThis.__nat.randomBytes(32));
      if (ips.length) { fs.writeFileSync(LITE_DIR + 'wa_lite.bin', Buffer.concat([head, payload, Buffer.from([ips.length / 4]), ips, seed, Buffer.from([meb.length]), meb])); writeLiteFiles.done = true; log('plugin: credenziali del client leggero scritte (' + payload.length + ' byte, ' + ips.length / 4 + ' indirizzi)'); }
    }
    const lines = []; const seen = new Set();
    for (const c of [...chats.values()].sort((a, b) => (b.ts || 0) - (a.ts || 0)).slice(0, 300)) {
      const nm = String(c.name || '').replace(/[\t\r\n]+/g, ' ').slice(0, 50), fl = (c.archived || (c.muted && c.muted > Date.now())) ? '1' : '0';
      for (const j of [c.jid, alias.get(c.jid)]) if (j && !seen.has(j)) { seen.add(j); lines.push(j + '\t' + nm + '\t' + fl); }
    }
    let txt = lines.join('\n'); if (Buffer.byteLength(txt) > 22000) txt = txt.slice(0, 22000).replace(/\n[^\n]*$/, '');
    fs.writeFileSync(LITE_DIR + 'wa_names.txt', txt + '\n');
  } catch (e) { log('plugin: file non scritti:', e.message); }
}
function pluginLastNotif() {   // il plugin scrive "jid<TAB>millisecondi-dall'accensione"; vale 10 minuti
  if (!ENGINE) return null;
  try {
    const t = fs.readFileSync(LITE_DIR + 'wa_lastnotif.txt', 'utf8').trim().split('\t'); const age = globalThis.__nat.sysUptimeMs() - (parseInt(t[1], 10) || 0);
    if (!t[0] || age < -5000 || age > 10 * 60 * 1000) return null;
    const cand = [t[0], alias.get(t[0]), norm(t[0])]; for (const j of cand) if (j && chats.has(j)) return j;   // la chat puo' essere nota con l'altro indirizzo (numero o lid)
    return t[0];
  } catch (_) { return null; }
}
setInterval(() => writeLiteFiles(false), 10 * 60 * 1000);

// ---------- connessione ----------
async function start() {
  const first = !B;
  B = await import('@whiskeysockets/baileys');
  if (first) loadStore();
  const { default: makeWASocket, useMultiFileAuthState, fetchLatestBaileysVersion, DisconnectReason } = B;
  const { state: auth, saveCreds } = await useMultiFileAuthState(AUTH);
  let version;
  try { ({ version } = await fetchLatestBaileysVersion()); } catch (_) {}
  sock = makeWASocket({ auth, version, logger, browser: [process.env.WA_DEVICE_NAME || ('PSVita ' + require('os').hostname()), 'Chrome', '1.0.0'], syncFullHistory: false });
  sock.ev.on('creds.update', saveCreds);
  sock.ev.on('connection.update', async ({ connection, qr, lastDisconnect, isNewLogin }) => {
    if (isNewLogin) { log('nuovo login: parte la prima sincronizzazione'); sync.active = true; sync.progress = 0; sync.t = Date.now(); }
    if (connection) log('connessione:', connection, lastDisconnect?.error?.message || '');
    if (qr) { sync.active = false; state.qr = qr; state.status = 'qr'; console.log('QR pronto: http://localhost:' + PORT + '/qr.png'); }
    if (connection === 'open') { state.status = 'open'; state.qr = null; state.me = sock.user?.id || null; console.log('Connesso', state.me); setTimeout(() => writeLiteFiles(true), 3000); sock.fetchBlocklist().then((l) => { blocked.clear(); for (const j of l || []) if (j) blocked.add(B.jidNormalizedUser(j)); }).catch(() => {}); }
    if (connection === 'close') {
      const code = lastDisconnect?.error?.output?.statusCode;
      if (code === DisconnectReason.loggedOut) {
        state.status = 'loggedout';
        fs.rmSync(AUTH, { recursive: true, force: true }); fs.mkdirSync(AUTH, { recursive: true });
      } else state.status = 'reconnecting';
      setTimeout(() => start().catch(console.error), 2000);
    }
  });
  sock.ev.on('messaging-history.set', ({ chats: cs, contacts, messages: ms, progress, isLatest, lidPnMappings }) => {
    sync.t = Date.now(); if (typeof progress === 'number') sync.progress = progress; sync.active = !(isLatest || progress >= 100);
    log('sync: pacchetto cronologia chat=' + (cs || []).length + ' contatti=' + (contacts || []).length + ' messaggi=' + (ms || []).length + ' progresso=' + progress + ' ultimo=' + isLatest);
    for (const mp of lidPnMappings || []) link(mp.lid, mp.pn);
    for (const c of contacts || []) learnContact(c);
    dirtyMeta = true;
    for (const c of cs || []) chats.set(c.id, { jid: c.id, name: c.name || nameFor(c.id) || c.id.split('@')[0], unread: c.unreadCount || 0, ts: num(c.conversationTimestamp), pinned: c.pinned ? num(c.pinned) : 0 });
    for (const m of ms || []) addMessage(m, false);
  });
  sock.ev.on('contacts.upsert', (cs) => { for (const c of cs) learnContact(c); });
  sock.ev.on('contacts.update', (cs) => { for (const c of cs) learnContact(c); });
  sock.ev.on('lid-mapping.update', ({ lid, pn }) => { link(lid, pn); dirtyMeta = true; });
  sock.ev.on('chats.upsert', (cs) => { for (const c of cs) if (!chats.has(c.id)) chats.set(c.id, { jid: c.id, name: c.name || nameFor(c.id) || c.id.split('@')[0], unread: c.unreadCount || 0, ts: num(c.conversationTimestamp), pinned: c.pinned ? num(c.pinned) : 0 }); });
  sock.ev.on('messages.upsert', ({ messages: ms, type }) => {
    if (ms.length > 5) log('messages.upsert tipo=' + type + ' n=' + ms.length); for (const m of ms) addMessage(m, type === 'notify'); });
  sock.ev.on('messages.update', (ups) => {
    for (const u of ups) {
      const jid = u.key.remoteJid; const t = raws.get(jid)?.get(u.key.id); if (!t) continue;
      if (u.update.status != null) { t.status = u.update.status; dirtyChats.add(jid); }
      const ed = u.update.message?.editedMessage?.message || u.update.message?.protocolMessage?.editedMessage;
      if (ed) { t.message = ed; t.__edited = true; refreshLast(jid); }
    }
  });
  sock.ev.on('messages.reaction', (rs) => {
    for (const { key, reaction } of rs) {
      const k = key.remoteJid + '|' + key.id; if (!reacts.has(k)) reacts.set(k, new Map());
      const s = reaction.key?.participant || (reaction.key?.fromMe ? 'me' : key.remoteJid);
      if (reaction.text) reacts.get(k).set(s, reaction.text); else reacts.get(k).delete(s);
    }
  });
  setTimeout(() => { for (const j of chats.keys()) if (j.endsWith('@lid')) pendingLid.add(j); resolveLids(); }, 3000);
  sock.ev.on('chats.update', (ups) => {
    for (const u of ups) { const c = chats.get(u.id); if (!c) continue; if ('archived' in u) c.archived = !!u.archived; if ('muteEndTime' in u) c.muted = u.muteEndTime ? num(u.muteEndTime) : 0; if ('pinned' in u) c.pinned = u.pinned ? num(u.pinned) : 0; dirtyMeta = true; }
  });
  sock.ev.on('message-receipt.update', (ups) => {
    for (const { key, receipt } of ups) {
      if (key.remoteJid !== 'status@broadcast' || !key.fromMe || !receipt) continue;
      const t = num(receipt.readTimestamp || receipt.playedTimestamp); if (!t) continue;
      let m = statusViewers.get(key.id); if (!m) statusViewers.set(key.id, m = new Map());
      m.set(norm(receipt.userJid), t); dirtyMeta = true;
    }
  });
  sock.ev.on('blocklist.set', ({ blocklist }) => { blocked.clear(); for (const j of blocklist || []) blocked.add(B.jidNormalizedUser(j)); });
  sock.ev.on('blocklist.update', ({ blocklist, type }) => { for (const j of blocklist || []) { if (type === 'add') blocked.add(B.jidNormalizedUser(j)); else blocked.delete(B.jidNormalizedUser(j)); } });
  sock.ev.on('call', (cs) => {
    for (const c of cs) { const from = c.chatId || c.from; callAdd({ id: c.id, jid: from, video: !!c.isVideo, status: c.status, ts: Math.floor(Date.now() / 1000), out: false, dur: 0 }); }
  });
  sock.ev.on('presence.update', ({ id, presences }) => {
    const first = Object.values(presences || {})[0];
    if (first) presence.set(id, { state: first.lastKnownPresence, ts: Date.now(), lastSeen: first.lastSeen });
  });
}

// ---------- HTTP ----------
const json = (res, code, obj) => { res.writeHead(code, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(obj)); };
async function readBody(req, limit = 40e6) {
  const chunks = []; let n = 0;
  for await (const ch of req) { n += ch.length; if (n > limit) throw new Error('body troppo grande'); chunks.push(ch); }
  return Buffer.concat(chunks);
}
const mimeOf = (name) => ({ pdf: 'application/pdf', txt: 'text/plain', zip: 'application/zip', mp3: 'audio/mpeg', mp4: 'video/mp4', png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg', gif: 'image/gif', doc: 'application/msword', docx: 'application/vnd.openxmlformats-officedocument.wordprocessingml.document' }[(name.split('.').pop() || '').toLowerCase()] || 'application/octet-stream');

async function download(m) {
  try { return await B.downloadMediaMessage(m, 'buffer', {}, { logger, reuploadRequest: sock.updateMediaMessage }); }
  catch (e1) {   // link scaduto: si chiede al telefono di ricaricare il file e si riprova una volta
    log('download fallito, chiedo il file di nuovo al telefono:', String(e1.message || e1).slice(0, 80));
    let m2; try { m2 = await Promise.race([sock.updateMediaMessage(m), new Promise((_, rej) => setTimeout(() => rej(new Error('il telefono non risponde')), 25000))]); } catch (e2) { log('ricarica non riuscita:', e2.message); throw e1; }
    return B.downloadMediaMessage(m2, 'buffer', {}, { logger, reuploadRequest: sock.updateMediaMessage });
  }
}
async function downloadOld(m) {
  return B.downloadMediaMessage(m, 'buffer', {}, { logger, reuploadRequest: sock.updateMediaMessage });
}
function cacheSet(k, v) { mediaCache.set(k, v); if (mediaCache.size > 60) mediaCache.delete(mediaCache.keys().next().value); }

const MEDIA_DIR = path.join(DATA, 'mediacache'); fs.mkdirSync(MEDIA_DIR, { recursive: true });
try { const fl = fs.readdirSync(MEDIA_DIR).map((f) => [f, fs.statSync(path.join(MEDIA_DIR, f)).mtimeMs]).sort((a, b) => a[1] - b[1]); for (const [f] of fl.slice(0, Math.max(0, fl.length - 600))) fs.unlinkSync(path.join(MEDIA_DIR, f)); } catch (_) {}
const mediaFile = (k) => path.join(MEDIA_DIR, require('crypto').createHash('sha1').update(k).digest('hex'));
// ---------- video: conversione nel formato che la Vita legge (H.264 Main 3.1, max 640 px, AAC stereo) ----------
const VIDEO_DIR = path.join(DATA, 'videocache'); fs.mkdirSync(VIDEO_DIR, { recursive: true });
try { const fl = fs.readdirSync(VIDEO_DIR).map((f) => { const st = fs.statSync(path.join(VIDEO_DIR, f)); return [f, st.mtimeMs, st.size]; }).sort((a, b) => b[1] - a[1]); let tot = 0; for (const [f, , sz] of fl) { tot += sz; if (tot > 3e9) fs.unlinkSync(path.join(VIDEO_DIR, f)); } } catch (_) {}   // video/audio convertiti: si tengono finche' stanno in 3 GB, si butta il piu' vecchio
const converting = new Map();
function convertForVita(jid, id) {
  const key = require('crypto').createHash('sha1').update(jid + '|' + id).digest('hex'); const out = path.join(VIDEO_DIR, key + '.mp4');
  if (fs.existsSync(out)) return Promise.resolve(out);
  if (converting.has(key)) return converting.get(key);
  const pr = (async () => {
    const m = raws.get(jid)?.get(id); if (!m) throw new Error('messaggio non trovato');
    const src = path.join(VIDEO_DIR, key + '.src'); fs.writeFileSync(src, await download(m)); const tmp = path.join(VIDEO_DIR, key + '.tmp.mp4'); const t0 = Date.now();
    await new Promise((resolve, reject) => require('child_process').execFile('ffmpeg', ['-y', '-i', src, '-vf', "scale=w='min(720,iw)':h='min(480,ih)':force_original_aspect_ratio=decrease:force_divisible_by=2,format=yuv420p", '-c:v', 'libx264', '-profile:v', 'main', '-level', '3.1', '-preset', 'veryfast', '-bf', '0', '-refs', '2', '-g', '60', '-crf', '26', '-maxrate', '2500k', '-bufsize', '5000k', '-r', '30', '-c:a', 'aac', '-b:a', '128k', '-ac', '2', '-ar', '44100', '-movflags', '+faststart', tmp], { timeout: 300000 }, (e, so, se) => e ? reject(new Error('ffmpeg: ' + String(se || '').slice(-200))) : resolve()));
    fs.renameSync(tmp, out); try { fs.unlinkSync(src); } catch (_) {} log('video convertito per la Vita in', Date.now() - t0, 'ms:', fs.statSync(out).size, 'byte'); return out;
  })().finally(() => { converting.delete(key); try { fs.unlinkSync(path.join(VIDEO_DIR, key + '.src')); fs.unlinkSync(path.join(VIDEO_DIR, key + '.tmp.mp4')); } catch (_) {} });
  converting.set(key, pr); return pr;
}
// ---------- video, audio e file per la Vita, con avanzamento reale ----------
// L'app avvia un lavoro (/prep), ne legge lo stato (/prep-status: scarico da WhatsApp, conversione con percentuale) e poi scarica il risultato (/prep-file).
const preps = new Map();   // chiave -> { state: download|convert|ready|error, pct, file, ext, size, err }
const sha1hex = (t) => require('crypto').createHash('sha1').update(t).digest('hex');
// colori e proporzioni dei pixel sempre normalizzati (bt709, nessun rapporto d'aspetto dei pixel): alcuni video di WhatsApp li hanno incompleti e il decodificatore della Vita li rifiuta
const VITA_VF = "scale=w='min(720,iw)':h='min(544,ih)':force_original_aspect_ratio=decrease:force_divisible_by=16:out_color_matrix=bt709:out_range=tv,setsar=0,format=yuv420p";
const VITA_COLOR = ['-colorspace', 'bt709', '-color_primaries', 'bt709', '-color_trc', 'bt709', '-color_range', 'tv'];
function probeDuration(file) { return new Promise((res) => require('child_process').execFile('ffprobe', ['-v', 'error', '-show_entries', 'format=duration', '-of', 'csv=p=0', file], { timeout: 20000 }, (e, so) => res(e ? 0 : parseFloat(so) || 0))); }
function runFfmpeg(args, durSec, onPct, bg, holder) {
  return new Promise((resolve, reject) => {
    const fa = [...args, '-progress', 'pipe:1', '-nostats', '-loglevel', 'error'];
    const cp = bg ? require('child_process').spawn('nice', ['-n', '10', 'ffmpeg', ...fa], { stdio: ['ignore', 'pipe', 'pipe'] }) : require('child_process').spawn('ffmpeg', fa, { stdio: ['ignore', 'pipe', 'pipe'] });
    if (holder) holder.cp = cp;
    let err = '', buf = '';
    cp.stderr.on('data', (d) => { err = (err + d).slice(-300); });
    cp.stdout.on('data', (d) => { buf += d; let i; while ((i = buf.indexOf('\n')) >= 0) { const line = buf.slice(0, i).trim(); buf = buf.slice(i + 1); const m = /^out_time_(?:us|ms)=(\d+)/.exec(line); if (m && durSec > 0) onPct(Math.min(99, Math.round(100 * Number(m[1]) / 1e6 / durSec))); } });
    const t = setTimeout(() => { cp.kill('SIGKILL'); reject(new Error('conversione troppo lunga')); }, 600000);
    cp.on('close', (c) => { clearTimeout(t); c === 0 ? resolve() : reject(new Error('ffmpeg: ' + err)); });
  });
}
function prepStart(jid, id, kind, bg) {
  const key = sha1hex(jid + '|' + id + '|' + kind + '|r11'), old = preps.get(key);
  // se l'utente chiede un file che sta convertendo in background a bassa priorita', si ferma e si rifa' a priorita' normale
  if (old && old.bg && !bg) {
    old.bg = false;   // da ora ha la priorita' dell'utente
    if (old.state === 'convert' && old.cp) { old.restarting = true; try { old.cp.kill('SIGKILL'); } catch (_) {} return key; }   // il catch qui sotto lo riavvia a priorita' normale
  }
  if (old && old.state !== 'error' && !(old.state === 'ready' && !fs.existsSync(old.file))) return key;
  const p = { state: 'download', pct: 0, file: '', ext: kind === 'video' ? 'mp4' : kind === 'audio' ? 'm4a' : 'bin', size: 0, err: '', bg: !!bg, cp: null }; preps.set(key, p);
  (async () => {
    const out = path.join(VIDEO_DIR, key + '.' + p.ext), src = path.join(VIDEO_DIR, key + '.src'), tmp = path.join(VIDEO_DIR, key + '.tmp.' + p.ext); const t0 = Date.now();
    try {
      if (fs.existsSync(out)) { p.file = out; p.size = fs.statSync(out).size; p.pct = 100; p.state = 'ready'; return; }
      const m = raws.get(jid)?.get(id); if (!m) throw new Error('messaggio non trovato');
      fs.writeFileSync(src, await download(m));
      if (kind === 'file') fs.renameSync(src, out);
      else {
        p.state = 'convert'; p.pct = 0; const dur = await probeDuration(src);
        const args = kind === 'video'
          ? ['-y', '-i', src, '-vf', VITA_VF, ...VITA_COLOR, '-c:v', 'libx264', '-profile:v', 'main', '-level', '3.1', '-preset', 'veryfast', '-bf', '0', '-refs', '2', '-g', '60', '-crf', '26', '-maxrate', '2500k', '-bufsize', '5000k', '-r', '30', '-c:a', 'aac', '-b:a', '128k', '-ac', '2', '-ar', '44100', '-movflags', '+faststart', tmp]
          : ['-y', '-i', src, '-vn', '-c:a', 'aac', '-b:a', '96k', '-ac', '2', '-ar', '44100', '-movflags', '+faststart', tmp];
        await runFfmpeg(args, dur, (v) => { p.pct = v; }, p.bg, p); fs.renameSync(tmp, out);
      }
      p.file = out; p.size = fs.statSync(out).size; p.pct = 100; p.state = 'ready'; log('preparato', kind, 'in', Date.now() - t0, 'ms:', p.size, 'byte');
    } catch (e) {
      if (p.restarting) { preps.delete(key); prepStart(jid, id, kind, false); return; }
      p.state = 'error'; p.err = String(e.message || e).slice(0, 120); log('preparazione fallita', kind, p.err);
    }
    finally { for (const f of [src, tmp]) { try { fs.unlinkSync(f); } catch (_) {} } }
  })();
  return key;
}
// Video in arrivo (storie e chat): si convertono in background, uno alla volta, cosi' sulla Vita sono gia' pronti.
const bgQueue = []; let bgBusy = false;
function bgPrep(jid, id) {
  const key = sha1hex(jid + '|' + id + '|video|r11'); const x = preps.get(key);
  if ((x && x.state !== 'error') || bgQueue.some((t) => t[2] === key)) return;
  if (fs.existsSync(path.join(VIDEO_DIR, key + '.mp4'))) return;
  bgQueue.push([jid, id, key]); bgNext();
}
function bgNext() {
  if (bgBusy || !bgQueue.length) return; bgBusy = true; const [jid, id, key] = bgQueue.shift();
  prepStart(jid, id, 'video', true); const t = setInterval(() => { const x = preps.get(key); if (!x || x.state === 'ready' || x.state === 'error') { clearInterval(t); bgBusy = false; bgNext(); } }, 500);
}
// Le foto si preparano in anticipo (download da WhatsApp + riduzione) con al massimo 2 lavori alla volta; una richiesta che arriva
// mentre la stessa foto e' gia' in preparazione si accoda a quella invece di rifare tutto.
// Versione grande (max 1280 px) per il visualizzatore con zoom; si salva su disco e non resta in memoria.
const bigInflight = new Map();
async function bigMediaFor(jid, id) {
  const k = jid + '|' + id, f = mediaFile(k) + '.big.jpg';
  try { return { type: 'image/jpeg', buf: fs.readFileSync(f) }; } catch (_) {}
  if (bigInflight.has(k)) return bigInflight.get(k);
  const pr = (async () => {
    const m = raws.get(jid)?.get(id); if (!m) return null;
    const d = describe(m); if (!d || d.kind !== 'image' || m.key.remoteJid.endsWith('@newsletter')) return null;
    const buf = await sharp(await download(m)).rotate().resize(1280, 1280, { fit: 'inside', withoutEnlargement: true }).jpeg({ quality: 80 }).toBuffer();
    try { fs.writeFileSync(f, buf); } catch (_) {}
    return { type: 'image/jpeg', buf };
  })().finally(() => bigInflight.delete(k));
  bigInflight.set(k, pr); return pr;
}
const mfInflight = new Map();
function mediaFor(jid, id) {
  const k = jid + '|' + id; if (mfInflight.has(k)) return mfInflight.get(k);
  const pr = mediaForRaw(jid, id).finally(() => mfInflight.delete(k)); mfInflight.set(k, pr); return pr;
}
const pfQueue = [], pfSeen = new Set(); let pfBusy = 0;
function prefetchMedia(jid, id) {
  const k = jid + '|' + id; if (pfSeen.has(k) || mediaCache.has(k) || pfQueue.length > 300) return;
  if (pfSeen.size > 5000) pfSeen.clear(); pfSeen.add(k); pfQueue.push([jid, id]); pfPump();
}
function pfPump() {
  while (pfBusy < 2 && pfQueue.length) {
    const [j, i] = pfQueue.shift(); pfBusy++;
    mediaFor(j, i).catch(() => {}).finally(() => { pfBusy--; setTimeout(pfPump, 30); });
  }
}
function prefetchChat(jid, n) {
  const list = [...(raws.get(jid)?.values() || [])].sort((a, b) => num(b.messageTimestamp) - num(a.messageTimestamp));
  let c = 0; for (const m of list) { if (c >= n) break; const d = describe(m); if (d && (d.kind === 'image' || d.kind === 'sticker')) { prefetchMedia(jid, m.key.id); c++; } }
}
// la miniatura puo' essere un Buffer, un Uint8Array, {type:'Buffer',data} oppure una stringa base64 (come nei messaggi arrivati in diretta)
function thumbBuf(th) {
  if (!th) return null;
  if (typeof th === 'string') return Buffer.from(th, 'base64');
  if (th.type === 'Buffer' && th.data) return Buffer.from(th.data, typeof th.data === 'string' ? 'base64' : undefined);
  return Buffer.from(th);
}
const VIDEO_PLACEHOLDER = '/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAA0JCgsKCA0LCgsODg0PEyAVExISEyccHhcgLikxMC4pLSwzOko+MzZGNywtQFdBRkxOUlNSMj5aYVpQYEpRUk//2wBDAQ4ODhMREyYVFSZPNS01T09PT09PT09PT09PT09PT09PT09PT09PT09PT09PT09PT09PT09PT09PT09PT09PT0//wAARCAC0AUADASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDhqKKKoQUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFTWVv9rvre23bPOkWPdjOMnGcVc1bQ7/AEgg3UamNjhZUOVJxnHqPxA6GgDNooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAu6L/wAhyw/6+Y//AEIV6y6JIjJIqsjAhlYZBB7GvJtF/wCQ5Yf9fMf/AKEK9boA5jVvBtndbpbBvssxydnWNjz2/h5x04AHSuIv9OvNOlEd7bvEx6E8hunQjg9R0r16mTQxXERiniSWNuqOoYH8DQI8aortdZ8FBi02kOq8f8e7k9h2Y+vHB9+a5C7tLmynMN3C8Ug7MMZGcZHqOOooGQ0UUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFAF3Rf+Q5Yf9fMf/oQr1uvJNF/5Dlh/wBfMf8A6EK9boAKKKKBBVe+sbXULc295CssZIODkYI7gjkVYooA4TVfBM8WZNLl89f+eUhAft0PQ9z2/GuTdHjdkkVldSQysMEEdjXsk00VvEZZ5UijXq7sFA/E1wPibxBp+pZitrBJWXgXMoKtjn7uOcc5GT9RQM5iiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigC7ov/IcsP+vmP/0IV63Xkmi/8hyw/wCvmP8A9CFet0AFFFc9qvi7TrHKW5+2TekbfIOnVvoe2enagRvu6RozyMqooJZmOAAO5rmNZ8ZWtsGi0zbczg43kHyxzz6E9O3HOc9q5DVtZvtWl3XUmEGNsSZCKR3xnryefes+gZav9RvNRlEl7cPKw6A8BenQDgdB0qrRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAXdF/5Dlh/wBfMf8A6EK9E1rxDY6QpWRvOuOghjIyDjI3f3R0/Poa8vR3jdXjZldSCrKcEEdxSUAbWreJ9R1PdH5n2e3OR5URxkc/ePU8HB7e1YtFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFABRRRQAUUUUAFFFFAH/2Q==';
async function mediaForRaw(jid, id) {
  const k = jid + '|' + id; if (mediaCache.has(k)) return mediaCache.get(k);
  for (const [ext, type] of [['.jpg', 'image/jpeg'], ['.png', 'image/png']]) { try { const buf = fs.readFileSync(mediaFile(k) + ext); const out0 = { type, buf }; cacheSet(k, out0); return out0; } catch (_) {} }
  const m = raws.get(jid)?.get(id); if (!m) return null;
  const d = describe(m); let out = null;
  if ((d.kind === 'image' || d.kind === 'video') && m.key.remoteJid.endsWith('@newsletter')) { const th = thumbBuf(unwrap(m).imageMessage?.jpegThumbnail || unwrap(m).videoMessage?.jpegThumbnail); if (th) out = { type: 'image/jpeg', buf: await sharp(th).resize(320, null).jpeg({ quality: 80 }).toBuffer() }; }
  else if (d.kind === 'image') out = { type: 'image/jpeg', buf: await sharp(await download(m)).rotate().resize(480, 480, { fit: 'inside', withoutEnlargement: true }).jpeg({ quality: 72 }).toBuffer() };
  else if (d.kind === 'sticker') out = { type: 'image/png', buf: await sharp(await download(m), { animated: false }).resize(192, 192, { fit: 'inside', withoutEnlargement: true }).png().toBuffer() };
  else if (d.kind === 'video') {
    const th = thumbBuf(unwrap(m).videoMessage?.jpegThumbnail);
    if (th) { try { out = { type: 'image/jpeg', buf: await sharp(th).resize(320, null).jpeg({ quality: 80 }).toBuffer() }; } catch (e) { log('miniatura video non leggibile:', e.message); } }
    else log('video senza miniatura nel messaggio:', id);
    if (!out) return { type: 'image/jpeg', buf: Buffer.from(VIDEO_PLACEHOLDER, 'base64') };   // senza miniatura: riquadro scuro con il simbolo di play (non si salva: se la miniatura arriva dopo, si usa quella)
  }
  if (out) { cacheSet(k, out); try { fs.writeFileSync(mediaFile(k) + (out.type === 'image/png' ? '.png' : '.jpg'), out.buf); } catch (_) {} }
  return out;
}

async function sendText(jid, text, quoteId, editId) {
  let sent;
  if (editId) {
    const t = raws.get(jid)?.get(editId); if (!t) throw new Error('messaggio non trovato');
    sent = await sock.sendMessage(jid, { text, edit: t.key });
    t.message = { conversation: text }; t.__edited = true; refreshLast(jid);
    return sent;
  }
  const q = quoteId ? raws.get(jid)?.get(quoteId) : undefined;
  sent = await sock.sendMessage(jid, { text }, q ? { quoted: q } : {});
  addMessage(sent, false);
  return sent;
}

http.createServer(async (req, res) => {
  const u = new URL(req.url, 'http://x');
  const p = u.pathname; const q = u.searchParams;
  const arg = (prefix) => decodeURIComponent(p.slice(prefix.length));
  try {
    if (p === '/lastnotif') {   // chat dell'ultima notifica (valida 10 minuti); con ?take=1 viene consumata
      const take = new URL(req.url, 'http://x').searchParams.get('take');
      const pj = pluginLastNotif(); if (pj) { if (take) { try { fs.unlinkSync(LITE_DIR + 'wa_lastnotif.txt'); } catch (_) {} } return json(res, 200, { jid: pj, age: 0 }); }
      const n = lastNotif, ok = n && !n.taken && Date.now() - n.ts < 10 * 60 * 1000;
      if (ok && take) n.taken = true;
      return json(res, 200, ok ? { jid: n.jid, age: Math.round((Date.now() - n.ts) / 1000) } : { jid: '' });
    }
    if (p === '/debug/call' && /^(127\.0\.0\.1|::1|::ffff:127\.0\.0\.1)$/.test(req.socket.remoteAddress || '')) {   // solo dal PC: simula una voce del registro chiamate
      const q = new URL(req.url, 'http://x').searchParams;
      callAdd({ id: 'dbg-' + Date.now(), jid: q.get('jid') || '0@s.whatsapp.net', video: q.get('video') === '1', status: q.get('status') || 'timeout', ts: Math.floor(Date.now() / 1000), out: q.get('out') === '1', dur: parseInt(q.get('dur') || '0', 10) });
      return json(res, 200, { ok: true });
    }
    if (p === '/debug/event' && /^(127\.0\.0\.1|::1|::ffff:127\.0\.0\.1)$/.test(req.socket.remoteAddress || '')) {   // solo dal PC del bridge: simula un messaggio in arrivo per provare le notifiche
      const q = new URL(req.url, 'http://x').searchParams;
      events.push({ seq: ++evSeq, jid: q.get('jid') || 'test@s.whatsapp.net', title: q.get('title') || 'Prova', text: q.get('text') || 'Messaggio di prova', ts: Math.floor(Date.now() / 1000) });
      return json(res, 200, { ok: true, seq: evSeq });
    }
    if (p === '/events') { lastPoll.ip = (req.socket.remoteAddress || '').replace('::ffff:', ''); lastPoll.t = Date.now(); lastPoll.n++; }
    if (p === '/status') {
      const act = sync.active && Date.now() - sync.t < 120000;
      return json(res, 200, { plugin_poll: { ip: lastPoll.ip, ago: lastPoll.t ? Math.round((Date.now() - lastPoll.t) / 1000) : null, count: lastPoll.n }, status: state.status, me: state.me, qr: state.qr, sync: { active: act, progress: Math.round(sync.progress), chats: chats.size } });
    }
    if (p === '/qr.png') {
      if (!state.qr) return json(res, 404, { error: 'no qr' });
      const buf = await QRCode.toBuffer(state.qr, { width: 400, margin: 2 });
      res.writeHead(200, { 'Content-Type': 'image/png' }); return res.end(buf);
    }
    if (state.status !== 'open' && p !== '/events') return json(res, 503, { error: 'non connesso' });

    if (p === '/chats') {
      const list = [...chats.values()].sort((a, b) => (b.pinned ? 1 : 0) - (a.pinned ? 1 : 0) || (b.ts || 0) - (a.ts || 0)).slice(0, Math.min(400, parseInt(q.get('limit') || '400', 10)))
        .map((c) => ({ jid: c.jid, name: c.jid.endsWith('@newsletter') ? (chMetaFor(c.jid)?.name || (c.name && !/^\d+$/.test(c.name) ? c.name : 'Canale')) : nameOf(c.jid), channel: c.jid.endsWith('@newsletter'), last: c.last, ts: c.ts, unread: c.unread, archived: !!c.archived, pinned: c.pinned ? 1 : 0, muted: c.muted && c.muted > Date.now() ? 1 : 0, blocked: blocked.has(c.jid) }));
      return json(res, 200, list);
    }
    if (p.startsWith('/messages/') && arg('/messages/').endsWith('@newsletter')) {
      const jid = arg('/messages/'); const n = Math.min(100, parseInt(q.get('n') || '30', 10)); const c = chCache.get(jid);
      let list = c && Date.now() - c.ts < 20000 && c.n >= n ? c.msgs : null;
      if (!list) {
        try {
          const node = await Promise.race([sock.query({ tag: 'iq', attrs: { id: sock.generateMessageTag(), type: 'get', xmlns: 'newsletter', to: 's.whatsapp.net' }, content: [{ tag: 'messages', attrs: { type: 'jid', jid, count: String(n) } }] }), new Promise((_, rej) => setTimeout(() => rej(new Error('timeout 10 s')), 10000))]); list = [];
          const walk = (nd) => {
            if (!nd) return;
            if (nd.tag === 'message' && nd.attrs && Array.isArray(nd.content)) {
              const pt = nd.content.find((x) => x.tag === 'plaintext' && x.content);
              if (pt) {
                try {
                  const msg = B.proto.Message.decode(pt.content);
                  const rx = nd.content.find((x) => x.tag === 'reactions'); const vw = nd.content.find((x) => x.tag === 'views_count');
                  const reactions = (Array.isArray(rx?.content) ? rx.content : []).filter((r) => r.tag === 'reaction' && r.attrs?.code).slice(0, 4).map((r) => ({ e: r.attrs.code, n: Number(r.attrs.count || 1) }));
                  list.push({ key: { remoteJid: jid, id: String(nd.attrs.server_id || nd.attrs.id), fromMe: false }, messageTimestamp: Number(nd.attrs.t || 0), message: msg, __views: vw?.attrs?.count, __reactions: reactions });
                } catch (_) {}
              }
            }
            if (Array.isArray(nd.content)) nd.content.forEach(walk);
          };
          walk(node); list.sort((a, b) => a.messageTimestamp - b.messageTimestamp);
          chCache.set(jid, { ts: Date.now(), n, msgs: list });
          const rl = rawList(jid); for (const m of list) rl.set(m.key.id, m);       // cosi' /media puo' trovarli
          const ch = chats.get(jid); const last = list[list.length - 1]; if (ch && last) { const d = describe(last); if (d) { ch.last = preview(d); ch.ts = last.messageTimestamp; } }
        } catch (e) { log('canale: lettura fallita', jid, e.message); list = c?.msgs || []; }
      }
      return json(res, 200, list.slice(-n).map((m) => { const v = view(m); if (m.__views) v.views = Number(m.__views); if (m.__reactions?.length) v.reactions = m.__reactions; if (v.text && v.text.length > 1800) v.text = v.text.slice(0, 1800) + '...'; return v; }));
    }
    if (p.startsWith('/messages/')) {
      const jid = arg('/messages/'); prefetchChat(jid, ENGINE ? 4 : 12);
      const list = [...(raws.get(jid)?.values() || [])].sort((a, b) => num(a.messageTimestamp) - num(b.messageTimestamp));
      const nlim = Math.min(300, parseInt(q.get('n') || '30', 10));
      // l'app chiede piu' messaggi di quelli che ci sono: si chiedono al telefono quelli piu' vecchi (arrivano come pacchetto di cronologia)
      if (list.length > 0 && list.length < nlim + 1 && sock && !oldAsked.has(jid)) {
        oldAsked.set(jid, Date.now()); const o = list[0];
        sock.fetchMessageHistory(50, o.key, num(o.messageTimestamp)).then(() => log('cronologia piu\' vecchia richiesta per', jid)).catch((e) => log('cronologia piu\' vecchia non richiesta:', e.message));
        setTimeout(() => oldAsked.delete(jid), 45000);
      }
      return json(res, 200, list.slice(-nlim).map((m) => { const v = view(m); if (v.text && v.text.length > 1800) v.text = v.text.slice(0, 1800) + '...'; return v; }));
    }
    if (p.startsWith('/read/')) {
      const jid = arg('/read/'); const c = chats.get(jid); if (c) c.unread = 0;
      const keys = [...(raws.get(jid)?.values() || [])].filter((m) => !m.key.fromMe).slice(-20).map((m) => m.key);
      if (keys.length) sock.readMessages(keys).catch(() => {});
      return json(res, 200, { ok: true });
    }
    if (p.startsWith('/presence/')) {
      const jid = arg('/presence/');
      if (!subscribed.has(jid)) { subscribed.add(jid); sock.presenceSubscribe(jid).catch(() => {}); }
      const pr = presence.get(jid);
      return json(res, 200, { state: pr && Date.now() - pr.ts < 30000 ? pr.state : (pr?.state === 'available' ? 'available' : 'unavailable') });
    }
    if (p.startsWith('/avatar/')) {                    // le foto profilo si ricordano: 1 ora quelle trovate, 10 minuti quelle assenti
      const aj = arg('/avatar/'); const hit = avatarCache.get(aj);
      if (hit && Date.now() - hit.ts < (hit.buf ? 3600000 : 600000)) { if (!hit.buf) return json(res, 404, { error: 'no avatar' }); res.writeHead(200, { 'Content-Type': hit.type }); return res.end(hit.buf); }
      const url = await Promise.race([sock.profilePictureUrl(aj, 'preview').catch(() => null), new Promise((ok) => setTimeout(() => ok(null), 7000))]);   // se WhatsApp non risponde entro 7 s si lascia perdere (si riprova fra 10 minuti)
      if (!url) { avatarCache.set(aj, { ts: Date.now(), buf: null }); return json(res, 404, { error: 'no avatar' }); }
      const r = await fetch(url); const buf = Buffer.from(await r.arrayBuffer()); const type = r.headers.get('content-type') || 'image/jpeg';
      avatarCache.set(aj, { ts: Date.now(), buf, type }); res.writeHead(200, { 'Content-Type': type }); return res.end(buf);
    }
    if (p.startsWith('/media/')) {
      const [jid, id] = arg('/media/').split('/');
      const m = q.get('big') ? await bigMediaFor(jid, id) : await mediaFor(jid, id);
      if (!m) return json(res, 404, { error: 'no media' });
      res.writeHead(200, { 'Content-Type': m.type }); return res.end(m.buf);
    }
    if (p.startsWith('/prep/')) {
      const [jid, id] = arg('/prep/').split('/'); const kind = q.get('kind') || 'file';
      if (!['video', 'audio', 'file'].includes(kind)) return json(res, 400, { error: 'tipo non valido' });
      return json(res, 200, { key: prepStart(jid, id, kind) });
    }
    if (p.startsWith('/prep-status/')) { const x = preps.get(arg('/prep-status/')); if (!x) return json(res, 404, { error: 'sconosciuto' }); return json(res, 200, { state: x.state, pct: x.pct, size: x.size, err: x.err }); }
    if (p.startsWith('/prep-file/')) {
      const x = preps.get(arg('/prep-file/')); if (!x || x.state !== 'ready' || !fs.existsSync(x.file)) return json(res, 404, { error: 'non pronto' });
      res.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': fs.statSync(x.file).size }); return fs.createReadStream(x.file).pipe(res);
    }
    if (p.startsWith('/vita-video/')) {
      const [jid, id] = arg('/vita-video/').split('/');
      const file = await convertForVita(jid, id);
      res.writeHead(200, { 'Content-Type': 'video/mp4', 'Content-Length': fs.statSync(file).size }); return fs.createReadStream(file).pipe(res);
    }
    if (p.startsWith('/file/')) {
      const [jid, id] = arg('/file/').split('/');
      const m = raws.get(jid)?.get(id); if (!m) return json(res, 404, { error: 'no msg' });
      const buf = await download(m);
      res.writeHead(200, { 'Content-Type': 'application/octet-stream' }); return res.end(buf);
    }
    if (p === '/send' && req.method === 'POST') {
      const { jid, text, quote, edit } = JSON.parse((await readBody(req, 1e6)).toString());
      await sendText(jid, text, quote, edit);
      return json(res, 200, { ok: true });
    }
    if (p === '/sendmedia' && req.method === 'POST') {
      const jid = q.get('jid'); const kind = q.get('kind'); const name = q.get('name') || 'file'; const caption = q.get('caption') || undefined;
      const qm = q.get('quote') ? raws.get(jid)?.get(q.get('quote')) : undefined;
      let buf = await readBody(req);
      let content;
      if (kind === 'image') { buf = await sharp(buf).rotate().resize(1600, 1600, { fit: 'inside', withoutEnlargement: true }).jpeg({ quality: 82 }).toBuffer(); content = { image: buf, caption }; }
      else if (kind === 'video') content = { video: buf, caption };
      else if (kind === 'audio') content = { audio: buf, mimetype: mimeOf(name) === 'application/octet-stream' ? 'audio/mpeg' : mimeOf(name) };
      else content = { document: buf, fileName: name, mimetype: mimeOf(name), caption };
      const sent = await sock.sendMessage(jid, content, qm ? { quoted: qm } : {});
      addMessage(sent, false);
      return json(res, 200, { ok: true });
    }
    if (p === '/react' && req.method === 'POST') {
      const { jid, id, emoji } = JSON.parse((await readBody(req, 1e5)).toString());
      const t = raws.get(jid)?.get(id); if (!t) return json(res, 404, { error: 'no msg' });
      await sock.sendMessage(jid, { react: { text: emoji, key: t.key } });
      const k = jid + '|' + id; if (!reacts.has(k)) reacts.set(k, new Map());
      if (emoji) reacts.get(k).set('me', emoji); else reacts.get(k).delete('me');
      return json(res, 200, { ok: true });
    }
    if (p === '/delete' && req.method === 'POST') {
      const { jid, id } = JSON.parse((await readBody(req, 1e5)).toString());
      const t = raws.get(jid)?.get(id); if (!t) return json(res, 404, { error: 'no msg' });
      await sock.sendMessage(jid, { delete: t.key });
      t.__deleted = true; refreshLast(jid);
      return json(res, 200, { ok: true });
    }

    const lastMsgs = (jid) => {
      const l = [...(raws.get(jid)?.values() || [])].sort((a, b) => num(a.messageTimestamp) - num(b.messageTimestamp)); const m = l[l.length - 1];
      return m ? [{ key: m.key, messageTimestamp: num(m.messageTimestamp) }] : [{ key: { remoteJid: jid, fromMe: false, id: '0' }, messageTimestamp: Math.floor(Date.now() / 1000) }];
    };
    const body = async (lim = 1e6) => JSON.parse((await readBody(req, lim)).toString() || '{}');

    if (p === '/search') {
      const qs = (q.get('q') || '').toLowerCase().trim(); const only = q.get('jid');
      if (!qs) return json(res, 200, { chats: [], messages: [] });
      const cs = only ? [] : [...chats.values()].filter((c) => nameOf(c.jid).toLowerCase().includes(qs)).slice(0, 20).map((c) => ({ jid: c.jid, name: nameOf(c.jid) }));
      const ms = [];
      for (const [jid, list] of raws) {
        if (jid === 'status@broadcast' || (only && jid !== only)) continue;
        for (const m of list.values()) { const v = view(m); if (v.text && v.text.toLowerCase().includes(qs)) ms.push({ jid, chat: nameOf(jid), id: v.id, text: v.text, ts: v.ts }); }
      }
      ms.sort((a, b) => b.ts - a.ts);
      return json(res, 200, { chats: cs, messages: ms.slice(0, 40) });
    }
    if (p === '/checknumber') {
      const num0 = (q.get('num') || '').replace(/[^0-9]/g, '');
      if (num0.length < 6) return json(res, 200, { exists: false });
      const r = await sock.onWhatsApp(num0 + '@s.whatsapp.net');
      const f = Array.isArray(r) ? r[0] : null;
      if (!f || !f.exists) return json(res, 200, { exists: false });
      const jid = f.jid; if (q.get('name')) names.set(jid, q.get('name'));
      if (!chats.has(jid)) chats.set(jid, { jid, name: q.get('name') || num0, unread: 0, ts: Math.floor(Date.now() / 1000), last: '' });
      dirtyMeta = true; return json(res, 200, { exists: true, jid });
    }
    if (p === '/block' && req.method === 'POST') {
      const { jid, block } = await body();
      await sock.updateBlockStatus(jid, block ? 'block' : 'unblock');
      if (block) blocked.add(jid); else blocked.delete(jid);
      return json(res, 200, { ok: true });
    }
    if (p === '/chatmod' && req.method === 'POST') {
      const { jid, action, until } = await body();
      if (action === 'mute') { const t = until > 0 ? Date.now() + until : null; await sock.chatModify({ mute: t }, jid); const c = chats.get(jid); if (c) c.muted = t || 0; }
      else if (action === 'pin' || action === 'unpin') { await sock.chatModify({ pin: action === 'pin' }, jid); const c = chats.get(jid); if (c) c.pinned = action === 'pin' ? Math.floor(Date.now() / 1000) : 0; }
      else if (action === 'archive' || action === 'unarchive') { await sock.chatModify({ archive: action === 'archive', lastMessages: lastMsgs(jid) }, jid); const c = chats.get(jid); if (c) c.archived = action === 'archive'; }
      else if (action === 'clear') { await sock.chatModify({ clear: true, lastMessages: lastMsgs(jid) }, jid); raws.get(jid)?.clear(); const c = chats.get(jid); if (c) c.last = ''; }
      else if (action === 'delete') { await sock.chatModify({ delete: true, lastMessages: lastMsgs(jid) }, jid); raws.delete(jid); chats.delete(jid); }
      else return json(res, 400, { error: 'azione sconosciuta' });
      dirtyMeta = true; dirtyChats.add(jid); return json(res, 200, { ok: true });
    }
    if (p === '/disappear' && req.method === 'POST') {
      const { jid, seconds } = await body();
      await sock.sendMessage(jid, { disappearingMessagesInChat: seconds > 0 ? seconds : false });
      return json(res, 200, { ok: true });
    }
    if (p === '/group/create' && req.method === 'POST') {
      const { subject, jids } = await body();
      const g = await sock.groupCreate(subject, jids);
      chats.set(g.id, { jid: g.id, name: subject, unread: 0, ts: Math.floor(Date.now() / 1000), last: '' }); dirtyMeta = true;
      return json(res, 200, { ok: true, jid: g.id });
    }
    if (p === '/deleteforme' && req.method === 'POST') {
      const { jid, id } = await body();
      const t = raws.get(jid)?.get(id); if (!t) return json(res, 404, { error: 'no msg' });
      await sock.chatModify({ deleteForMe: { deleteMedia: false, key: t.key, timestamp: num(t.messageTimestamp) } }, jid);
      raws.get(jid).delete(id); refreshLast(jid);
      return json(res, 200, { ok: true });
    }
    if (p.startsWith('/contact/')) {
      const jid = arg('/contact/'); const isGroup = jid.endsWith('@g.us');
      const out = { jid, name: nameOf(jid), phone: isGroup ? '' : '+' + jid.split('@')[0], blocked: blocked.has(jid), group: isGroup, about: '', desc: '', participants: [] };
      if (isGroup) { const md = await sock.groupMetadata(jid).catch(() => null); if (md) { out.desc = md.desc || ''; out.participants = md.participants.map((x) => ({ name: nameOf(x.id), admin: !!x.admin })); } }
      else { try { const s = await sock.fetchStatus(jid); const f = Array.isArray(s) ? s[0] : s; const a = f?.status?.status ?? f?.status ?? ''; out.about = typeof a === 'string' ? a : ''; } catch (_) {} }
      return json(res, 200, out);
    }
    if (p.startsWith('/list/')) {
      const jid = arg('/list/'); const kind = q.get('kind') || 'media';
      const all = [...(raws.get(jid)?.values() || [])].sort((a, b) => num(b.messageTimestamp) - num(a.messageTimestamp)).map(view);
      let out;
      if (kind === 'media') out = all.filter((v) => v.kind === 'image' || v.kind === 'video').slice(0, 60);
      else if (kind === 'docs') out = all.filter((v) => v.kind === 'doc').slice(0, 60);
      else { out = []; for (const v of all) for (const u of (v.text || '').match(/https?:\/\/\S+/g) || []) out.push({ id: v.id, kind: 'link', text: u, ts: v.ts }); out = out.slice(0, 60); }
      return json(res, 200, out);
    }
    if (p === '/updates') {
      const cutoff = Math.floor(Date.now() / 1000) - 86400; const by = new Map();
      for (const [id, m] of [...rawList('status@broadcast')]) if (num(m.messageTimestamp) < cutoff - 7200) rawList('status@broadcast').delete(id);
      for (const m of rawList('status@broadcast').values()) {
        if (m.key.fromMe) continue;
        const v = view(m); if (v.ts < cutoff) continue;
        const sender = m.key.participant || m.participant || '?'; if (!by.has(sender)) by.set(sender, []);
        if (v.kind === 'video') bgPrep('status@broadcast', v.id);
        by.get(sender).push({ id: v.id, kind: v.kind, text: v.text, ts: v.ts, w: v.w, h: v.h });
      }
      const out = [...by.entries()].map(([jid, items]) => ({ jid, name: nameOf(jid), items: items.sort((a, b) => a.ts - b.ts) })).sort((a, b) => b.items[b.items.length - 1].ts - a.items[a.items.length - 1].ts);
      return json(res, 200, out);
    }
    if (p === '/calls') { callsLoad(); return json(res, 200, calls.map((c) => ({ ...c, name: nameOf(c.jid) }))); }
    if (p === '/communities') {
      if (Date.now() - commCache.ts > 60000) {
        const withTimeout = (pr, ms) => Promise.race([pr, new Promise((_, rej) => setTimeout(() => rej(new Error('timeout')), ms))]);
        let comm = {}; try { comm = await withTimeout(sock.communityFetchAllParticipating(), 20000); } catch (e) { log('community: elenco non riuscito:', e.message); }
        const gs = Object.values(await withTimeout(sock.groupFetchAllParticipating(), 20000).catch(() => ({})));
        const parents = new Map(); for (const c of Object.values(comm || {})) if (c && c.id) parents.set(c.id, c); for (const g of gs) if (g.isCommunity) parents.set(g.id, g);
        for (const g of gs) if (g.linkedParent && !parents.has(g.linkedParent)) parents.set(g.linkedParent, { id: g.linkedParent, subject: nameOf(g.linkedParent) || 'Community', participants: [] });   // community di cui si vedono solo i gruppi
        const out = [];
        for (const c of parents.values()) {
          let groups = gs.filter((g) => g.linkedParent === c.id && !g.isCommunityAnnounce).map((g) => ({ id: g.id, subject: g.subject }));
          try { const lg = await withTimeout(sock.communityFetchLinkedGroups(c.id), 15000); for (const g of lg?.linkedGroups || []) if (!groups.some((x) => x.id === g.id)) groups.push({ id: g.id, subject: g.subject }); } catch (e) { /* si usano solo i gruppi gia' noti */ }
          out.push({ id: c.id, subject: c.subject || nameOf(c.id) || 'Community', size: c.size || (c.participants || []).length, groups });
        }
        log('community trovate:', out.length, '| gruppi totali:', gs.length, '| collegati a una community:', gs.filter((g) => g.linkedParent).length);
        commCache = { ts: Date.now(), data: out };
      }
      return json(res, 200, commCache.data);
    }
    if (p.startsWith('/export/')) {
      const jid = arg('/export/');
      const lines = [...(raws.get(jid)?.values() || [])].sort((a, b) => num(a.messageTimestamp) - num(b.messageTimestamp)).map((m) => {
        const v = view(m); const d = new Date(v.ts * 1000); const pad = (n) => String(n).padStart(2, '0');
        return `[${pad(d.getDate())}/${pad(d.getMonth() + 1)}/${d.getFullYear()}, ${pad(d.getHours())}:${pad(d.getMinutes())}] ${v.from_me ? 'Tu' : (v.name || nameOf(jid))}: ${v.kind === 'text' ? v.text : '<' + v.kind + '> ' + (v.text || '')}`;
      });
      res.writeHead(200, { 'Content-Type': 'text/plain; charset=utf-8' }); return res.end(lines.join('\n'));
    }

    if (p.startsWith('/community/') && req.method === 'POST') {   // gestione community (la libreria non ha l'eliminazione/disattivazione)
      const b = JSON.parse((await readBody(req, 1e5)).toString() || '{}'); const act = p.slice('/community/'.length);
      const wt = (pr) => Promise.race([pr, new Promise((_, rej) => setTimeout(() => rej(new Error('WhatsApp non risponde')), 30000))]);
      let out = { ok: true };
      if (act === 'create') { const r = await wt(sock.communityCreate(String(b.subject || '').slice(0, 100), String(b.description || '').slice(0, 2000))); out.id = r?.id; log('community creata:', r?.id, b.subject); }
      else if (act === 'creategroup') { const r = await wt(sock.communityCreateGroup(String(b.subject || '').slice(0, 100), [], b.id)); out.id = r?.id; log('gruppo creato nella community', b.id, b.subject); }
      else if (act === 'rename') { await wt(sock.communityUpdateSubject(b.id, String(b.subject || '').slice(0, 100))); }
      else if (act === 'description') { await wt(sock.communityUpdateDescription(b.id, String(b.description || '').slice(0, 2000))); }
      else if (act === 'leave') { await wt(sock.communityLeave(b.id)); }
      else return json(res, 404, { error: 'azione sconosciuta' });
      commCache = { ts: 0, data: [] };   // l'elenco si rilegge al prossimo accesso
      return json(res, 200, out);
    }
    if (p === '/logout' && req.method === 'POST') {
      json(res, 200, { ok: true });
      try { await sock.logout(); } catch (_) {}
      chats.clear(); raws.clear(); reacts.clear(); names.clear(); pnames.clear(); alias.clear(); blocked.clear(); calls.length = 0;
      try { fs.rmSync(STORE_DIR, { recursive: true, force: true }); fs.mkdirSync(STORE_DIR, { recursive: true }); dirtyChats.clear(); } catch (_) {}
      return;
    }

    // ---------- storie mie, pubblicazione, visualizzazioni ----------
    if (p === '/mystatus') {
      const cutoff = Math.floor(Date.now() / 1000) - 86400;
      const items = [...rawList('status@broadcast').values()].filter((m) => m.key.fromMe && num(m.messageTimestamp) >= cutoff).sort((a, b) => num(a.messageTimestamp) - num(b.messageTimestamp))
        .map((m) => { const v = view(m); return { id: v.id, kind: v.kind, text: v.text, ts: v.ts, w: v.w, h: v.h, views: statusViewers.get(v.id)?.size || 0 }; });
      return json(res, 200, items);
    }
    if (p.startsWith('/viewers/')) {
      const list = [...(statusViewers.get(arg('/viewers/'))?.entries() || [])].sort((a, b) => b[1] - a[1]).map(([jid, ts]) => ({ jid, name: nameOf(jid), ts }));
      return json(res, 200, list);
    }
    const statusRecipients = () => {
      const set = new Set([B.jidNormalizedUser(sock.user.id)]);
      for (const jid of chats.keys()) { let j = norm(jid); if (j.endsWith('@lid')) j = alias.get(j) || null; if (j && j.endsWith('@s.whatsapp.net')) set.add(j); }
      return [...set];
    };
    if (p === '/status/post' && req.method === 'POST') {
      const { text, bg, font } = await body();
      if (!text) return json(res, 400, { error: 'testo vuoto' });
      const sent = await sock.sendMessage('status@broadcast', { text }, { statusJidList: statusRecipients(), backgroundColor: bg || '#128C7E', font: font || 1 });
      addMessage(sent, false); return json(res, 200, { ok: true });
    }
    if (p === '/status/postmedia' && req.method === 'POST') {
      let buf = await readBody(req); const caption = q.get('caption') || undefined;
      buf = await sharp(buf).rotate().resize(1280, 1280, { fit: 'inside', withoutEnlargement: true }).jpeg({ quality: 82 }).toBuffer();
      const sent = await sock.sendMessage('status@broadcast', { image: buf, caption }, { statusJidList: statusRecipients() });
      addMessage(sent, false); return json(res, 200, { ok: true });
    }
    if (p === '/status/view' && req.method === 'POST') {          // conferma di visualizzazione (come il client ufficiale)
      const { id } = await body(); const t = rawList('status@broadcast').get(id);
      if (t && !t.key.fromMe) await sock.readMessages([t.key]);
      return json(res, 200, { ok: true });
    }
    // ---------- canali (newsletter) ----------
    if (p === '/channels') {
      const out = [...chats.values()].filter((c) => c.jid.endsWith('@newsletter')).sort((a, b) => (b.ts || 0) - (a.ts || 0)).map((c) => { const m = chMetaFor(c.jid); return { jid: c.jid, name: m?.name || (c.name && !/^\d+$/.test(c.name) ? c.name : 'Canale'), last: c.last || '', ts: c.ts || 0, unread: c.unread || 0, subscribers: m?.subscribers || 0 }; });
      return json(res, 200, out);
    }
    if (p === '/channel/follow' && req.method === 'POST') {
      const { link } = await body(); const code = ((link || '').match(/channel\/([A-Za-z0-9_-]+)/) || [])[1] || (link || '').trim();
      if (!code) return json(res, 400, { error: 'link non valido' });
      const meta = await sock.newsletterMetadata('invite', code); if (!meta?.id) return json(res, 404, { error: 'canale non trovato' });
      await sock.newsletterFollow(meta.id);
      chats.set(meta.id, { jid: meta.id, name: meta.name || 'Canale', unread: 0, ts: Math.floor(Date.now() / 1000), last: '' }); chMeta.set(meta.id, { name: meta.name || '', subscribers: meta.subscribers || 0 }); dirtyMeta = true;
      return json(res, 200, { ok: true, jid: meta.id, name: meta.name });
    }
    if (p === '/channel/unfollow' && req.method === 'POST') {
      const { jid } = await body(); await sock.newsletterUnfollow(jid); chats.delete(jid); raws.delete(jid); chCache.delete(jid); dirtyMeta = true; dirtyChats.add(jid);
      return json(res, 200, { ok: true });
    }

    if (p === '/profile' && req.method === 'GET') {
      const me = B.jidNormalizedUser(sock.user.id);
      let about = '';
      try { const s = await sock.fetchStatus(me); const f = Array.isArray(s) ? s[0] : s; about = f?.status?.status ?? f?.status ?? ''; if (typeof about !== 'string') about = ''; } catch (_) {}
      return json(res, 200, { jid: me, name: sock.user.name || sock.authState.creds.me?.name || '', about, phone: me.split('@')[0] });
    }
    if (p === '/profile' && req.method === 'POST') {
      const { name, about } = JSON.parse((await readBody(req, 1e5)).toString());
      if (name) await sock.updateProfileName(name);
      if (about != null) await sock.updateProfileStatus(about);
      return json(res, 200, { ok: true });
    }
    if (p === '/profile/photo' && req.method === 'POST') {
      const buf = await sharp(await readBody(req, 20e6)).rotate().resize(640, 640, { fit: 'cover' }).jpeg({ quality: 85 }).toBuffer();
      await sock.updateProfilePicture(B.jidNormalizedUser(sock.user.id), buf);
      return json(res, 200, { ok: true });
    }
    if (p === '/events') {
      const since = parseInt(q.get('since') || '0', 10);
      return json(res, 200, { seq: evSeq, events: events.filter((e) => e.seq > since) });
    }
    json(res, 404, { error: 'not found' });
  } catch (e) { log('errore API', req.method, req.url, e && e.stack || e); json(res, 500, { error: String(e.message || e) }); }
}).listen(PORT, '0.0.0.0', () => console.log('Bridge su porta ' + PORT));

start().catch(console.error);

// Canale per il plugin delle notifiche. La Vita rompe la libreria delle notifiche se il processo che le mostra (la shell)
// legge dalla rete piu' di ~256 byte in una transazione: qui OGNI risposta e' di al massimo 16 byte.
//   "P <seq>\n"            -> "S<seqUltimo>,<quanti>\n"      (quanti eventi nuovi, max 5)
//   "G <seq> <idx> <off>\n" -> 16 byte del testo "titolo: messaggio" dell'evento idx, a partire da off (meno di 16 = fine)
const RAW_PORT = PORT + 1;
let lastNotif = null;   // ultimo messaggio mostrato dal plugin: l'app, aprendosi, porta l'utente in quella chat
function rawText(e) {
  let b = Buffer.from(`${e.title || 'WhatsApp'}: ${e.text || ''}`.replace(/[\r\n]+/g, ' '), 'utf8');
  if (b.length > 90) { let n = 90; while (n > 0 && (b[n] & 0xC0) === 0x80) n--; b = b.subarray(0, n); }
  return b;
}
require('net').createServer((sock) => {
  let buf = ''; let done = false;
  sock.setTimeout(4000, () => sock.destroy());
  sock.on('error', () => {});
  sock.on('data', (d) => {
    if (done) return; buf += d.toString('latin1'); const i = buf.indexOf('\n'); if (i < 0) { if (buf.length > 64) sock.destroy(); return; }
    done = true; const p = buf.slice(0, i).trim().split(/\s+/);
    lastPoll.ip = (sock.remoteAddress || '').replace('::ffff:', ''); lastPoll.t = Date.now(); lastPoll.n++;
    if (p[0] === 'P') {
      const since = parseInt(p[1], 10) || 0;
      const n = Math.min(5, events.filter((e) => e.seq > since).length);
      return sock.end(`S${evSeq},${n}\n`);
    }
    if (p[0] === 'G') {
      const since = parseInt(p[1], 10) || 0, idx = parseInt(p[2], 10) || 0, off = parseInt(p[3], 10) || 0;
      const e = events.filter((x) => x.seq > since)[idx];
      if (e && off === 0) lastNotif = { jid: e.jid, ts: Date.now(), taken: false };
      return sock.end(e ? rawText(e).subarray(off, off + 16) : Buffer.alloc(0));
    }
    sock.end();
  });
}).listen(RAW_PORT, '0.0.0.0', () => console.log('Canale plugin su porta ' + RAW_PORT));

// Dopo l'avvio si preparano le ultime foto delle chat piu' recenti, cosi' la prima apertura sulla Vita non deve aspettare.
setTimeout(() => {
  if (ENGINE) return;   // dentro la Vita le foto si scaricano solo quando servono
  try { [...chats.values()].filter((c) => !c.archived).sort((a, b) => (b.ts || 0) - (a.ts || 0)).slice(0, 25).forEach((c) => prefetchChat(c.jid, 6)); log('precaricamento foto delle chat recenti avviato'); } catch (e) { log('precaricamento foto:', e.message); }
}, 60000);
