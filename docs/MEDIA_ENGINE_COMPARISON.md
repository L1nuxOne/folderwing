# Media preview engine comparison — 2026-10-23

Status: standalone prototypes compared; production engine selection and integration
remain pending. The current release is unchanged. WSL and Docker were not stopped.

The implementation lives in [tools/media-prototype](../tools/media-prototype/README.md).
The retained cohort is `build/media_prototype/20261023/`, with exact source/binary
inputs, dependency provenance, synthetic fixtures, raw telemetry, process receipts
and rendered post-seek snapshots.

## What was built

One small native host runs either IMFMediaEngine (Media Foundation) or dynamically
loaded libmpv, with the same D3D11 device, output texture, 640×360 size and frame
readback oracle. Each backend renders on its own thread; commands stay on the host
thread. Audio-only mode creates no D3D device. Both prototypes have play/pause,
relative seeking, mute and volume controls; duration and source dimensions are
collected programmatically. Post-seek frame capture demonstrates a usable image
output path; thumbnail scheduling/caching is not implemented.

The combined host is 95,744 bytes. Media Foundation adds no bundled engine DLL;
its OS libraries/codecs are dependencies. This pinned libmpv build adds a
121,108,480-byte DLL (115.5 MiB). Its development archive is 31,622,190 bytes
(30.2 MiB), including headers/import library; this is not a measured installer size.
The ffmpeg executable is fixture-generation tooling, not a runtime dependency of
the prototype. Audio-only libmpv uses much less private memory than its DLL's
on-disk size; those quantities must not be confused.

The libmpv version is `v0.41.0-1227-ge471496c5`, from shinchiro's 20261021 build.
Its D3D11 render header was retrieved from that exact source commit because the
binary archive omitted it. Packaged client/render headers matched that commit.
This is a pinned development build, not a claim about the D3D11 embedding surface
in every stable libmpv release. The source, header and binary must stay matched.

## How to interpret the measurements

See [the complete measured tables](../build/media_prototype/20261023/MEASUREMENTS.md),
[CSV](../build/media_prototype/20261023/MEASUREMENTS.csv) and
[aggregate JSON](../build/media_prototype/20261023/SUMMARY.json).

Three fresh processes per engine and valid fixture, run serially with alternating
engine order. Files/OS/driver caches remain warm. Synthetic five-second clips cover
1280×720/30 fps video, a 3840×2160/60 fps H.264 clip, and six audio formats. The
second matrix uses losslessly remuxed video-only copies to remove audio endpoint
dependence. It is a separate workload; subtracting its times from the audio/video
matrix would not isolate audio cost because host load also changed.

First video frame means a successful backend render followed by non-black GPU
readback. It includes launch/initialization and the readback itself. It does not
measure DWM presentation or the user's first visible pixel. Audio startup means
loaded/ready, not first audible sample. Private bytes exclude dedicated VRAM;
working set and GPU engine activity are retained separately. Host load varied, and
three observations support descriptive medians/ranges rather than p95 or ideal
performance targets. CPU process-time counters are coarse; zero ticks do not mean
zero decoding cost.

Every supported run checks advancing playback, pause stability, volume/mute state,
seek to 60%, a new video frame after seek, resume, stream presence, duration and
video dimensions. Tests remain muted: no listening/loopback test is claimed. Hidden
windows avoid desktop input. All owned processes have bounded outer timeouts.

## Coverage and audio incident

On this Windows installation, without installing optional codecs:

| Fixture family | Media Foundation | libmpv |
|---|---|---|
| H.264/AAC MP4 and MKV | Playback and controls pass | Playback and controls pass |
| MPEG-4 Part 2 / PCM AVI | Pass | Pass |
| H.264/AAC 4K60 | Pass | Pass |
| HEVC/AAC, AV1/AAC, VP9/Opus, VP8/Vorbis | Codec-not-found, `0xc00d5212` | Pass |
| WAV PCM, MP3, AAC M4A, FLAC | Pass | Pass |
| Vorbis Ogg, Opus | Unsupported byte stream, `0xc00d36c4` | Pass |
| Owned corrupt MP4 | Explicit rejection | Explicit rejection |

These are fixture-level observations, not exhaustive codec/container guarantees.
Media Foundation support depends on installed decoders/container handlers; filename
extension alone cannot select a reliable backend. The installed codec-package
query and raw error HRESULTs are retained in the matrix evidence.

