# WhatsApp per PS Vita

🇮🇹 Italiano · [🇬🇧 English](README.en.md)

Un client WhatsApp completo per PS Vita: lista chat, conversazioni con foto, video, vocali, emoji, storie, canali, profilo, notifiche di sistema anche ad app chiusa e aggiornamenti automatici.

**Dalla versione 2.0 non serve più nessun PC o server.** Il "motore" di WhatsApp gira dentro la Vita stessa: installi il VPK, scansioni il QR dal telefono e funziona.

## Come funziona

| Parte | Cosa fa |
|---|---|
| `app/` | L'app per Vita (VitaSDK, vita2d). Contiene il **motore WhatsApp**: [QuickJS](https://bellard.org/quickjs/) che esegue la libreria [Baileys](https://github.com/WhiskeySockets/Baileys), con la parte pesante (TLS, crittografia, curve25519, immagini, audio Opus) scritta in C. |
| `plugin/` | Plugin taiHEN per la shell. Contiene un piccolo client WhatsApp (TLS ridotto + Noise) che resta collegato quando l'app è chiusa e mostra una notifica di sistema per ogni messaggio. |
| `engine/` | Il motore: codice nativo C, pacchetto JS e strumenti di compilazione. |
| `bridge/` | Il codice JavaScript del "bridge" (logica di chat, conversione, archivio). È lo stesso che gira dentro la Vita; si può ancora avviare su un PC per lo sviluppo. |

L'app e il plugin non sono mai collegati insieme: quando apri l'app il plugin si ferma, quando la chiudi riparte da solo.

## Installazione

