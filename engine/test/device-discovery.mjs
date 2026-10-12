import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import { patchDeviceDiscovery } from '../js/patches/messages-send.js';
import { jidDecode, jidEncode, jidNormalizedUser, isLidUser, isHostedLidUser } from '../../bridge/node_modules/@whiskeysockets/baileys/lib/WABinary/jid-utils.js';
const source = fs.readFileSync(new URL('../../bridge/node_modules/@whiskeysockets/baileys/lib/Socket/messages-send.js', import.meta.url), 'utf8');
function discovery(code, response, cached) {
  const start = code.indexOf('const getUSyncDevices =');
  const end = code.indexOf('return deviceResults;\n    };', start) + 'return deviceResults;\n    };'.length;
  class Query { withContext() { return this; } withDeviceProtocol() { return this; } withLIDProtocol() { return this; } withUser() { return this; } }
  class User { withId() { return this; } }
  const context = { jidDecode, jidEncode, jidNormalizedUser, isLidUser, isHostedLidUser,
    logger: {debug() {}, trace() {}, warn() {}}, USyncQuery: Query, USyncUser: User,
    userDevicesCache: {mget: async () => cached || {}, mset: async () => {}},
    sock: {executeUSyncQuery: async () => response},
    authState: {creds: {me: {id: '123:1@s.whatsapp.net', lid: '456:1@lid'}}, keys: {set: async () => {}}},
    devicesMutex: {mutex: async fn => fn()}, extractDeviceJids: list => list };
  return vm.runInNewContext(code.slice(start, end) + '\ngetUSyncDevices', context);
}
// Riproduce il falso successo della versione originale: timeout -> lista vuota.
assert.equal((await discovery(source, undefined)(['789@lid'], true, false)).length, 0);
const patched = patchDeviceDiscovery(source);
await assert.rejects(discovery(patched, undefined)(['789@lid'], true, false), /messaggio non inviato/);
const device = {user:'789', device:2, server:'lid'};
assert.equal((await discovery(patched, {list:[device]})(['789@lid'], true, false))[0].jid, '789:2@lid');
assert.equal((await discovery(patched, undefined, {'789':[device]})(['789@lid'], true, false))[0].jid, '789:2@lid');
assert.throws(() => patchDeviceDiscovery(''), /incompatibile/);
console.log('OK: timeout rifiutato, dispositivi USync e cache conservati');
