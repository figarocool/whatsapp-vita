#!/bin/sh
# Compila e pubblica una release su GitHub (figarocool/whatsapp-vita).
# Uso: scripts/release.sh v1.0.1 "Note brevi della versione"
# Richiede VitaSDK ($VITASDK) e la CLI `gh` gia' autenticata.
set -eu
TAG=${1:?uso: scripts/release.sh vX.Y.Z "note"}
NOTES=${2:-}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/release-out/$TAG
rm -rf "$OUT" && mkdir -p "$OUT"
# 0) motore WhatsApp interno (bundle + bytecode)
"$ROOT/engine/build_engine.sh"
# 1) plugin
cmake -S "$ROOT/plugin" -B "$ROOT/plugin/build" >/dev/null && cmake --build "$ROOT/plugin/build" --target wa_notify.suprx-self
# 2) app, con la versione scritta dentro il pacchetto (assets/version.txt)
cmake -S "$ROOT/app" -B "$ROOT/app/build-release" -DWA_VERSION="$TAG" >/dev/null
cmake --build "$ROOT/app/build-release" --parallel 4
cp "$ROOT/app/build-release/whatsapp_vita.vpk" "$OUT/whatsapp-vita.vpk"
# la versione "sicura" (senza unsafe homebrew) non si pubblica: il motore interno non e' stato provato in quella modalita'
# 3) manifest con la firma SHA-256
python3 "$ROOT/scripts/create-release-manifest.py" "$TAG" "$OUT/whatsapp-vita.vpk" "$NOTES"
(cd "$OUT" && sha256sum whatsapp-vita.vpk whatsapp-vita-update.json > SHA256SUMS)
# 4) pubblicazione (con DRY_RUN=1 si ferma qui)
[ "${DRY_RUN:-0}" = 1 ] && { echo "Prova completata, nulla pubblicato: $OUT"; exit 0; }
gh release create "$TAG" -R figarocool/whatsapp-vita --title "WhatsApp Vita $TAG" --notes "${NOTES:-WhatsApp per PS Vita $TAG}" \
  "$OUT/whatsapp-vita.vpk" "$OUT/whatsapp-vita-update.json" "$OUT/SHA256SUMS"
echo "Pubblicata la release $TAG"
