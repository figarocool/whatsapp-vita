import './prelude.js';
(async () => {
  let t0 = Date.now();
  let r = await fetch('https://example.com/');
  const txt = await r.text(); console.log('GET example.com ->', r.status, txt.length, 'byte', txt.includes('Example Domain') ? 'OK' : 'contenuto strano', Date.now() - t0, 'ms');
  t0 = Date.now(); r = await fetch('https://httpbin.org/post', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ ciao: 'mondo' }) });
  const j = await r.json(); console.log('POST httpbin ->', r.status, JSON.stringify(j.json), Date.now() - t0, 'ms');
  t0 = Date.now(); r = await fetch('https://httpbin.org/redirect/2'); console.log('redirect ->', r.status, r.url, Date.now() - t0, 'ms');
  t0 = Date.now(); r = await fetch('https://httpbin.org/stream-bytes/300000?chunk_size=7000'); const b = await r.arrayBuffer(); console.log('chunked 300000 ->', b.byteLength, 'byte', Date.now() - t0, 'ms');
  t0 = Date.now(); r = await fetch('https://httpbin.org/bytes/2000000'); let n = 0; for await (const c of r.body) n += c.length; console.log('stream 2 MB ->', n, 'byte', Date.now() - t0, 'ms');
})().catch((e) => console.log('ERRORE', e && e.message, e && e.stack));
