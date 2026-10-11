const t0 = Date.now();
try {
  const B = await import('./baileys.bundle.js');
  console.log('caricato in', Date.now() - t0, 'ms; export:', Object.keys(B).length, typeof B.makeWASocket, typeof B.default);
} catch (e) { console.log('ERR', e && e.message, '\n', e && e.stack); }
