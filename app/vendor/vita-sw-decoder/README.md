# vita-sw-decoder

Plug-and-play CPU H.264 player backend with optional AAC audio for PlayStation Vita. Video uses
FFmpeg's software H.264 decoder with Vita-tuned pthread workers; AAC output uses
the public Vita audio decoder. It never loads ReAvPlayer and never selects
`h264_vita`.

<p align="center">
  <img src="screenshots/software-decoder.jpeg" alt="Software H.264 decoding on PlayStation Vita">
</p>
<p align="center"><sub>FFmpeg software H.264 playback on PlayStation Vita</sub></p>

## 2.0 ABI migration

Version 2 adds `open_with_cancel` to `VitaSwDecoderStreamFactory` and the
non-owning `abort` callback to `VitaSwDecoderStreamHandle`. These additions also
change the layout of `VitaSwDecoderPlayerConfig`. Rebuild every consumer against
the 2.x header and zero- or designated-initialize every public structure,
including each handle returned by a factory; do not reuse a 1.x binary or leave
a stack-allocated factory/handle partially initialized.

```c
VitaSwDecoderStreamFactory source = {0};
VitaSwDecoderPlayerConfig config = {0};
```

## Integrate in three steps

```sh
export VITASDK=/path/to/vitasdk
./tools/build-ffmpeg.sh
```

```cmake
set(VITA_SW_DECODER_FFMPEG_ROOT "/path/to/vita-sw-decoder/build/deps/ffmpeg-vita-sw")
add_subdirectory(external/vita-sw-decoder)
target_link_libraries(my_app PRIVATE VitaSwDecoder::VitaSwDecoder)
```

```c
#include <vita_sw_decoder.h>

VitaSwDecoderStreamFactory source = {0};
vita_sw_decoder_file_stream_factory("ux0:video/movie.mp4", &source);
VitaSwDecoderPlayerConfig config = { .stream = source, .volume_percent = 100 };
VitaSwDecoderPlayer *player = vita_sw_decoder_create();
int result = vita_sw_decoder_open(player, &config);
```

The complete render loop is in `examples/local_file.c`.

## Current playback contract

- H.264 is decoded by FFmpeg on the CPU with Vita-tuned pthread workers; this
  package never loads or selects `h264_vita`. Copied U/V frames use the
  known-working online renderer's `YVU420P2` GXM sampling contract, and the retained display surface counts
  as available between video deadlines rather than reporting false buffering.
- MOV/MP4 and Matroska sources may contain optional AAC audio. The initial
  zero-based audio ordinal comes from `VitaSwDecoderPlayerConfig.audio_track`;
  an ordinal outside the playable AAC snapshot is clamped to track zero during
  open so stale preferences cannot silently disable audio.
- AAC demux/decode produces into a bounded eight-grain PCM ring. A dedicated
  1024-frame AudioOut consumer runs one scheduler priority above the video
  worker, so H.264, transport, and UI/GXM bursts cannot accumulate into audible
  starvation. The consumer alone publishes the played clock after AudioOut has
  accepted a grain; EOF is not exposed until every queued grain has drained.
  Shutdown diagnostics report submitted grains, delayed refills, and the
  maximum wall-clock refill gap.
- The already-open video demux snapshots up to 16 playable AAC tracks and 16
  embedded text-subtitle tracks (SubRip, ASS/SSA, WebVTT, MOV text, plain text,
  and MicroDVD) before decode starts. `vita_sw_decoder_*_track_count()` and
  `vita_sw_decoder_*_track_info()` expose each stream index, default flag,
  channel count, UTF-8 language/title, and codec name without opening a third
  probe cursor.
- `vita_sw_decoder_select_audio_track()` seeks the already-open audio demux
  cursor and restarts only its AAC/output pipeline at the supplied media
  timestamp. The live video pipeline, pause, and volume state are retained, and
  remote track changes do not reconnect or probe the container again. The
  replacement stays gated until its worker has retained an access unit at or
  after that timestamp. A known target at/after the stream's origin-normalized
  end is rejected before the live worker stops; unknown or sparse tails get a
  1.5-second readiness deadline. An empty/timed-out replacement rolls back to
  the prior track instead of committing a silent cursor.
