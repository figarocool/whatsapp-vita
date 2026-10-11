#!/bin/sh
# Prepara il motore per l'app: impacchetta Baileys + bridge in un file JS, poi lo compila in bytecode (.qbc).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
( cd "$HERE/js" && [ -d node_modules ] || npm i esbuild events util buffer path-browserify readable-stream@4 string_decoder process abort-controller >/dev/null 2>&1 )
( cd "$HERE/js" && ENTRY=host/bridge_main.js node build.mjs bridge.min.js --min )
[ -x "$HERE/build/qjsbc" ] || { cmake -S "$HERE" -B "$HERE/build" >/dev/null && cmake --build "$HERE/build" --target qjsbc >/dev/null; }
"$HERE/build/qjsbc" "$HERE/js/bridge.min.js" "$HERE/js/bridge.min.qbc"
