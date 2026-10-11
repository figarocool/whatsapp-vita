#!/usr/bin/env python3
"""Ricava i font per giapponese, coreano e cinese (app/assets/font-*.ttf) da Noto Sans CJK, tenendo solo i caratteri che servono.
Richiede fontTools (python3 -m venv v && v/bin/pip install fonttools) e NotoSansCJK-Regular.ttc (Debian: fonts-noto-cjk).
Prima eseguire tools/gen_i18n.py: scrive tools/i18n/glyphs-*.txt con i caratteri usati dalle traduzioni.
Noto Sans CJK e' sotto licenza SIL OFL 1.1 (vedi app/assets/LICENSE-NotoSansCJK.txt)."""
import sys
from pathlib import Path
from fontTools import subset
from fontTools.ttLib import TTFont, TTCollection

ROOT = Path(__file__).resolve().parent.parent
TTC = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
FACES = {"ja": (0, "font-ja.ttf", "日本語"), "ko": (1, "font-ko.ttf", "한국어"), "zh-Hans": (2, "font-zhs.ttf", "简体中文"), "zh-Hant": (3, "font-zht.ttf", "繁體中文")}
BASE = "".join(chr(c) for c in list(range(0x20, 0x7F)) + list(range(0xA0, 0x100))) + "–—‘’“”…•€™’、。，．：；！？（）「」『』【】《》〈〉〜～・－　" + "０１２３４５６７８９ＡＢＣ"
col = TTCollection(TTC)
for lang, (idx, out, name) in FACES.items():
    chars = (ROOT / f"tools/i18n/glyphs-{lang}.txt").read_text(encoding="utf-8")
    text = set(BASE) | set(chars) | set(name)
    opts = subset.Options(); opts.layout_features = ["kern", "palt", "vert"]; opts.name_IDs = [0, 1, 2, 3, 4, 5, 6]; opts.notdef_outline = True; opts.hinting = False; opts.desubroutinize = True
    font = col.fonts[idx]; sub = subset.Subsetter(opts); sub.populate(text="".join(sorted(text))); sub.subset(font)
    dest = ROOT / "app/assets" / out; font.save(str(dest)); print(f"{lang}: {len(text)} caratteri -> {dest.name} {dest.stat().st_size // 1024} KB")
