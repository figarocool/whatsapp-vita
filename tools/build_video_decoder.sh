#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
export VITASDK="${VITASDK:-/usr/local/vitasdk}"
exec bash "$repo_root/app/vendor/vita-sw-decoder/tools/build-ffmpeg.sh"
