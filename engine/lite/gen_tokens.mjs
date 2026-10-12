// Genera lite/wabin_tokens.h dalle tabelle dei token di Baileys (la fonte di verita' del protocollo).
import { SINGLE_BYTE_TOKENS, DOUBLE_BYTE_TOKENS } from '../../bridge/node_modules/@whiskeysockets/baileys/lib/WABinary/constants.js';
import fs from 'fs';
// Un token del dizionario del protocollo ha la forma di una chiave API di Google (e' pubblico, nel client ufficiale e in Baileys), ma qui non serve mai il suo testo: solo la posizione.
// Si scrive un segnaposto, cosi' i controlli automatici sui segreti non lo scambiano per una chiave nostra.
const mask = (t) => (/^AIza[0-9A-Za-z_-]{30,}$/.test(String(t)) ? '(chiave-pubblica-del-protocollo)' : t);
const q = (s) => '"' + String(mask(s)).replace(/\\/g, '\\\\').replace(/"/g, '\\"') + '"';
let o = '// Generato da gen_tokens.mjs: non modificare a mano.\n#pragma once\n';
o += `#define WB_SINGLE_N ${SINGLE_BYTE_TOKENS.length}\nstatic const char *const WB_SINGLE[WB_SINGLE_N] = {\n${SINGLE_BYTE_TOKENS.map((t) => '  ' + q(t || '')).join(',\n')}\n};\n`;
o += `#define WB_DICTS ${DOUBLE_BYTE_TOKENS.length}\nstatic const int WB_DICT_N[WB_DICTS] = { ${DOUBLE_BYTE_TOKENS.map((d) => d.length).join(', ')} };\n`;
DOUBLE_BYTE_TOKENS.forEach((d, i) => { o += `static const char *const WB_DICT${i}[${d.length}] = {\n${d.map((t) => '  ' + q(t)).join(',\n')}\n};\n`; });
o += `static const char *const *const WB_DICT[WB_DICTS] = { ${DOUBLE_BYTE_TOKENS.map((_, i) => 'WB_DICT' + i).join(', ')} };\n`;
fs.writeFileSync(new URL('./wabin_tokens.h', import.meta.url), o);
console.log('single', SINGLE_BYTE_TOKENS.length, 'dict', DOUBLE_BYTE_TOKENS.map((d) => d.length));
