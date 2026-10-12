# Third-party software / Componenti di terze parti

The engine and the notification plugin bundle or link the following software. Each keeps its own license.

| Component | Use | License |
|---|---|---|
| [QuickJS](https://bellard.org/quickjs/) (2026-06-04, with PS Vita patches) | JavaScript engine | MIT |
| [Baileys](https://github.com/WhiskeySockets/Baileys) 7.0.0-rc | WhatsApp Web protocol library (bundled into the engine bytecode) | MIT |
| [mbedTLS](https://github.com/Mbed-TLS/mbedtls) 3.6 | TLS 1.2, AES-GCM, SHA, RSA/ECDH (engine and plugin) | Apache-2.0 OR GPL-2.0-or-later |
| [TweetNaCl](https://tweetnacl.cr.yp.to/)-derived code | curve25519 / XEdDSA (`engine/src/curve25519.c`) | Public domain |
| [libopus](https://opus-codec.org/), libogg | Voice message decoding | BSD-3-Clause |
| libjpeg-turbo, libpng, [libwebp](https://developers.google.com/speed/webp) | Image conversion | IJG/BSD, libpng license, BSD-3-Clause |
| zlib | Compression | zlib license |
| [VitaShell](https://github.com/TheOfficialFloW/VitaShell) (package preparation) | VPK installer helper | GPL-3.0 |
| [FFmpeg-vita](https://github.com/fish47/FFmpeg-vita) | Reference for the hardware video decoder | LGPL/GPL (reference only, no code bundled) |
| Roboto | Font | Apache-2.0 |
| Noto Sans CJK, Noto Color Emoji | Fonts | SIL OFL 1.1 |

Baileys copyright: Copyright (c) 2025 Rajeh Taher/WhiskeySockets (MIT). QuickJS copyright: Copyright (c) 2017-2025 Fabrice Bellard, Charlie Gordon (MIT).
