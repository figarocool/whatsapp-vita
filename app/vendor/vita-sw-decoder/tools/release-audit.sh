#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
: "${VITASDK:?Set VITASDK before auditing}"
ffmpeg="${VITA_SW_DECODER_FFMPEG_ROOT:-$repo_root/build/deps/ffmpeg-vita-sw}"
for file in "$repo_root/LICENSE" "$repo_root/THIRD_PARTY_NOTICES.md" \
  "$repo_root/DEPENDENCIES.lock" \
  "$ffmpeg/share/licenses/FFmpeg-LGPL-2.1.txt" \
  "$ffmpeg/share/sources/ffmpeg-8.1.2.tar.xz"; do
  [[ -f "$file" ]] || { echo "Missing $file" >&2; exit 1; }
done
nm_output="$(mktemp "${TMPDIR:-/tmp}/vita-sw-nm.XXXXXX")"
trap 'rm -f "$nm_output"' EXIT
"$VITASDK/bin/arm-vita-eabi-gcc-nm" -g --defined-only \
  "$ffmpeg/lib/libavcodec.a" > "$nm_output"
grep -q 'ff_h264_decoder' "$nm_output" || { echo "software H.264 is missing" >&2; exit 1; }
if grep -q 'h264_vita' "$nm_output"; then
  echo "hardware H.264 leaked into the software-only package" >&2; exit 1
fi
echo "vita-sw-decoder release audit passed"