- Initial playback and in-place seek restarts use that same bounded audio-start
  helper when incomplete AAC metadata requires a synchronous ADTS access-unit
  probe. A timed-out, transport-aborted seek follows the normal checked
  full-session recovery path instead of reusing the poisoned cursor.
- `start_position_ms`, seeking, stream-reported bitrate, duration, decoded/shown
  counters, and dropped-frame counters are exposed through the public config and
  status structures. Runtime seeks reuse the open demuxers and first seek the
  selected H.264 stream backward, preventing sparse subtitle, attachment, or
  cover tracks in Matroska from moving the demuxer to a later cluster. Both the
  video cursor and the independent audio cursor hop through that H.264 Cue index;
  the decoder never seeks the sparse AAC index or falls back to a multi-stream
  linear search. After the byte-level hop, each input exposes only its selected
  H.264 or AAC track while performing the bounded keyframe/interleave preroll.
  Startup and live seek diagnostics report video- and audio-demux time
  separately. They
  feed that preceding keyframe and its
  dependent packets into H.264, and discard only decoded preroll pictures before
  publishing from the requested timestamp. Compressed reference packets are
  never discarded merely because their PTS precedes a backward-seek target.
  Only the decode pipelines are
  rebuilt; after the owner fences GXM and calls
  `vita_sw_decoder_prepare_background_restart()`, the copied surfaces follow a
  conservative full teardown/reallocation lifecycle so no stale GXM or AVFrame
  ownership crosses the seek. A normal close keeps the conservative render
  wait, and a full reopen remains
  the recovery fallback for non-cancellation failures. A seek at the reported
  duration is clamped inside the video-stream end by at least 250 ms so the final
  GOP cannot be mistaken for a broken empty startup. Audio is released after the
  first decoded video frame. Startup
  requires a real picture within a six-second CPU budget rather than publishing
  an audio-only session with an endless Preparing video state. Seek first-frame
  waits use a four-second CPU bound and fail cleanly if no picture is produced.
  A live seek also rejects a first ready frame more than two seconds beyond the
  requested clock. Startup, resume, live seek, and audio-track replacement
  retain the first AAC timestamp and require the first audio and video landings
  to agree within 250 ms before the shared start gate opens. Invalid landings
  invoke the checked fresh-session recovery instead of committing offset audio
  or a black screen. If a selected audio stream reaches clean EOF before the
  video, clock ownership moves to the monotonic presentation clock without
  clearing the selected audio pipeline, so position remains continuous, the
  video tail drains, and a later seek can restart that same track. Hidden or
  energy-saving playback can call `vita_sw_decoder_discard_video_to_clock()`
  after fencing GXM to keep the decode queue draining without drawing.
- Stream discovery runs within the configured probe/analyse bounds whenever
  the required H.264/AAC parameters are incomplete, including for indexed MP4
  and Matroska inputs; complete indexed inputs keep the fast path. If an initial
  saved-position seek fails or
  points at/past the reported end, playback performs a checked restart at zero
  rather than decoding linearly through a long file.
- A stream factory can represent a Vita file or an authenticated remote reader,
  but every open call must return a fresh readable, seekable cursor. Existing
  factories can keep using `open`; an optional `open_with_cancel` callback
  receives the per-operation flag so a blocked remote connection/read can stop
  promptly. A handle may additionally provide non-owning `abort` to wake a
  concurrently blocked cursor operation. The decoder calls it only while I/O is
  published active: ordinary seek/track restart first allows a shared 30 ms
  cooperative grace, while explicit stop/close interrupts immediately. It never
  uses `abort` as `close`, and reopens that cursor before a seek or track switch
  because transports may become unusable after abort. The callback must itself
  return promptly; the deadline selects cancellation time but cannot make a
  non-cooperative transport's blocked callback return.
