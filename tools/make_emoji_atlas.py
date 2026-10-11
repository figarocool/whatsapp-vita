#!/usr/bin/env python3
"""Genera app/assets/emoji.png (atlante 32px, 32 colonne) e emoji.bin (uint32 LE, codepoint per cella) da Noto Color Emoji."""
import struct, sys
from fontTools.ttLib import TTFont
from PIL import Image, ImageFont, ImageDraw

FONT = '/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf'
OUT = sys.argv[1] if len(sys.argv) > 1 else 'app/assets'
CELL, COLS = 32, 32

cmap = TTFont(FONT).getBestCmap()
skip = lambda c: (c < 0xA9 or c in (0xAE, 0x200D, 0xFE0F, 0x20E3) or 0x1F1E6 <= c <= 0x1F1FF
                  or 0x1F3FB <= c <= 0x1F3FF or 0xE0000 <= c <= 0xE007F or 0x2000 <= c <= 0x203B)
cps = [c for c in cmap if not skip(c)]
groups = [(0x1F600, 0x1F64F), (0x1F910, 0x1F9FF), (0x1F440, 0x1F4FF), (0x2600, 0x27BF)]
def key(c):
    for i, (a, b) in enumerate(groups):
        if a <= c <= b: return (i, c)
    return (len(groups), c)
cps.sort(key=key)

font = ImageFont.truetype(FONT, 109)
rows = (len(cps) + COLS - 1) // COLS
atlas = Image.new('RGBA', (COLS * CELL, rows * CELL), (0, 0, 0, 0))
ok = []
for c in cps:
    im = Image.new('RGBA', (136, 128), (0, 0, 0, 0))
    ImageDraw.Draw(im).text((0, 0), chr(c), font=font, embedded_color=True)
    if im.getbbox() is None: continue
    im = im.resize((CELL, int(CELL * 128 / 136)), Image.LANCZOS)
    i = len(ok)
    atlas.paste(im, ((i % COLS) * CELL, (i // COLS) * CELL + (CELL - im.height) // 2))
    ok.append(c)
atlas = atlas.crop((0, 0, COLS * CELL, ((len(ok) + COLS - 1) // COLS) * CELL))
atlas.save(f'{OUT}/emoji.png', optimize=True)
open(f'{OUT}/emoji.bin', 'wb').write(b''.join(struct.pack('<I', c) for c in ok))
print(len(ok), 'emoji', atlas.size)
