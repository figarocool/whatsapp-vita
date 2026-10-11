#!/usr/bin/env python3
"""Genera app/src/i18n_data.c dalle tabelle in tools/i18n/part*.py e controlla le traduzioni.

Ogni voce e' una tupla: (italiano, en, fr, de, es, pt, pt-BR, nl, da, fi, no, sv, pl, tr, ru, ja, ko, zh-Hans, zh-Hant)
L'italiano e' anche la CHIAVE e va scritto identico a come compare nel codice C (con eventuali \\xC3\\xA8).
pt-BR puo' essere None (= uguale al portoghese europeo).
Controlli: stessi segnaposto (%d, %s, %u...) nello stesso ordine, stessi a capo, nessuna traduzione vuota.
"""
import glob, importlib.util, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LANGS = ["it", "en", "fr", "de", "es", "pt", "pt-BR", "nl", "da", "fi", "no", "sv", "pl", "tr", "ru", "ja", "ko", "zh-Hans", "zh-Hant"]

def c_unescape(s):
    out = bytearray(); i = 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            n = s[i + 1]
            if n == "x":
                j = i + 2
                while j < len(s) and j < i + 4 and s[j] in "0123456789abcdefABCDEF": j += 1
                out.append(int(s[i + 2:j], 16)); i = j; continue
            out += {"n": b"\n", "t": b"\t", '"': b'"', "'": b"'", "\\": b"\\"}.get(n, n.encode()); i += 2; continue
        out += c.encode("utf-8"); i += 1
    return bytes(out)

def spec(b):
    return re.findall(rb"%[-+ #0-9.*]*(?:l|ll|h|hh|z)?[a-zA-Z%]", b)

def c_literal(b):
    r = []
    for ch in b:
        if ch == 0x22: r.append('\\"')
        elif ch == 0x5C: r.append("\\\\")
        elif ch == 0x0A: r.append("\\n")
        elif 0x20 <= ch < 0x7F: r.append(chr(ch))
        else: r.append("\\%03o" % ch)    # ottale a 3 cifre: nessuna ambiguita' con la cifra successiva
    return "".join(r)

entries = []
for f in sorted(glob.glob(str(ROOT / "tools/i18n/part*.py"))):
    spc = importlib.util.spec_from_file_location(Path(f).stem, f); m = importlib.util.module_from_spec(spc); spc.loader.exec_module(m)
    entries += list(m.E)

errors = []; seen = {}
for e in entries:
    if len(e) != len(LANGS): errors.append(f"{e[0]!r}: {len(e)} valori invece di {len(LANGS)}"); continue
    if e[0] in seen: errors.append(f"chiave doppia: {e[0]!r}"); continue
    seen[e[0]] = e
    key = c_unescape(e[0])
    for lang, v in zip(LANGS[1:], e[1:]):
        if v is None:
            if lang != "pt-BR": errors.append(f"{e[0]!r}: manca {lang}")
            continue
        if not v.strip(): errors.append(f"{e[0]!r}: {lang} vuota"); continue
        vb = v.encode("utf-8")
        if spec(vb) != spec(key): errors.append(f"{e[0]!r}: {lang} ha segnaposto diversi {spec(vb)} invece di {spec(key)}")
        if vb.count(b"\n") != key.count(b"\n"): errors.append(f"{e[0]!r}: {lang} ha un numero diverso di a capo")
if errors:
    print("ERRORI NELLE TRADUZIONI:"); [print(" -", x) for x in errors]; sys.exit(1)

rows = sorted(seen.values(), key=lambda e: c_unescape(e[0]))
out = ['// GENERATO da tools/gen_i18n.py: non modificare a mano. Sorgente: tools/i18n/part*.py', '#include "i18n.h"', '', f"const int I18N_COUNT = {len(rows)};", "const char *const I18N_KEYS[] = {"]
for e in rows: out.append(f'  "{e[0]}",')
out += ["};", "const char *const I18N_VAL[][L_COUNT] = {"]
for e in rows:
    vals = ['0']
    pt = e[5]
    for lang, v in zip(LANGS[1:], e[1:]):
        if v is None and lang == "pt-BR": v = pt
        vals.append('"' + c_literal(v.encode("utf-8")) + '"')
    out.append("  { " + ", ".join(vals) + " },")
out.append("};")
(ROOT / "app/src/i18n_data.c").write_text("\n".join(out) + "\n", encoding="utf-8")

# glifi necessari per i font delle lingue asiatiche (servono al sottoinsieme dei font)
for lang in ("ja", "ko", "zh-Hans", "zh-Hant"):
    i = LANGS.index(lang); chars = set()
    for e in rows:
        if e[i]: chars |= set(e[i])
    (ROOT / f"tools/i18n/glyphs-{lang}.txt").write_text("".join(sorted(chars)), encoding="utf-8")
print(f"{len(rows)} frasi x {len(LANGS)} lingue -> app/src/i18n_data.c")
