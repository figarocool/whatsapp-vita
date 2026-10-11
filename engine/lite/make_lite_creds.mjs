// Prepara il file di credenziali per il client leggero a partire dalla cartella auth del motore.
// Uso: node make_lite_creds.mjs <cartella-auth> <uscita.bin>
import fs from 'fs';
import { createRequire } from 'module';
const BR = new URL('../../bridge/node_modules/', import.meta.url).pathname;
const req = createRequire(BR + 'x.js');
const B = await import(BR + '@whiskeysockets/baileys/lib/index.js');
const { BufferJSON, generateLoginNode, DEFAULT_CONNECTION_CONFIG } = B;
const [, , authDir, out] = process.argv;
const creds = JSON.parse(fs.readFileSync(authDir + '/creds.json', 'utf8'), BufferJSON.reviver);
const cfg = { ...DEFAULT_CONNECTION_CONFIG, browser: ['PS Vita', 'Chrome', '1.0'], syncFullHistory: false, countryCode: 'US' };
const node = generateLoginNode(creds.me.id, cfg);
const payload = B.proto.ClientPayload.encode(node).finish();
const priv = Buffer.from(creds.noiseKey.private), pub = Buffer.from(creds.noiseKey.public);
const head = Buffer.alloc(4 + 64 + 2); head.write('WALT', 0); priv.copy(head, 4); pub.copy(head, 36); head.writeUInt16LE(payload.length, 68);
fs.writeFileSync(out, Buffer.concat([head, Buffer.from(payload)]));
console.log('credenziali scritte:', out, '- utente', creds.me.id, '- payload', payload.length, 'byte, versione', cfg.version.join('.'));
