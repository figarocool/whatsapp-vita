#!/bin/sh
# Avvia l'app in Vita3K con un profilo isolato in /tmp (non tocca la tua libreria Vita3K).
# Uso: tools/run_vita3k.sh [percorso.vpk]
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VPK=${1:-$ROOT/app/build/whatsapp_vita.vpk}
EMU=/tmp/wa-vita3k/squashfs-root/usr/bin/Vita3K
BASE=/tmp/wa-emutest
SRC="$HOME/.local/share/Vita3K/Vita3K"
mkdir -p "$BASE/pref/ux0/data/whatsapp" "$BASE/pref/ux0/user" "$BASE/config" "$BASE/cache" "$BASE/data"
for d in os0 sa0 vs0 pd0 vd0 grw0 imc0 sd0 tm0 ud0 uma0 xmc0 host0; do [ -e "$BASE/pref/$d" ] || ln -s "$SRC/$d" "$BASE/pref/$d"; done
# ur0 scrivibile (copia): l'app e il plugin scrivono in ur0:tai
[ -d "$BASE/pref/ur0" ] || { rm -f "$BASE/pref/ur0"; cp -r "$SRC/ur0" "$BASE/pref/ur0"; }
[ -d "$BASE/pref/ux0/user/00" ] || cp -r "$SRC/ux0/user/." "$BASE/pref/ux0/user/" 2>/dev/null || true
sed -e "s#^pref-path:.*#pref-path: $BASE/pref/#" -e "s/^initial-setup:.*/initial-setup: true/" "$HOME/.config/Vita3K/config.yml" > "$BASE/config.yml"
rm -rf "$BASE/pref/ux0/app/WHAP00001"
echo "${WA_BRIDGE:-http://127.0.0.1:8787}" > "$BASE/pref/ux0/data/whatsapp/config.txt"
[ "${KEEP_STATE:-0}" = 1 ] || rm -f "$BASE/pref/ux0/data/whatsapp/log.txt" "$BASE/pref/ux0/data/whatsapp/running.lock"
# Vita3K richiede una glibc piu' recente di quella del sistema: gira in un container Docker (immagine wa-vita3k-env, vedi tools/Dockerfile.vita3k)
exec docker run --rm --network host --ipc=host --user "$(id -u):$(id -g)" \
  -e DISPLAY="$DISPLAY" -e HOME="$HOME" -e APPDIR=/tmp/wa-vita3k/squashfs-root -e LD_LIBRARY_PATH=/tmp/wa-vita3k/squashfs-root/usr/lib \
  -e XDG_CACHE_HOME="$BASE/cache" -e XDG_CONFIG_HOME="$BASE/config" -e XDG_DATA_HOME="$BASE/data" \
  -v /tmp/.X11-unix:/tmp/.X11-unix -v "$HOME/.Xauthority:$HOME/.Xauthority:ro" -e XAUTHORITY="$HOME/.Xauthority" \
  --device /dev/dri --group-add video --group-add render \
  -v /tmp/wa-vita3k:/tmp/wa-vita3k -v /tmp/wa-emutest:/tmp/wa-emutest -v "$SRC:$SRC:ro" -v "$(dirname "$VPK"):$(dirname "$VPK"):ro" \
  wa-vita3k-env "$EMU" --config-location "$BASE/config.yml" --load-config --keep-config "$VPK"
