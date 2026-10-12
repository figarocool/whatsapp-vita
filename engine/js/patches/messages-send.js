// Baileys rc14 tratta un timeout USync come una lista vuota e invia senza destinatari.
export function patchDeviceDiscovery(source) {
  const replacements = [
    ['const result = await sock.executeUSyncQuery(query);',
      "const result = await sock.executeUSyncQuery(query);\n        if (!result) throw new Error('WhatsApp non ha risposto alla ricerca dei dispositivi: messaggio non inviato');"],
    ['devices.push(...sessionDevices);',
      "if (!sessionDevices.length) throw new Error('Nessun dispositivo disponibile: messaggio non inviato');\n                        devices.push(...sessionDevices);"]
  ];
  for (const [original, replacement] of replacements) {
    if (!source.includes(original)) throw new Error('Patch invio incompatibile con questa versione di Baileys');
    source = source.replace(original, replacement);
  }
  return source;
}
