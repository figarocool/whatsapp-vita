import './prelude.js';
import { spawn, execFile } from 'child_process';
const N = globalThis.__nat;
const run = (name, args) => new Promise((res) => { const p = spawn('ffmpeg', args); let out = '', err = ''; p.stdout.on('data', (d) => { out += d; }); p.stderr.on('data', (d) => { err += d; }); p.on('close', (c) => res(`${name}: codice ${c} ${err ? '(' + err + ')' : ''} ${out.trim().split('\n').pop()}`)); });
(async () => {
  console.log(await run('vocale ogg/opus', ['-y', '-i', '/tmp/imgtest/voice.ogg', '-vn', '-c:a', 'aac', '/tmp/imgtest/voice.out.m4a', '-progress', 'pipe:1']));
  const w = Buffer.from(N.fsRead('/tmp/imgtest/voice.out.m4a')); console.log('  uscita:', w.length, 'byte, intestazione', w.slice(0, 4).toString(), w.slice(8, 12).toString());
  console.log(await run('video H.264', ['-y', '-i', '/tmp/imgtest/vid264.mp4', '-c:v', 'libx264', '/tmp/imgtest/vid.out.mp4', '-progress', 'pipe:1']));
  console.log(await run('video L5.1', ['-y', '-i', '/tmp/imgtest/vid51.mp4', '-c:v', 'libx264', '/tmp/imgtest/vid51.out.mp4', '-progress', 'pipe:1']));
  console.log(await run('video HEVC', ['-y', '-i', '/tmp/imgtest/vid265.mp4', '-c:v', 'libx264', '/tmp/imgtest/vid265.out.mp4', '-progress', 'pipe:1']));
})();
