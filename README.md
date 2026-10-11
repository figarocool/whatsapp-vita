# WhatsApp per PS Vita

Un client WhatsApp per PS Vita: lista chat, conversazioni con foto/video/emoji, storie, canali, profilo, notifiche di sistema anche ad app chiusa e aggiornamenti automatici.

Funziona con tre parti:

| Parte | Cosa fa |
|---|---|
| `bridge/` | Server Node.js sul PC (libreria [Baileys](https://github.com/WhiskeySockets/Baileys)). È lui il "dispositivo collegato" a WhatsApp: tiene la sessione, la cronologia e converte foto/video per la Vita. |
| `app/` | L'app per Vita (VitaSDK, vita2d). Parla solo col bridge. |
| `plugin/` | Plugin taiHEN che mostra le notifiche di sistema quando l'app è chiusa. |

> **Attenzione:** il bridge non ha password. Usalo solo su una rete di casa di cui ti fidi.

## Installazione rapida

1. **Bridge** (sul PC, con Node.js ≥ 18 e `ffmpeg`): `cd bridge && npm install && npm start`, poi apri `http://<ip-del-pc>:8787/qr.png` e scansiona il QR da *WhatsApp → Dispositivi collegati*. I dati restano in `bridge/data/` (non vanno mai condivisi).
2. **Vita**: scarica `whatsapp-vita.vpk` dalle [release](https://github.com/figarocool/whatsapp-vita/releases/latest) e installalo con VitaShell.
   - Serve **"Enable unsafe homebrew"** in HENkaku/Ensō. Chi non può attivarlo usa `whatsapp-vita-sicura.vpk`: funziona uguale ma senza notifiche e senza aggiornamento automatico.
3. Apri l'app: cerca da sola il bridge nella rete (porta 8787). In alternativa scrivi l'indirizzo nelle impostazioni del profilo.
4. L'app propone di installare il plugin delle notifiche e di riavviare la Vita.

Il popup di notifica che si apre toccandolo funziona con firmware **3.60 e 3.65**. Su altri firmware le notifiche arrivano senza il tocco.

## Aggiornamenti automatici

All'avvio l'app legge `whatsapp-vita-update.json` dall'ultima [release](https://github.com/figarocool/whatsapp-vita/releases). Se la versione è più nuova propone di aggiornare: scarica `whatsapp-vita.vpk` (con barra di avanzamento), controlla la firma SHA-256, estrae il pacchetto e lancia un piccolo installatore (`WHAPUPD01`) che chiude l'app, la sostituisce e la riapre. Dal Profilo → *Controlla aggiornamenti* si può forzare il controllo. Il registro è in `ux0:data/whatsapp/update.log`.

Le build locali (`dev`) non vengono mai sostituite.

### Pubblicare una release

```sh
scripts/release.sh v1.0.1 "Cosa cambia in questa versione"
```

Compila plugin e app con la versione incorporata, crea il manifest con lo SHA-256 e pubblica i file con `gh release create`.

## Compilare

```sh
# plugin
cd plugin && mkdir -p build && cd build && cmake .. && make
# app (include il plugin e l'installatore nel VPK)
cd ../../app && mkdir -p build && cd build && cmake -DWA_VERSION=v1.0.0 .. && make
```

## Comandi

Tocco per quasi tutto; tenere premuto su un messaggio = menu (rispondi, reazioni, copia, modifica, elimina). Cerchio = indietro, X = scrivi/scatta, Triangolo = emoji/cambia fotocamera, L1/R1 = cambia scheda, stick/croce = scorri. Nel visualizzatore foto: pizzico con due dita = zoom.

## Crediti e licenza

Il progetto include parti derivate da software GPL-3.0, quindi è distribuito con licenza **GPL-3.0** (vedi `LICENSE`):

- la preparazione del pacchetto per l'installazione (`app/src/fpkg_head.h`, `app/assets/vitashell-head.bin`) viene da [VitaShell](https://github.com/TheOfficialFloW/VitaShell) di TheFloW;
- lo schema di aggiornamento (manifest + installatore separato) è ripreso da *Desktop Mode*;
- la notifica con tocco usa il metodo documentato da Princess of Sleeping (*SceShell-Notice-PoC*) e da VitaDB Downloader.

Font: Roboto (Apache-2.0); emoji: Noto Color Emoji (OFL).
