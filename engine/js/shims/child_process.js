// Sulla Vita non ci sono programmi esterni: ffmpeg/ffprobe vengono "interpretati" dal motore.
//  - video: l'originale va bene com'e' (H.264); se e' HEVC la conversione fallisce subito
//  - audio: i vocali OGG/Opus si decodificano in C e diventano WAV; gli altri formati restano uguali
import EventEmitter from 'events';
const N = globalThis.__nat;
const enoent = (cmd) => { const e = new Error(`spawn ${cmd} ENOENT`); e.code = 'ENOENT'; return e; };
const headTail = (p, size) => { const a = Buffer.from(N.fsReadRange(p, 0, Math.min(size, 524288)) || []); if (size <= 524288) return a; const b = Buffer.from(N.fsReadRange(p, size - 524288, 524288) || []); return Buffer.concat([a, b]); };
function runFfmpeg(args) {   // ritorna { code, durationUs, err }
  const i = args.indexOf('-i'); const src = args[i + 1]; const pi = args.indexOf('-progress'); const out = pi > 0 ? args[pi - 1] : args[args.length - 1];
  if (i < 0 || !src || !out) return { code: 1, err: 'argomenti non riconosciuti' };
  const st = N.fsStat(src); if (!st) return { code: 1, err: 'file di ingresso mancante' };
  if (args.includes('-vn')) {   // audio
    const full = Buffer.from(N.fsRead(src)); const r = N.audioToWav(full);
    if (r) { if (!N.fsWrite(out, r.data)) return { code: 1, err: 'scrittura fallita' }; return { code: 0, durationUs: Math.round(r.seconds * 1e6) }; }
    return N.fsCopy(src, out) ? { code: 0, durationUs: 0 } : { code: 1, err: 'copia fallita' };
  }
  const ht = headTail(src, st.size); const vi = N.mp4Info(ht);
  console.log('video: ' + (vi ? `${vi.codec} ${vi.width}x${vi.height} profilo ${vi.profile} livello ${vi.level}` : 'formato non riconosciuto (non MP4?)') + ', ' + Math.round(st.size / 1024) + ' KB');
  if (!N.videoPlayable(ht)) return { code: 1, err: 'video HEVC: formato non supportato dalla Vita' };
  return N.fsCopy(src, out) ? { code: 0, durationUs: 0 } : { code: 1, err: 'copia fallita' };
}
const argsOf = (cmd, args) => { if (cmd === 'nice') { const k = args.indexOf('ffmpeg'); return k >= 0 ? ['ffmpeg', args.slice(k + 1)] : [args[0], args.slice(1)]; } return [cmd, args]; };
export const spawn = (cmd0, args0 = []) => {
  const [cmd, args] = argsOf(cmd0, args0); const p = new EventEmitter(); p.stdout = new EventEmitter(); p.stderr = new EventEmitter(); p.kill = () => {};
  setTimeout(() => {
    if (cmd !== 'ffmpeg') { p.emit('error', enoent(cmd)); p.emit('close', 127); return; }
    const r = runFfmpeg(args); if (r.err) p.stderr.emit('data', Buffer.from(r.err)); else if (r.durationUs) { p.stdout.emit('data', Buffer.from(`out_time_us=${Math.floor(r.durationUs / 2)}\n`)); p.stdout.emit('data', Buffer.from(`out_time_us=${r.durationUs}\n`)); }
    p.emit('close', r.code);
  }, 0);
  return p;
};
export const execFile = (cmd0, args0, opts, cb) => {
  if (typeof opts === 'function') cb = opts; const [cmd, args] = argsOf(cmd0, args0 || []);
  setTimeout(() => { if (cmd === 'ffmpeg') { const r = runFfmpeg(args); cb && cb(r.code ? new Error(r.err || 'ffmpeg fallito') : null, '', r.err || ''); } else cb && cb(enoent(cmd), '', ''); }, 0);
  return new EventEmitter();
};
export const exec = (cmd, opts, cb) => { if (typeof opts === 'function') cb = opts; setTimeout(() => cb && cb(enoent(cmd), '', ''), 0); return new EventEmitter(); };
export default { spawn, execFile, exec };
