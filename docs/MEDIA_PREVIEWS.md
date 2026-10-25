# Audio and video previews

Select a supported audio/video file to open a preview beside the source pane,
using the same pane replacement/restoration as PDF and image previews. Video
opens paused on its first frame; audio opens paused. Playback starts only when
requested. Leaving the selected media pauses it immediately; the paused preview
can remain for the configured linger interval. Close, source replacement and
feature disable retire the owned helper and source.

The first version includes play/pause, a draggable seek bar, ±5-second buttons,
volume and mute, elapsed/total time, audio/video stream presence and source video
dimensions. Buttons wrap into rows in narrow panes. Video poster thumbnails use
the existing byte-bounded thumbnail cache and separate muted helper jobs. They
never borrow or seek the live player's source. Archive video/audio entries can
use the third pane after the existing bounded extraction step; file operations
retain archive context rather than targeting the outer ZIP by accident. Leaving
the outer archive pauses its third-pane media immediately, including while the
archive layout lingers or a replacement preview waits for the existing debounce.
Returning to that archive does not resume playback automatically.

With focus in the preview:

| Key | Action |
|---|---|
| Space | Play / pause |
| Left / Right | Seek −5 / +5 seconds |
| Up / Down | Volume ±5 percent |
| M | Mute / unmute |
| Home | Seek to start |
| Tab / Shift+Tab | Move between controls and the other panes |
| Enter | Activate the focused button |
| Esc | Close the preview (nested: close only the third pane) |

The **Audio and video preview** function is enabled by default and can be disabled
in Settings. Its INI options are:

```ini
[functions]
preview.media = on

[preview.media]
linger = 5
volume = 25
muted = off
```

The initial volume is 25%. Previewing a file does not start sound. A missing
output is shown as unavailable; it does not trigger a different codec backend.
After waking an HDMI monitor, Retry a failed audio preview or select another file
and return. Seamless device-hotplug recovery is not claimed by this version.

## Engine and lifecycle

`folderwing-media.exe` is an on-demand native helper, started asynchronously with the same
restricted low-integrity token, inherited-handle allowlist and kill-on-close job
used for other previews. Its private-memory cap is 1536 MiB to allow 4K decoder
surfaces. The explorer does not link Media Foundation, D3D11 or libmpv. MF is the
default; libmpv is loaded from the helper's directory only for classified
unsupported source/codec errors. Access, timeout and audio-output errors do not
cause fallback. The MF source is an exact read-only file stream, preserving
extended Windows paths and literal Unicode/#/% filenames. The read handle allows
sharing for rename/delete and does not hold a write lock.

Decoding/rendering and blocking source calls stay outside the explorer. Pixels
cross a bounded shared section: two 1280×720 BGRA banks, with at most one frame
outstanding until the explorer copies it and acknowledges its ticket. Frame
publication is limited to roughly 30 Hz; this is a preview, not a claim
of full-resolution/60 Hz playback. Presentation scales the copied frame to the
pane. Source and launch generations reject stale replies. Client writes use the
existing asynchronous bounded writer. A stalled worker is killed by the preview
watchdog, with Retry available. Per-source disposal also unloads fallback codecs.

The fallback now selects the pinned Folderwing LGPL-only source build. Its mpv
client/D3D11 headers retain the same baseline API. `tools/get-media.ps1` verifies
the complete runtime/header/notice set from `tools/media-dependencies.json`;
normal and focused builds copy every manifest-listed runtime notice. Exact
source archives, compiler locks, configuration and rebuild instructions accompany
the local dependency bundle. See [the build guide](MEDIA_LGPL_BUILD.md). Public
release packaging must keep the matching corresponding-source archive available
beside the binary download.

Normal Windows files and ordinary WSL UNC paths are supported. WSL media routing
is disabled with `wsl.helper`; no Linux helper is loaded solely for Windows media.
Known Unix FIFOs, sockets, devices and symlinks are excluded. Linux names requiring
the engine's escaped-byte transport are currently excluded rather than decoded
through an inexact Windows path. Reparse/offline/device inputs are refused by the
worker. URLs, playlists and external reference loading are not media-preview
entry points; libmpv config/scripts, automatic sidecar loading and reference
following are disabled.

## Focused verification and acceptance

```powershell
./tests/media_preview.ps1 -BuildDirectory ./build/release
# Include the audio lane with an active playback endpoint:
./tests/media_preview.ps1 -BuildDirectory ./build/release -Audio
```

The independent video lane uses synthetic clips with **zero audio tracks**; it
checks MF/fallback selection, rendered color pixels, keyboard controls, seeks,
volume state, lazy DLL loading, the low-integrity token, crash recovery and source
replacement. The real-host lane checks pane routing/restoration, linger pause,
Unicode filenames, archive third-pane scope, actual poster cache behavior, native
WSL video and FIFO exclusion, and feature-disable cleanup.

The separate `media_audio_tests.exe` lane passed on 2026-10-23 after the LG
monitor woke: 10 checks cover MF WAV playback/pause, libmpv Opus playback/seek/pause,
volume state, low integrity, and audio/video streams in MP4 and WebM. The LG HDMI
endpoint was active/default before and after the run. The exact published runtime
was tested without a code change or rebuild. The feature now has 245 focused
checks passed, including the earlier media/host and archive-regression checks;
no prepared media selector remains deferred.

Automated playback stays muted and does not establish physical speaker audibility.
The native registry classifies/binds all three executables. The original deferred
receipt remains historical evidence; the audio addendum is retained under
`build/media_preview/20261023/audio-acceptance-1/`. No full application suite or new
performance audit is claimed for this feature batch.
