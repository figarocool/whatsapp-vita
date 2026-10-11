// Bridge finto per provare l'app (anche in emulatore) con una sincronizzazione simulata e molti dati.
// Uso: NODE_PATH=bridge/node_modules node tools/mock_bridge.js [porta]
const http = require('http'); const sharp = require('sharp');
let prepT0 = 0;
const PORT = parseInt(process.argv[2] || '8788', 10); const T0 = Date.now();
const SYNC_MS = 40000, TOTAL = 400;
const emojis = ['\u{1F600}', '\u{1F602}', '\u{2764}\u{FE0F}', '\u{1F44D}', '\u{1F64F}', '\u{1F389}', '\u{1F525}', '\u{1F60D}'];
const nameFor = (i) => isCh(i) ? `Canale Notizie ${i} \u{1F4F0}` : i % 9 === 0 ? `Utente ...${1000 + i}` : i % 7 === 0 ? `Gruppo ${emojis[i % 8]} Famiglia ${i}` : i % 11 === 0 ? `محمد ${i}` : i % 13 === 0 ? `山田 太郎 ${i}` : `Contatto ${i} ${'Cognome'.repeat(1 + (i % 3))}`;
const isCh = (i) => i % 19 === 5;
const jidFor = (i) => isCh(i) ? `1203${String(i).padStart(10, '0')}@newsletter` : i % 7 === 0 ? `1203634${String(i).padStart(8, '0')}@g.us` : i % 2 ? `${130000000000 + i}@lid` : `39333${String(1000000 + i)}@s.whatsapp.net`;
const rnd = (seed) => { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; };
const progress = () => Math.min(100, Math.round(((Date.now() - T0 - 3000) / SYNC_MS) * 100));
const grown = () => Math.max(0, Math.min(TOTAL, Math.round(TOTAL * Math.max(0, progress()) / 100)));
const longText = (r) => Array.from({ length: 60 + Math.floor(r() * 250) }, () => 'parola' + Math.floor(r() * 99)).join(' ');
function messages(jid, n) {
  const seed = [...jid].reduce((a, c) => a + c.charCodeAt(0), 7); const r = rnd(seed); const out = []; const now = Math.floor(Date.now() / 1000);
  for (let k = 0; k < n; k++) {
    const x = r(); const m = { id: `M${seed}_${k}`, from_me: r() < 0.4, ts: now - (n - k) * 600, name: jid.endsWith('@g.us') ? 'Persona ' + Math.floor(r() * 6) : '', status: 4, edited: r() < 0.05, quoted: r() < 0.1 ? { text: 'citazione di prova', name: 'Mario' } : null, reactions: r() < 0.1 ? [{ e: '\u{1F44D}', n: 2 }] : [], kind: 'text', text: '' };
    if (x < 0.45) m.text = 'Ciao ' + emojis[k % 8] + ' messaggio numero ' + k;
    else if (x < 0.55) m.text = longText(r);
    else if (x < 0.62) m.text = 'Guarda https://esempio.it/' + 'x'.repeat(380 + Math.floor(r() * 400)) + ' ok';
    else if (x < 0.78) { m.kind = 'image'; m.w = 800 + Math.floor(r() * 800); m.h = 600 + Math.floor(r() * 900); m.text = r() < 0.5 ? 'foto ' + k : ''; }
    else if (x < 0.84) { m.kind = 'sticker'; m.w = 512; m.h = 512; }
    else if (x < 0.9) { m.kind = 'doc'; m.fname = 'documento_con_nome_molto_lungo_' + k + '.pdf'; m.size = 123456; }
    else if (x < 0.95) { m.kind = 'voice'; m.dur = 5 + Math.floor(r() * 90); }
    else if (x < 0.98) { m.kind = 'deleted'; }
    else m.text = '\u{1F600}\u{1F602}\u{2764}\u{FE0F}\u{1F44D}\u{1F64F}'.repeat(40);
    out.push(m);
  }
  return out;
}
const send = (res, code, obj, type = 'application/json') => { res.writeHead(code, { 'Content-Type': type }); res.end(type === 'application/json' ? JSON.stringify(obj) : obj); };
http.createServer(async (req, res) => {
  const u = new URL(req.url, 'http://x'); const p = decodeURIComponent(u.pathname);
  if (p === '/status') { const pr = progress(); return send(res, 200, pr < 0 ? { status: 'reconnecting' } : { status: 'open', me: '39000@s.whatsapp.net', qr: null, sync: { active: pr < 100, progress: Math.max(0, pr), chats: grown() } }); }
  if (p === '/chats') { const lim = Math.min(400, parseInt(u.searchParams.get('limit') || '400', 10)); const n = Math.min(lim, grown()); const now = Math.floor(Date.now() / 1000);
    const sh = process.env.MOCK_SHIFT ? Math.max(0, Math.floor((Date.now() - T0 - 55000) / 8000)) : 0;   // prova del difetto 'chat sbagliata': dopo 55 s l'ordine dell'elenco cambia ogni 8 s
    return send(res, 200, Array.from({ length: n }, (_, k) => { const i = (k + sh) % Math.max(1, n); return ({ jid: jidFor(i), name: nameFor(i), last: i % 3 ? 'ultimo messaggio ' + emojis[i % 8] + ' di prova\ncon a capo' : '\u{1F4F7} Foto', ts: now - i * 900, unread: i % 5 === 0 ? i % 17 : 0, channel: isCh(i), archived: i % 41 === 0 && !isCh(i), muted: i % 23 === 0 ? 1 : 0, blocked: false }); })); }
  if (p.startsWith('/messages/')) return send(res, 200, messages(p.slice(10), Math.min(300, parseInt(u.searchParams.get('n') || '30', 10))));
  if (p.startsWith('/media/')) { const r = rnd(p.length * 31); const w = 320 + Math.floor(r() * 160), h = 240 + Math.floor(r() * 240);
    const buf = await sharp({ create: { width: w, height: h, channels: 3, background: { r: Math.floor(r() * 255), g: Math.floor(r() * 255), b: Math.floor(r() * 255) } } }).jpeg({ quality: 70 }).toBuffer(); return send(res, 200, buf, 'image/jpeg'); }
  if (p.startsWith('/avatar/')) { if ([...p].reduce((a, c) => a + c.charCodeAt(0), 0) % 3 === 0) return send(res, 404, { error: 'no avatar' });
    const buf = await sharp({ create: { width: 96, height: 96, channels: 3, background: { r: 60, g: 140, b: 200 } } }).jpeg().toBuffer(); return send(res, 200, buf, 'image/jpeg'); }
  if (p.startsWith('/presence/')) return send(res, 200, { state: 'composing' });
  if (p === '/profile') return send(res, 200, { jid: '39000@s.whatsapp.net', name: 'Io Stesso', about: 'Info di prova', phone: '393331234567' });
  if (p === '/updates') { const now = Math.floor(Date.now() / 1000); return send(res, 200, [
    { jid: '39333111@s.whatsapp.net', name: 'Anna Rossi', items: [{ id: 'S1', kind: 'text', text: 'Buongiorno a tutti \u{1F31E}', ts: now - 3600 }, { id: 'S2', kind: 'image', text: 'La mia colazione', ts: now - 3000, w: 800, h: 600 }] },
    { jid: '39333222@s.whatsapp.net', name: 'Luca Bianchi con un nome molto lungo da troncare', items: [{ id: 'S3', kind: 'text', text: 'Auguri!', ts: now - 7200 }] },
    { jid: '39333333@s.whatsapp.net', name: 'Maria Verdi', items: [{ id: 'S4', kind: 'image', text: '', ts: now - 9000, w: 600, h: 800 }] }]); }
  if (p === '/mystatus') { const now = Math.floor(Date.now() / 1000); return send(res, 200, [{ id: 'M1', kind: 'text', text: 'La mia prima storia', ts: now - 5000, views: 3 }, { id: 'M2', kind: 'image', text: 'foto', ts: now - 2000, w: 800, h: 600, views: 1 }]); }
  if (p.startsWith('/viewers/')) { const now = Math.floor(Date.now() / 1000); return send(res, 200, [{ jid: 'a', name: 'Anna Rossi', ts: now - 600 }, { jid: 'b', name: 'Luca Bianchi', ts: now - 1200 }, { jid: 'c', name: 'Maria Verdi', ts: now - 4000 }]); }
  if (p === '/calls' || p === '/communities') return send(res, 200, []);
  if (p.startsWith('/prep/')) { prepT0 = Date.now(); return send(res, 200, { key: 'mockkey' }); }
  if (p.startsWith('/prep-status/')) { const t = (Date.now() - prepT0) / 1000; return send(res, 200, t < 3 ? { state: 'download', pct: 0 } : t < 9 ? { state: 'convert', pct: Math.min(99, Math.round((t - 3) / 6 * 100)) } : { state: 'ready', pct: 100, size: 3000000 }); }
  if (p.startsWith('/prep-file/')) { res.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': 3000000 }); let sent = 0; const iv = setInterval(() => { const n = Math.min(100000, 3000000 - sent); res.write(Buffer.alloc(n, 7)); sent += n; if (sent >= 3000000) { clearInterval(iv); res.end(); } }, 150); return; }
  if (p.startsWith('/read/') || p === '/events') return send(res, 200, { ok: true, seq: 0, events: [] });
  if (req.method === 'POST') { req.resume(); return send(res, 200, { ok: true }); }
  send(res, 404, { error: 'mock: non gestito ' + p });
}).listen(PORT, '127.0.0.1', () => console.log('bridge finto su', PORT, '- sync simulata di', SYNC_MS / 1000, 's, chat totali', TOTAL));
