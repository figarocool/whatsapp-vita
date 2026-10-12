import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
const source = fs.readFileSync(new URL('../../bridge/server.js', import.meta.url), 'utf8');
const helpers = source.slice(source.indexOf('function mergeChat('), source.indexOf('// ---------- utilità messaggi'));
let timers = [];
const connection = { async groupFetchAllParticipating() {
  return { one: { id: 'one@g.us', subject: 'Group One' }, two: { id: 'two@g.us', subject: 'Group Two' } };
} };
const context = {
  chats: new Map([['one@g.us', { jid: 'one@g.us', name: '1234', unread: 4, ts: 12, pinned: 1, archived: true, last: 'preview' }]]),
  names: new Map(), dirtyMeta: false, lastMetaSave: 123,
  num: Number, nameFor(id) { return context.names.get(id); },
  log() {}, sock: connection, state: { status: 'open' },
  setTimeout(fn) { timers.push(fn); }
};
vm.createContext(context); vm.runInContext(helpers, context);
await context.recoverGroups(connection);
assert.equal(context.chats.size, 2);
assert.equal(context.names.get('one@g.us'), 'Group One');
assert.equal(context.chats.get('two@g.us').name, 'Group Two');
assert.equal(context.chats.get('one@g.us').unread, 4);
assert.equal(context.chats.get('one@g.us').last, 'preview');
assert.equal(context.chats.get('one@g.us').archived, true);
context.mergeChat({ id: 'one@g.us', conversationTimestamp: 3 });
assert.equal(context.chats.get('one@g.us').name, 'Group One');
assert.equal(context.chats.get('one@g.us').ts, 12);
context.mergeGroup({ id: 'one@g.us', subject: 'Renamed Group' });
assert.equal(context.chats.get('one@g.us').name, 'Renamed Group');
assert.equal(context.dirtyMeta, true); assert.equal(context.lastMetaSave, 0);
/* A failed request retries twice; disconnected/replaced sessions do not write. */
let failures = 0;
const failing = { async groupFetchAllParticipating() { failures++; throw Error('offline'); } };
context.sock = failing; await context.recoverGroups(failing);
while (timers.length) await timers.shift()();
assert.equal(failures, 3);
await context.recoverGroups(connection); assert.equal(failures, 3);
let resolve;
const stale = { groupFetchAllParticipating() { return new Promise(r => { resolve = r; }); } };
context.sock = stale; const pending = context.recoverGroups(stale);
context.sock = connection; resolve({ stale: { id: 'stale@g.us', subject: 'wrong session' } });
await pending; assert.equal(context.chats.has('stale@g.us'), false);
/* Recover chat files omitted from meta.json without creating a status chat. */
Object.assign(context, {
  Buffer, STORE_OLD: '/old', META: '/meta', STORE_DIR: '/store', B: { BufferJSON: {} },
  lazyChats: new Set(), raws: new Map(), reacts: new Map(), norm: x => x,
  pnames: new Map(), alias: new Map(), statusViewers: new Map(),
  chatFile: id => id,
  fs: {
    existsSync: p => p !== '/old',
    readFileSync(p) { return JSON.stringify(p === '/meta' ? { chats: [] } : { msgs: [{ key: { id: 'm', remoteJid: p }, messageTimestamp: 42 }] }); },
    readdirSync() { return ['missing@s.whatsapp.net', 'one@g.us', 'status@broadcast'].map(jid => 'c_' + Buffer.from(jid).toString('base64url') + '.json'); }
  },
  describe: () => ({ kind: 'text', text: 'stored preview' }), preview: d => d.text
});
vm.runInContext(source.slice(source.indexOf('function loadChatFile('), source.indexOf('function loadAllLazy(')), context);
vm.runInContext(source.slice(source.indexOf('function loadStore('), source.indexOf('setInterval(() => saveStore')), context);
context.loadStore();
assert(context.chats.has('missing@s.whatsapp.net'));
assert.equal(context.chats.get('missing@s.whatsapp.net').last, 'stored preview');
assert.equal(context.chats.get('missing@s.whatsapp.net').ts, 42);
assert(!context.chats.has('status@broadcast'));
assert(context.lazyChats.has('one@g.us'), 'known chats remain lazy');
/* A metadata write failure must leave dirtyMeta set for the next save. */
context.fs.writeFileSync = () => { throw Error('disk full'); };
context.writeAtomic = (p, s) => context.fs.writeFileSync(p, s);
context.dirtyChats = new Set(); context.ENGINE = true; context.dirtyMeta = true;
vm.runInContext(source.slice(source.indexOf('function saveStore('), source.indexOf('function loadChatFile(')), context);
context.saveStore(true); assert.equal(context.dirtyMeta, true);
console.log('OK: group recovery, names, metadata merge, retries, stale sessions, orphan chat files, failed saves');
