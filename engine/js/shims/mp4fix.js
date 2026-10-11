// Ritocchi al contenitore MP4 senza ricodificare, per il decodificatore hardware della Vita:
//  - livello H.264 troppo alto (es. 5.1) -> 4.1
//  - dati VUI nell'SPS (colori, limiti del buffer...) che la Vita rifiuta -> tolti, il flusso video resta identico
const CONT = new Set(['moov', 'trak', 'mdia', 'minf', 'stbl', 'edts', 'dinf']);
const u32 = (b, o) => b.readUInt32BE(o);
function unescapeRbsp(b) { const o = []; for (let i = 0; i < b.length; i++) { if (i >= 2 && b[i] === 3 && b[i - 1] === 0 && b[i - 2] === 0) continue; o.push(b[i]); } return Buffer.from(o); }
function escapeRbsp(b) { const o = []; let z = 0; for (const x of b) { if (z >= 2 && x <= 3) { o.push(3); z = 0; } o.push(x); z = x === 0 ? z + 1 : 0; } return Buffer.from(o); }
class BR { constructor(b) { this.b = b; this.p = 0; } bit() { const v = (this.b[this.p >> 3] >> (7 - (this.p & 7))) & 1; this.p++; return v; } bits(n) { let v = 0; while (n--) v = v * 2 + this.bit(); return v; } ue() { let z = 0; while (this.bit() === 0 && z < 32) z++; return z ? (2 ** z) - 1 + this.bits(z) : 0; } se() { const k = this.ue(); return k & 1 ? (k + 1) / 2 : -k / 2; } }
// rbsp senza il byte di intestazione NAL; ritorna le posizioni dei bit frame_cropping_flag e vui_parameters_present_flag
function vuiFlagPos(rb) {
  const r = new BR(rb); const prof = r.bits(8); r.bits(16); r.ue();
  if ([100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135].includes(prof)) {
    const cf = r.ue(); if (cf === 3) r.bit(); r.ue(); r.ue(); r.bit();
    if (r.bit()) { const n = cf !== 3 ? 8 : 12; for (let i = 0; i < n; i++) if (r.bit()) { let last = 8, next = 8; const sz = i < 6 ? 16 : 64; for (let j = 0; j < sz; j++) { if (next !== 0) { next = (last + r.se() + 256) % 256; } last = next === 0 ? last : next; } } }
  }
  r.ue(); const poc = r.ue();
  if (poc === 0) r.ue(); else if (poc === 1) { r.bit(); r.se(); r.se(); const n = r.ue(); for (let i = 0; i < n; i++) r.se(); }
  r.ue(); r.bit(); r.ue(); r.ue(); const fm = r.bit(); if (!fm) r.bit(); r.bit();
  const cropPos = r.p; const crop = r.bit();
  if (crop) { r.ue(); r.ue(); r.ue(); r.ue(); }
  return { cropPos, crop, vuiPos: r.p };
}
function stripVui(sps) {   // sps = NAL completo (con intestazione); toglie ritaglio e VUI. Ritorna il nuovo NAL o null se non c'e' niente da togliere
  const rb = unescapeRbsp(sps.subarray(1)); let P; try { P = vuiFlagPos(rb); } catch (_) { return null; }
  const bitAt = (p) => (rb[p >> 3] >> (7 - (p & 7))) & 1;
  if (P.vuiPos >= rb.length * 8) return null;
  const hasVui = bitAt(P.vuiPos); if (!hasVui && !P.crop) return null;
  // nuovo flusso di bit: tutto fino a frame_cropping_flag, poi crop=0, vui=0, bit di chiusura
  const bits = []; for (let p = 0; p < P.cropPos; p++) bits.push(bitAt(p)); bits.push(0, 0, 1);
  while (bits.length & 7) bits.push(0);
  const out = Buffer.alloc(bits.length >> 3); for (let i = 0; i < bits.length; i++) if (bits[i]) out[i >> 3] |= 1 << (7 - (i & 7));
  return Buffer.concat([sps.subarray(0, 1), escapeRbsp(out)]);
}
function findBox(b, start, end, type) { for (let o = start; o + 8 <= end;) { let s = u32(b, o); const t = b.toString('latin1', o + 4, o + 8); if (s === 1) s = Number(b.readBigUInt64BE(o + 8)); if (s < 8) return null; if (t === type) return { o, s }; o += s; } return null; }
export function fixMp4(b, maxLevel = 41) {   // ritorna { buf, notes } oppure null se non c'e' niente da fare
  const notes = []; let buf = b;
  const moov = findBox(buf, 0, buf.length, 'moov'); if (!moov || findBox(buf, 0, buf.length, 'moof')) return null;
  const mdat = findBox(buf, 0, buf.length, 'mdat');
  // percorso trak -> ... -> stsd -> avc1 -> avcC
  const chain = []; let found = null;
  (function walk(o, end, path) {
    for (let p = o; p + 8 <= end && !found;) { const s = u32(buf, p), t = buf.toString('latin1', p + 4, p + 8); if (s < 8) return;
      if (t === 'avcC') { found = { p, s, path: path.slice() }; return; }
      if (CONT.has(t)) walk(p + 8, p + s, [...path, p]);
      else if (t === 'stsd') walk(p + 16, p + s, [...path, p]);
      else if (t === 'avc1' || t === 'avc3') walk(p + 86, p + s, [...path, p]);
      p += s; }
  })(moov.o + 8, moov.o + moov.s, [moov.o]);
  if (!found) return null;
  const a = found.p; let changed = false;
  if (buf[a + 11] > maxLevel) { buf = Buffer.from(buf); buf[a + 11] = maxLevel; changed = true; notes.push('livello ' + b[a + 11] + ' -> ' + maxLevel); }
  const nsps = buf[a + 13] & 0x1f; let delta = 0;
  if (nsps === 1) {
    const sl = buf.readUInt16BE(a + 14), sp = a + 16; const spsBuf = buf.subarray(sp, sp + sl);
    if (spsBuf[3] > maxLevel) { buf = Buffer.from(buf); buf[sp + 3] = maxLevel; spsBuf[3] = maxLevel; }
    const nn = stripVui(Buffer.from(spsBuf));
    if (nn && nn.length <= sl) {
      delta = sl - nn.length; notes.push('ritaglio/VUI tolti dall\'SPS (-' + delta + ' byte)');
      const nb = Buffer.concat([buf.subarray(0, sp), nn, buf.subarray(sp + sl)]); nb.writeUInt16BE(nn.length, a + 14);
      for (const q of found.path.concat([a])) nb.writeUInt32BE(u32(nb, q) - delta, q);   // le dimensioni dei contenitori
      buf = nb; changed = true;
      if (mdat && mdat.o > moov.o) {   // i dati stanno dopo l'indice: gli offset dei blocchi scalano
        (function fix(o, end) { for (let p = o; p + 8 <= end;) { const s = u32(buf, p), t = buf.toString('latin1', p + 4, p + 8); if (s < 8) return;
          if (CONT.has(t)) fix(p + 8, p + s);
          else if (t === 'stco') { const n = u32(buf, p + 12); for (let i = 0; i < n; i++) buf.writeUInt32BE(u32(buf, p + 16 + i * 4) - delta, p + 16 + i * 4); }
          else if (t === 'co64') { const n = u32(buf, p + 12); for (let i = 0; i < n; i++) buf.writeBigUInt64BE(buf.readBigUInt64BE(p + 16 + i * 8) - BigInt(delta), p + 16 + i * 8); }
          p += s; } })(moov.o + 8, moov.o + u32(buf, moov.o));
      }
    }
  }
  // dimensioni del contenitore portate a multipli di 16 (come il flusso, ora senza ritaglio); le misure vere restano in coda al file, in un riquadro 'free'
  if (changed && found.path.length >= 7 && /^avc[13]$/.test(buf.toString('latin1', found.path[6] + 4, found.path[6] + 8))) {
    const av = found.path[6], tw = buf.readUInt16BE(av + 32), th = buf.readUInt16BE(av + 34), aw = (tw + 15) & ~15, ah = (th + 15) & ~15;
    if (aw !== tw || ah !== th) {
      buf = Buffer.from(buf); buf.writeUInt16BE(aw, av + 32); buf.writeUInt16BE(ah, av + 34);
      const tk = findBox(buf, found.path[1] + 8, found.path[1] + u32(buf, found.path[1]), 'tkhd');
      if (tk && u32(buf, tk.o) >= 92) { buf.writeUInt32BE(aw * 65536, tk.o + 84); buf.writeUInt32BE(ah * 65536, tk.o + 88); }
      const tail = Buffer.alloc(16); tail.writeUInt32BE(16, 0); tail.write('free', 4, 'latin1'); tail.write('WVDM', 8, 'latin1'); tail.writeUInt16BE(tw, 12); tail.writeUInt16BE(th, 14);
      buf = Buffer.concat([buf, tail]); notes.push(`dimensioni ${tw}x${th} -> ${aw}x${ah} nel contenitore`);
    }
  }
  return changed ? { buf, notes } : null;
}
