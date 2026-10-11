import './prelude.js';
import { useMultiFileAuthState } from '@whiskeysockets/baileys';
const N = globalThis.__nat;
(async () => {
  const dir = '/tmp/authtest/auth'; N.fsMkdir(dir);
  let t0 = N.mono(); const a = await useMultiFileAuthState(dir);
  const batch = {}; for (let i = 1; i <= 812; i++) batch[i] = { private: Buffer.alloc(32, i & 255), public: Buffer.alloc(32, (i * 3) & 255) };
  await a.state.keys.set({ 'pre-key': batch, session: { 'abc.0': Buffer.from('sessione') }, 'sender-key': { 'g@x::1': Buffer.from('chiave') } });
  console.log('812 chiavi impostate in', (N.mono() - t0).toFixed(1), 'ms');
  await a.saveCreds(); setTimeout(async () => {
    const files = N.fsList(dir); console.log('file dopo il salvataggio:', files.length, files.slice(0, 5).join(','));
    const b = await useMultiFileAuthState(dir); const g = await b.state.keys.get('pre-key', ['1', '500', '812', '9999']);
    console.log('rilettura:', Buffer.from(g['500'].private)[0] === 500 % 256, g['812'] ? 'ok812' : 'manca812', g['9999'] === null ? 'inesistente=null' : 'ERRORE');
    const s = await b.state.keys.get('session', ['abc.0']); console.log('sessione:', Buffer.from(s['abc.0']).toString());
    await b.state.keys.set({ 'pre-key': { 500: null } }); globalThis.__flushAuth(); const c = await (await useMultiFileAuthState(dir)).state.keys.get('pre-key', ['500', '501']); console.log('cancellazione:', c['500'] === null, c['501'] !== null);
  }, 2500);
})().catch((e) => console.log('ERRORE', e && e.stack));
