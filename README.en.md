# WhatsApp for PS Vita

[🇮🇹 Italiano](README.md) · 🇬🇧 English

A complete WhatsApp client for the PS Vita: chat list, conversations with photos, videos, voice messages, emoji, status stories, channels, profile, system notifications even when the app is closed, and automatic updates.

**Since version 2.0 no PC or server is needed.** The WhatsApp "engine" runs inside the Vita itself: install the VPK, scan the QR code from your phone and it works.

## How it works

| Part | What it does |
|---|---|
| `app/` | The Vita app (VitaSDK, vita2d). It contains the **WhatsApp engine**: [QuickJS](https://bellard.org/quickjs/) running the [Baileys](https://github.com/WhiskeySockets/Baileys) library, with the heavy parts (TLS, cryptography, curve25519, images, Opus audio) written in C. |
| `plugin/` | taiHEN plugin for the shell. It embeds a tiny WhatsApp client (reduced TLS + Noise) that stays connected while the app is closed and shows a system notification for every message. |
| `engine/` | The engine: native C code, the JS bundle and the build tools. |
| `bridge/` | The JavaScript "bridge" (chat logic, conversion, storage). It is the same code that runs inside the Vita; it can still be started on a PC for development. |

The app and the plugin are never connected together: when you open the app the plugin stops, when you close it the plugin starts again by itself.

## Installation

1. Download `whatsapp-vita.vpk` from the [releases](https://github.com/figarocool/whatsapp-vita/releases/latest) and install it with VitaShell.
   - **"Enable unsafe homebrew"** is required in HENkaku/Ensō.
2. Open the app. On your phone go to **WhatsApp → Linked devices → Link a device** and scan the QR code shown on the Vita.
3. Wait for the first chat sync (it can take a few minutes: the Vita has to decrypt the history).
4. The app offers to install the **notification plugin** and to reboot the Vita. After the reboot notifications arrive even with the app closed (the plugin connects about 30-40 seconds after power-on).

Tapping a notification to open the app works on firmware **3.60 and 3.65**. On other firmwares notifications are shown without the tap action.

### If you used an older version (with the PC server)

- Update from inside the app (*Profile → Check for updates*) or install the new VPK by hand.
- The PC server is **no longer needed**; you can shut it down. The old configuration is ignored.
- The internal engine has a new session: **scan the QR code again**. The old linked device stays in WhatsApp's list on your phone; you can remove it.
- The app also updates the notification plugin by itself and asks you to reboot the Vita.

## What works

- Private chats and groups, photos, videos, voice messages, documents, emoji, replies, reactions, edit and delete, link previews.
- Status stories, channels, call log, profile, search, pinned/muted/archived chats.
- Video and voice playback **inside the app**: H.264 videos (even with a high level or cropping) are adapted to the Vita's decoder, Opus voice messages are decoded in C.
- System notifications with the app closed, with the contact name in the app's language; tapping opens the right chat.
- Fast start: the last chat list, photos and latest conversations are cached on disk and appear immediately.
- 19 languages.

## Known limitations

- **HEVC** (H.265) videos, and H.264 videos heavier than the Vita's player can handle, cannot be played: the Vita has no decoder for them and the CPU is too slow to convert them.
- The history shown at first is what WhatsApp sends to a new device (a few weeks). Scrolling up in a chat asks the phone for older messages; the phone must be on and connected.
- Previews of older videos may be missing (WhatsApp does not send them in the history); the video still opens.
- Notifications show who wrote, not the text (the plugin does not decrypt messages: they stay queued for the app).
- Tested mostly on a Vita with firmware 3.60 (Ensō). Other consoles or firmwares may need tweaks.
- The "safe" build (without unsafe homebrew) is no longer published: the internal engine has not been tested in that mode.

## Languages

The interface is available in **19 languages**, the ones the Vita supports: Italian, English, French, German, Spanish, Portuguese (Portugal and Brazil), Dutch, Danish, Finnish, Norwegian, Swedish, Polish, Turkish, Russian, Japanese, Korean and Chinese (Simplified and Traditional).

- At start the app uses **the Vita's system language**; if unsupported it falls back to English.
- Profile → *Language* lets you pick another language, or *Automatic* to go back to the system one. The choice is stored in `ux0:data/whatsapp/lang.txt`.
- Translations live in `tools/i18n/part*.py` (one row per sentence, one column per language); `python3 tools/gen_i18n.py` checks that placeholders (`%d`, `%s`...) match in every language and regenerates `app/src/i18n_data.c`. Sentences in the code are written in Italian and `T("...")` translates them.
- Japanese, Korean and Chinese use dedicated fonts built from Noto Sans CJK with `tools/make_cjk_fonts.py`. Other Asian characters in contacts' messages may appear as boxes.
- Translations have not been reviewed by native speakers: corrections welcome.

## Automatic updates

At start the app reads `whatsapp-vita-update.json` from the latest [release](https://github.com/figarocool/whatsapp-vita/releases). If the version is newer it offers to update: it downloads `whatsapp-vita.vpk` (with a progress bar), checks the SHA-256 signature, unpacks it and launches a small installer (`WHAPUPD01`) that closes the app, replaces it and reopens it. Profile → *Check for updates* forces the check. The log is in `ux0:data/whatsapp/update.log`. Local (`dev`) builds are never replaced.

### Publishing a release

```sh
scripts/release.sh v2.0.0 "What changes in this version"
```

It builds engine, plugin and app with the version embedded, creates the manifest with the SHA-256 and publishes the files with `gh release create`.

## Building

You need VitaSDK, Node.js, CMake and the mbedTLS 3.6 sources (for the plugin: `-DMBED_SRC=/path/to/mbedtls-3.6.x`).

```sh
# engine (Baileys + bridge -> QuickJS bytecode)
(cd bridge && npm install) && engine/build_engine.sh
# plugin
cd plugin && cmake -S . -B build -DMBED_SRC=/path/to/mbedtls-3.6.x && cmake --build build
# app (includes engine, plugin and installer in the VPK)
cd ../app && cmake -S . -B build -DWA_VERSION=v2.0.0 && cmake --build build
```

To try the engine on a PC, `engine/` has an `engine_pc` build using the same native sources (see `engine/CMakeLists.txt`).

## Controls

Touch for almost everything; long-press a message = menu (reply, reactions, copy, edit, delete). Circle = back, X = write/shoot, Triangle = emoji/switch camera, L1/R1 = switch tab, stick/d-pad = scroll. In the photo viewer: two-finger pinch = zoom.

## Author

**Stefano Basile**, developer and cyberculture enthusiast, founder of [8byte](https://8byte.it). Website: <https://stefanobasile.com/> - GitHub: <https://github.com/figarocool>.

## Credits and license

The project includes parts derived from GPL-3.0 software, so it is distributed under the **GPL-3.0** license (see `LICENSE`):

- the installation package preparation (`app/src/fpkg_head.h`, `app/assets/vitashell-head.bin`) comes from [VitaShell](https://github.com/TheOfficialFloW/VitaShell) by TheFloW;
- the update scheme (manifest + separate installer) is taken from *Desktop Mode*;
- the tap-to-open notification uses the method documented by Princess of Sleeping (*SceShell-Notice-PoC*) and VitaDB Downloader;
- the video decoder work draws on the [FFmpeg-vita](https://github.com/fish47/FFmpeg-vita) project.

Third-party components bundled in the engine (list and licenses in [THIRD_PARTY.md](THIRD_PARTY.md)): QuickJS, Baileys, mbedTLS, libopus/libogg, libjpeg, libpng, libwebp, zlib, TweetNaCl (curve25519).

Fonts: Roboto (Apache-2.0); Noto Sans CJK for Japanese, Korean and Chinese (SIL OFL 1.1, see `app/assets/LICENSE-NotoSansCJK.txt`); emoji: Noto Color Emoji (OFL).

WhatsApp is a trademark of Meta. This is an independent, unofficial project.
