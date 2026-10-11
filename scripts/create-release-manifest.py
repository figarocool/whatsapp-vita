#!/usr/bin/env python3
"""Scrive il manifest letto dall'aggiornatore dell'app: whatsapp-vita-update.json (schema 1)."""
import hashlib, json, re, sys
from pathlib import Path

if len(sys.argv) < 3 or not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+([-+][0-9A-Za-z.+_-]*)?", sys.argv[1]):
    raise SystemExit("uso: create-release-manifest.py vX.Y.Z percorso/whatsapp-vita.vpk [note]")
tag, vpk = sys.argv[1], Path(sys.argv[2])
notes = sys.argv[3] if len(sys.argv) > 3 else ""
if not vpk.is_file():
    raise SystemExit(f"manca il pacchetto: {vpk}")
data = vpk.read_bytes()
manifest = {
    "schema": 1,
    "tag": tag,
    "core": {"asset": "whatsapp-vita.vpk", "sha256": hashlib.sha256(data).hexdigest(), "size": len(data)},
    "notes": notes[:190],
}
out = vpk.with_name("whatsapp-vita-update.json")
out.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
print(out)