1. Scarica `whatsapp-vita.vpk` dalle [release](https://github.com/figarocool/whatsapp-vita/releases/latest) e installalo con VitaShell.
   - Serve **"Enable unsafe homebrew"** in HENkaku/Ensō.
2. Apri l'app e, sul telefono, vai in **WhatsApp → Dispositivi collegati → Collega un dispositivo**, poi inquadra il QR mostrato dalla Vita.
3. Aspetta la prima sincronizzazione delle chat (può richiedere qualche minuto: la Vita deve decifrare la cronologia).
4. L'app propone di installare il **plugin delle notifiche** e di riavviare la Vita. Dopo il riavvio le notifiche arrivano anche con l'app chiusa (il plugin si collega circa 30-40 secondi dopo l'accensione).

Il popup di notifica che si apre toccandolo funziona con firmware **3.60 e 3.65**. Su altri firmware le notifiche arrivano senza il tocco.

### Se usavi una versione precedente (con il server sul PC)

- Aggiorna dall'app: dopo qualche secondo propone l'aggiornamento da sola, oppure *Profilo → Controlla aggiornamenti*. Se sei fermo sulla schermata del QR (dove il Profilo non c'è) scarica `whatsapp-vita.vpk` dalle release e installalo a mano con VitaShell.
- Il server sul PC **non serve più** e puoi spegnerlo. La configurazione vecchia viene ignorata.
- Il motore interno ha una sessione nuova: **scansiona di nuovo il QR**. Il vecchio dispositivo collegato resta nell'elenco di WhatsApp sul telefono: puoi rimuoverlo.
- L'app aggiorna da sola anche il plugin delle notifiche e ti chiede di riavviare la Vita.

## Cosa funziona

- Chat private e gruppi, foto, video, vocali, documenti, emoji, risposte, reazioni, modifica ed eliminazione, anteprime dei link.
- Storie, canali, chiamate (registro), profilo, ricerca, chat fissate/silenziate/archiviate.
- Riproduzione di video e vocali **dentro l'app**: i video H.264 (anche con livello alto o ritaglio) sono adattati al decodificatore della Vita, i vocali Opus si decodificano in C.
- Notifiche di sistema ad app chiusa, con nome del contatto nella lingua dell'app; il tocco apre la chat giusta.
- Avvio rapido: l'ultima lista chat, le foto e le ultime conversazioni sono salvate sul disco e compaiono subito.
- 19 lingue.

## Limiti noti

- I video **HEVC** (H.265) e gli H.264 più pesanti del lettore della Vita non si possono riprodurre: la Vita non ha il decodificatore e la CPU non basta per convertirli.
- La cronologia mostrata all'inizio è quella che WhatsApp invia a un dispositivo nuovo (poche settimane). Scorrendo in alto in una chat l'app chiede altri messaggi al telefono, che deve essere acceso e connesso.
- Le anteprime dei video più vecchi possono mancare (WhatsApp non le invia nella cronologia); il video si apre comunque.
- Le notifiche mostrano chi scrive, non il testo (il plugin non decifra i messaggi: restano in coda per l'app).
- Provato soprattutto su Vita con firmware 3.60 (Ensō). Il video di un'altra console o firmware potrebbe richiedere ritocchi.
- La versione "sicura" (senza unsafe homebrew) non è più pubblicata: il motore interno non è stato provato in quella modalità.

## Lingue

L'interfaccia è disponibile in **19 lingue**, quelle della Vita: italiano, inglese, francese, tedesco, spagnolo, portoghese (Portogallo e Brasile), olandese, danese, finlandese, norvegese, svedese, polacco, turco, russo, giapponese, coreano e cinese (semplificato e tradizionale).

- All'avvio l'app usa **la lingua di sistema della Vita**. Se la lingua non è tra quelle supportate usa l'inglese.
- Dal Profilo → *Lingua* si sceglie un'altra lingua, oppure *Automatica* per tornare a quella di sistema. La scelta è in `ux0:data/whatsapp/lang.txt`.
- Le traduzioni sono in `tools/i18n/part*.py` (una riga per frase, una colonna per lingua); `python3 tools/gen_i18n.py` controlla che i segnaposto (`%d`, `%s`...) siano identici in ogni lingua e rigenera `app/src/i18n_data.c`. Le frasi nel codice sono scritte in italiano e `T("...")` le traduce.
- Giapponese, coreano e cinese usano font dedicati ricavati da Noto Sans CJK con `tools/make_cjk_fonts.py`. Altri caratteri asiatici nei messaggi dei contatti possono comparire come quadratini.
- Le traduzioni non sono state riviste da madrelingua: correzioni benvenute.

## Aggiornamenti automatici

All'avvio l'app legge `whatsapp-vita-update.json` dall'ultima [release](https://github.com/figarocool/whatsapp-vita/releases). Se la versione è più nuova propone di aggiornare: scarica `whatsapp-vita.vpk` (con barra di avanzamento), controlla la firma SHA-256, estrae il pacchetto e lancia un piccolo installatore (`WHAPUPD01`) che chiude l'app, la sostituisce e la riapre. Dal Profilo → *Controlla aggiornamenti* si può forzare il controllo. Il registro è in `ux0:data/whatsapp/update.log`. Le build locali (`dev`) non vengono mai sostituite.

### Pubblicare una release

```sh
scripts/release.sh v2.0.0 "Cosa cambia in questa versione"
```

Compila motore, plugin e app con la versione incorporata, crea il manifest con lo SHA-256 e pubblica i file con `gh release create`.

## Compilare

Servono VitaSDK, Node.js, CMake e i sorgenti di mbedTLS 3.6 (per il plugin: `-DMBED_SRC=/percorso/mbedtls-3.6.x`).

```sh
# motore (Baileys + bridge -> bytecode QuickJS)
(cd bridge && npm install) && engine/build_engine.sh
# plugin
cd plugin && cmake -S . -B build -DMBED_SRC=/percorso/mbedtls-3.6.x && cmake --build build
# app (include motore, plugin e installatore nel VPK)
cd ../app && cmake -S . -B build -DWA_VERSION=v2.0.0 && cmake --build build
```

Per provare il motore sul PC: `engine/` ha una versione `engine_pc` con gli stessi sorgenti nativi (vedi `engine/CMakeLists.txt`).

## Comandi

Tocco per quasi tutto; tenere premuto su un messaggio = menu (rispondi, reazioni, copia, modifica, elimina). Cerchio = indietro, X = scrivi/scatta, Triangolo = emoji/cambia fotocamera, L1/R1 = cambia scheda, stick/croce = scorri. Nel visualizzatore foto: pizzico con due dita = zoom.

## Autore

**Stefano Basile**, sviluppatore e appassionato di cybercultura, fondatore di [8byte](https://8byte.it). Sito web: <https://stefanobasile.com/> - GitHub: <https://github.com/figarocool>.

## Crediti e licenza

Il progetto include parti derivate da software GPL-3.0, quindi è distribuito con licenza **GPL-3.0** (vedi `LICENSE`):

- la preparazione del pacchetto per l'installazione (`app/src/fpkg_head.h`, `app/assets/vitashell-head.bin`) viene da [VitaShell](https://github.com/TheOfficialFloW/VitaShell) di TheFloW;
- lo schema di aggiornamento (manifest + installatore separato) è ripreso da *Desktop Mode*;
- la notifica con tocco usa il metodo documentato da Princess of Sleeping (*SceShell-Notice-PoC*) e da VitaDB Downloader;
- il decodificatore video sfrutta le indicazioni del progetto [FFmpeg-vita](https://github.com/fish47/FFmpeg-vita).

Componenti di terze parti incluse nel motore (elenco e licenze in [THIRD_PARTY.md](THIRD_PARTY.md)): QuickJS, Baileys, mbedTLS, libopus/libogg, libjpeg, libpng, libwebp, zlib, TweetNaCl (curve25519).

Font: Roboto (Apache-2.0); Noto Sans CJK per giapponese, coreano e cinese (SIL OFL 1.1, vedi `app/assets/LICENSE-NotoSansCJK.txt`); emoji: Noto Color Emoji (OFL).

WhatsApp è un marchio di Meta. Questo è un progetto indipendente e non ufficiale.
