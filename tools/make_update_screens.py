#!/usr/bin/env python3
"""Genera le schermate dell'installatore (app/assets/update-screen-<lingua>.z, 960x544 RGBA compresso con zlib) nelle 19 lingue.
Usa Roboto (latino/cirillico) e Noto Sans CJK (giapponese, coreano, cinese). Esegui: python3 tools/make_update_screens.py"""
import zlib
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "app/assets"
TTC = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
# lingua: (titolo, riga1, riga2, indice del font CJK o None)
T = {
 "it": ("Aggiornamento in corso", "Non spegnere la Vita.", "WhatsApp si riaprirà da sola tra pochi secondi.", None),
 "en": ("Updating", "Do not turn off the Vita.", "WhatsApp will reopen by itself in a few seconds.", None),
 "fr": ("Mise à jour en cours", "N'éteignez pas la Vita.", "WhatsApp se rouvrira seule dans quelques secondes.", None),
 "de": ("Update läuft", "Vita nicht ausschalten.", "WhatsApp öffnet sich in wenigen Sekunden von selbst wieder.", None),
 "es": ("Actualizando", "No apagues la Vita.", "WhatsApp se abrirá sola en unos segundos.", None),
 "pt": ("A atualizar", "Não desligue a Vita.", "O WhatsApp volta a abrir sozinho dentro de segundos.", None),
 "pt-BR": ("Atualizando", "Não desligue o Vita.", "O WhatsApp abrirá sozinho em alguns segundos.", None),
 "nl": ("Bezig met updaten", "Zet de Vita niet uit.", "WhatsApp opent over enkele seconden vanzelf opnieuw.", None),
 "da": ("Opdaterer", "Sluk ikke Vita.", "WhatsApp åbner igen af sig selv om få sekunder.", None),
 "fi": ("Päivitetään", "Älä sammuta Vitaa.", "WhatsApp avautuu itse uudelleen muutaman sekunnin kuluttua.", None),
 "no": ("Oppdaterer", "Ikke slå av Vita.", "WhatsApp åpnes igjen av seg selv om noen sekunder.", None),
 "sv": ("Uppdaterar", "Stäng inte av Vita.", "WhatsApp öppnas igen av sig själv om några sekunder.", None),
 "pl": ("Trwa aktualizacja", "Nie wyłączaj Vity.", "WhatsApp otworzy się sam za kilka sekund.", None),
 "tr": ("Güncelleniyor", "Vita'yı kapatmayın.", "WhatsApp birkaç saniye içinde kendiliğinden yeniden açılacak.", None),
 "ru": ("Идёт обновление", "Не выключайте Vita.", "WhatsApp откроется сам через несколько секунд.", None),
 "ja": ("アップデート中", "Vitaの電源を切らないでください。", "数秒後にWhatsAppが自動で開きます。", 0),
 "ko": ("업데이트 중", "Vita를 끄지 마세요.", "몇 초 후 WhatsApp이 저절로 다시 열립니다.", 1),
 "zh-Hans": ("正在更新", "请勿关闭Vita电源。", "几秒后WhatsApp将自动重新打开。", 2),
 "zh-Hant": ("正在更新", "請勿關閉Vita電源。", "幾秒後WhatsApp將自動重新開啟。", 3),
}
W, H = 960, 544
icon = Image.open(ROOT / "app/sce_sys/icon0.png").convert("RGBA").resize((128, 128))
for lang, (title, l1, l2, cjk) in T.items():
    def font(size):
        return ImageFont.truetype(TTC, size, index=cjk) if cjk is not None else ImageFont.truetype(str(ASSETS / "Roboto-Regular.ttf"), size)
    im = Image.new("RGBA", (W, H), (0x07, 0x5E, 0x54, 255)); d = ImageDraw.Draw(im); im.alpha_composite(icon, ((W - 128) // 2, 90))
    def center(t, y, f, col):
        w = d.textlength(t, font=f); d.text(((W - w) / 2, y), t, font=f, fill=col)
    big = 44
    while d.textlength(title, font=font(big)) > W - 80: big -= 2
    center(title, 250, font(big), (255, 255, 255, 255))
    for i, line in enumerate((l1, l2)):
        s = 24
        while d.textlength(line, font=font(s)) > W - 60: s -= 1
        center(line, 330 + i * 38, font(s), (220, 248, 198, 255))
    d.rounded_rectangle((260, 450, 700, 466), 8, fill=(0x0A, 0x4D, 0x45, 255)); d.rounded_rectangle((260, 450, 480, 466), 8, fill=(0x25, 0xD3, 0x66, 255))
    (ASSETS / f"update-screen-{lang}.z").write_bytes(zlib.compress(im.tobytes(), 9))   # compressa: l'installatore la espande con zlib
print(len(T), "schermate scritte in app/assets/ (", sum(f.stat().st_size for f in ASSETS.glob("update-screen-*.z")) // 1024, "KB in tutto)")
