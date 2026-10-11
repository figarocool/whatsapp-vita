#!/usr/bin/env python3
"""Avvolge in T("...") le frasi italiane dell'interfaccia presenti in tools/i18n/part*.py (uso unico, poi si committa il risultato)."""
import glob, importlib.util, re, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
keys = set()
for f in sorted(glob.glob(str(ROOT / "tools/i18n/part*.py"))):
    spc = importlib.util.spec_from_file_location(Path(f).stem, f); m = importlib.util.module_from_spec(spc); spc.loader.exec_module(m)
    keys |= {e[0] for e in m.E}

def lex(src):
    """Ritorna le posizioni (inizio, fine, contenuto) dei letterali stringa C, ignorando commenti e caratteri."""
    out = []; i = 0; n = len(src)
    while i < n:
        c = src[i]
        if c == "/" and src[i:i+2] == "//":
            j = src.find("\n", i); i = n if j < 0 else j; continue
        if c == "/" and src[i:i+2] == "/*":
            j = src.find("*/", i + 2); i = n if j < 0 else j + 2; continue
        if c == "'":
            j = i + 1
            while j < n and src[j] != "'":
                j += 2 if src[j] == "\\" else 1
            i = j + 1; continue
        if c == '"':
            j = i + 1
            while j < n and src[j] != '"':
                j += 2 if src[j] == "\\" else 1
            out.append((i, j + 1, src[i+1:j])); i = j + 1; continue
        i += 1
    return out

total = 0
for rel in ("app/src/main.c", "app/src/extra.inc", "app/src/updater.c"):
    p = ROOT / rel; src = p.read_text(encoding="utf-8"); lits = lex(src); res = []; last = 0; cnt = 0
    for idx, (a, b, content) in enumerate(lits):
        if content not in keys: continue
        before = src[:a].rstrip(); after = src[b:].lstrip()
        if before.endswith("T(") or before.endswith("tr("): continue
        if before.endswith('"') or after.startswith('"'):
            print(f"  ATTENZIONE {rel}: letterale concatenato, da sistemare a mano: {content[:50]!r}"); continue
        line_start = src.rfind("\n", 0, a) + 1; line = src[line_start:src.find("\n", a)]
        if line.lstrip().startswith("#"): print(f"  salto (direttiva): {content[:50]!r}"); continue
        if re.match(r"\s*static\s+const\s+char\s*\*\s*\w+\s*\[", line): continue   # elenchi statici: la traduzione si fa dove li si usa
        res.append(src[last:a]); res.append("T(" + src[a:b] + ")"); last = b; cnt += 1
    res.append(src[last:]); p.write_text("".join(res), encoding="utf-8"); total += cnt; print(f"{rel}: {cnt} frasi avvolte")
print("totale:", total)