The first A/V matrix was interrupted when the test processes reported no playback
endpoint. It is retained as `comparison-1` with an explicit interrupted-environment
receipt and is excluded from accepted measurements. The user reported working LG
monitor audio. A read-only Core Audio probe subsequently found the LG HDR WQHD
endpoint active/default for all three roles, with the probe in the active console
session (1). A fresh MF WAV retry passed. No settings, audio services, drivers or
security policies were changed. The transient loss's cause was not established;
we do not attribute it to a separate desktop/session. The endpoint was confirmed before the accepted A/V rerun and again by an explicit
process probe after it. The attempted per-case endpoint guard did not return a
status under the inherited PATHEXT and produced no usable per-case endpoint logs;
those checks are not claimed. All accepted engine runs independently passed
playback advancement and controls without audio-sink errors. The guard now uses
explicit ProcessStartInfo and its read-only execution is verified separately. Both
engines successfully initialize audio while muted; physical audibility was not tested.

## Measured highlights

Medians from the accepted A/V matrix (three runs per engine; ranges in the full ledger):

| Operation / cost | Media Foundation | libmpv |
|---|---:|---:|
| H.264/AAC MP4 first verified frame | 238 ms | 406 ms |
| Same file, ready private memory | 93.5 MiB | 168.6 MiB |
| Same file, seek | 31.6 ms | 23.5 ms |
| 4K60 H.264/AAC first frame | 322 ms | 456 ms |
| Same 4K file, private memory | 371 MiB | 540 MiB |
| Same 4K file, seek | 204 ms | 66.6 ms |
| WAV ready | 109 ms | 106 ms |
| WAV private memory | 4.3 MiB | 20.7 MiB |
| WAV engine shutdown | 204 ms | 2.3 ms |
| Valid fixture formats passed | 8 / 14 | 14 / 14 |

Audio startup is competitive in both engines; libmpv's MP3/AAC/FLAC medians are
lower here despite its higher private memory. Libmpv also seeks faster in the
matched supported workloads. This is not a universal MF latency win. The 4K
checks prove rendering and control behavior, not sustained 60 fps or zero drops.

The two accepted matrices contain 134 runs: 102 supported playback/control passes
and 32 explicit unsupported-format/corrupt-input rejections, with no outer timeout.
The A/V controller encountered an inherited-PATHEXT native-pipeline error only
while writing its final endpoint receipt, after all 86 samples completed. A
separate explicit-process endpoint probe and unchanged-input check finalized that
receipt without replaying samples; its original controller exit remains recorded.
The controller's native probe invocation is corrected for future runs. The exact executed
versions are retained in `comparison-2-inputs/` and `video-comparison-1-inputs/`.

## Decision

Media Foundation is the lower-footprint path for the common formats tested here.
Libmpv provides substantially broader coverage on the same unmodified Windows
installation. Its H.264, HEVC, AV1 and VP9 runs report `d3d11va`; own-process video
engine counters also show activity. MF receives the hardware D3D11 device and shows
video-engine activity, but IMFMediaEngine does not expose an equivalent decoder
name in this prototype. VP8/MPEG-4 software decoding remains usable in libmpv.

Recommended product direction: Media Foundation first, with libmpv loaded lazily
when the OS backend cannot decode the selected media. This follows the app's
lightweight common-path approach while retaining broad preview coverage. It adds
a second backend lifecycle and a fallback transition; choose libmpv alone instead
if one consistent decoder stack is more valuable than the measured common-format
startup/memory savings. No engine has been integrated automatically by this work.

For a dual backend, retry only a classified unsupported-source/codec failure, not
an audio endpoint outage or every arbitrary error. Keep both engines in the
on-demand media helper, and preserve one UI/control contract. Release each media
source before switching backend; avoid audible restart and preserve requested
position/pause/volume. A codec-aware result must drive fallback rather than a
hardcoded filename-extension map.

Feature work after selection: integrate the helper/pane contract; add seek bar,
duration/metadata display and cached thumbnails; define Windows/WSL input staging
and cancellation. This experiment does not claim those flows, arbitrary malformed
media hardening, HDR/color management, subtitles, long-duration playback, device
hotplug recovery or production IPC/lifecycle acceptance. Choose/pin the packaging
and licensing of the exact libmpv build before redistribution.

## Upstream references

- [Media Foundation formats](https://learn.microsoft.com/en-us/windows/win32/medfound/supported-media-formats-in-media-foundation)
- [MF frame-server tick and frame transfer](https://learn.microsoft.com/en-us/windows/win32/api/mfmediaengine/nf-mfmediaengine-imfmediaengine-onvideostreamtick)
- [mpv Windows build providers](https://mpv.io/installation/)
- [Exact prototype D3D11 render header](https://github.com/mpv-player/mpv/blob/e471496c56/include/mpv/render_d3d11.h)
- [mpv build/licensing terms](https://github.com/mpv-player/mpv/blob/e471496c56/Copyright)