- Caller-owned cancellation is never written or cleared by close. Cancellation
  also suppresses the expensive full-open recovery path; the embedding
  application can leave its loading screen immediately and decide what to do
  next. Audio rollback gets a fresh decoder-owned interrupt phase, so the flag
  that cancelled a replacement cannot also suppress restoration of the prior
  track. The UI delivers the decoder interrupt before publishing its caller
  flag, so a forward cancellation cannot arrive late in rollback; a later
  interrupt or session stop can still terminate a blocked restore. A
  replacement remains gated until
  pause/volume state is installed and its operation-only transport interrupt
  has been cleared. If the new track fails but rollback succeeds, the call
  returns `VITA_SW_DECODER_AUDIO_CHANGE_ROLLED_BACK` and the previous track
  remains live.
- Concise timing records identify factory open, FFmpeg header/probe, startup
  first-frame wait, demux seek, worker join/restart, audio-switch latency, and
  AudioOut refill cadence in the module log for on-device performance checks.

VitaMediaDeck can consume the decoder's immutable track-metadata snapshot while
layering embedded text-subtitle packet demux, cover/frame previews, resume
history, and player UI above this package.

The current VitaMediaDeck integration uses the same H.264 cue anchor for an
initial saved-position resume, a live seek, and AAC track replacement. Subtitle
and thumbnail cursors remain app-owned and use independent cancellation
generations, so restarting grid-cover work cannot invalidate the active media
seek or playback cursor.

## Use both backends in one app

`vita-sw-decoder` and `vita-hw-decoder` use the same lifecycle and stream
contract but have distinct symbols and CMake targets, so they can be linked
together. An application can try the hardware package first, destroy that
session after a non-cancellation failure, then reopen the same stream factory
with `vita_sw_decoder_open()`. A cancellation result must be returned directly.
`vita_sw_decoder_backend_name()` returns `software`.
`VitaSwDecoderPlayerStatus.video_bitrate_bps` exposes the H.264 stream bitrate
reported by FFmpeg, then the container bitrate. When Matroska advertises neither,
the decoder derives an average from the seekable file size and duration instead
of returning an empty HUD value.
`VitaSwDecoderPlayerConfig.audio_track` selects a zero-based AAC stream
ordinal, and `vita_sw_decoder_select_audio_track()` changes it at the supplied
playback timestamp without interrupting the video decoder or losing pause and
volume state.

The source factory creates two independent seekable cursors (audio and video).
Session startup currently opens and probes the video cursor first so it can
snapshot/select tracks, then opens and probes the audio cursor. These stages are
intentionally serialized because the factory contract guarantees independent
cursors but does not guarantee that concurrent factory calls are thread-safe.
It therefore works with local files and with remote Range readers. Supported
content is a seekable container recognized by the pinned FFmpeg build with H.264
video with optional AAC audio in a MOV/MP4 or Matroska container. CPU decoding is deliberately a compatibility path;
resolution and frame-rate limits must be measured on hardware.

## Install and consume

```sh
cmake -S . -B build/package \
  -DVITA_SW_DECODER_FFMPEG_ROOT="$PWD/build/deps/ffmpeg-vita-sw"
cmake --build build/package
cmake --install build/package --prefix "$PWD/build/stage"
```

Installed consumers may use `find_package(VitaSwDecoder CONFIG REQUIRED)` and
link `VitaSwDecoder::VitaSwDecoder`. The installed package carries its pinned
FFmpeg static archives, license text and corresponding source.

## Related repositories

| Repository | Relationship |
| --- | --- |
| [`VitaMediaDeck`](https://github.com/spyro-98/VitaMediaDeck) | Main application and hardware-first/software-fallback orchestrator |
| [`vita-hw-decoder`](https://github.com/spyro-98/vita-hw-decoder) | API-compatible hardware H.264 backend |
| [`vita-https`](https://github.com/spyro-98/vita-https) | Hardened seekable HTTPS Range source used by WebDAV playback |
| [`VitaMediaDeck-Transcoder`](https://github.com/spyro-98/VitaMediaDeck-Transcoder) | Optional desktop producer of compatible H.264/AAC Matroska files |

Licensed GPL-3.0-only. See `THIRD_PARTY_NOTICES.md` for FFmpeg and VitaSDK
requirements.
