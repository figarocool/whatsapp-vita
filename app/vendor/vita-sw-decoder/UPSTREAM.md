Vendored from https://github.com/spyro-98/vita-sw-decoder
at commit 00c497d5474ad2f2693183f5e399a378aea1b128 (GPL-3.0).
The decoder source is unmodified. The FFmpeg builder disables LTO because
VitaSDK GCC 10.3.0 crashes during the application's final LTO link.
The CMake target links zlib after FFmpeg (required by the MOV demuxer).
WhatsApp integration lives in
app/src/software_player.c. Source/build artifacts under build/ are ignored.

FFmpeg 8.1.2 is downloaded from ffmpeg.org by tools/build-ffmpeg.sh and
verified against its pinned SHA-256; the configured decoder is software H.264.
