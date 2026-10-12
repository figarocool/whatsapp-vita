#!/usr/bin/env bash
set -euo pipefail

: "${VITASDK:?Set VITASDK before running this script}"

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
ffmpeg_version=8.1.2
ffmpeg_sha256=464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c
ffmpeg_url="https://ffmpeg.org/releases/ffmpeg-${ffmpeg_version}.tar.xz"
prefix="${VITA_SW_DECODER_FFMPEG_ROOT:-$repo_root/build/deps/ffmpeg-vita-sw}"
jobs="${VITA_SW_DECODER_FFMPEG_JOBS:-4}"
work="$(mktemp -d "${TMPDIR:-/tmp}/vita-sw-decoder-ffmpeg.XXXXXX")"
trap 'rm -rf "$work"' EXIT

curl -L --fail --max-time 180 "$ffmpeg_url" -o "$work/ffmpeg.tar.xz"
printf '%s  %s\n' "$ffmpeg_sha256" "$work/ffmpeg.tar.xz" | shasum -a 256 -c -
tar -xf "$work/ffmpeg.tar.xz" -C "$work"
cd "$work/ffmpeg-${ffmpeg_version}"

./configure \
  --prefix="$prefix" \
  --enable-cross-compile \
  --cross-prefix="$VITASDK/bin/arm-vita-eabi-" \
  --ar="$VITASDK/bin/arm-vita-eabi-gcc-ar" \
  --ranlib="$VITASDK/bin/arm-vita-eabi-gcc-ranlib" \
  --nm="$VITASDK/bin/arm-vita-eabi-gcc-nm" \
  --disable-shared --enable-static --disable-runtime-cpudetect \
  --disable-programs --disable-doc --disable-network --disable-everything \
  --enable-decoder=h264 --enable-demuxer=mov,matroska,mpegts \
  --enable-parser=aac,h264 --enable-protocol=file \
  --disable-debug --enable-hardcoded-tables \
  --arch=armv7-a --cpu=cortex-a9 --target-os=none \
  --disable-armv5te --disable-armv6t2 --enable-pthreads \
  --disable-bzlib --disable-iconv --disable-lzma --disable-sdl2 \
  --disable-securetransport --disable-xlib \
  --optflags='-O3 -DNDEBUG -ftree-vectorize -funroll-loops -fomit-frame-pointer' \
  --extra-cflags='-std=gnu11 -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -Wl,-q -ffast-math -D_BSD_SOURCE' \
  --extra-ldflags="-L$VITASDK/lib"

make -j"$jobs"
make install
mkdir -p "$prefix/share/licenses" "$prefix/share/sources"
install -m 0644 COPYING.LGPLv2.1 "$prefix/share/licenses/FFmpeg-LGPL-2.1.txt"
install -m 0644 "$work/ffmpeg.tar.xz" \
  "$prefix/share/sources/ffmpeg-${ffmpeg_version}.tar.xz"

"$VITASDK/bin/arm-vita-eabi-gcc-nm" -g --defined-only \
  "$prefix/lib/libavcodec.a" | grep 'ff_h264_decoder' >/dev/null
echo "FFmpeg software H.264 is ready at $prefix"
