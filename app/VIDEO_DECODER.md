# Video playback

Build the dependency once with `bash tools/build_video_decoder.sh` from the
repository root, then configure/build the app normally. `WA_SOFTWARE_VIDEO`
defaults to ON; OFF builds the original system-only player.

SceAvPlayer remains the first choice. If opening fails or it produces no video
frame within four seconds, the client obtains the untouched original MP4 and
opens the CPU H.264 decoder on a cancellable background thread. It keeps normal
audio playback, fits the picture inside the viewer and restarts looping clips.
After the first software picture, a small cache marker remembers the working
backend for that video, avoiding another failed hardware start on re-opening.
Only presented frames are released after GXM has finished rendering. Leaving
the viewer cancels and joins the worker before destroying its session.

The fallback decodes H.264, including clips without audio. AAC audio uses
SceAudiodec. HEVC, VP9, AV1 and other audio codecs are not enabled by this build.
Frame rate for large/high frame-rate clips must be measured on the console;
software decoding does not guarantee full speed for every H.264 file.

Logs identify the backend, opening result, decoded/shown/dropped frames and
memory failures. Source files and upstream GPL/FFmpeg LGPL notices are included
in this repository; the VPK includes license notices. FFmpeg source and license
are preserved under the generated dependency's share/ directory.
