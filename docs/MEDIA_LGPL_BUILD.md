# Folderwing minimal LGPL media build

The dependency recipe under `tools/media-lgpl/` builds a replaceable Windows x64
libmpv DLL from exact sources. Folderwing's own code remains MIT. The DLL's
selected combined license is LGPL-3.0-or-later; individual components retain their
notices and compatible terms. This is dependency-build tooling, not release CI.

## Inputs and isolation

- `sources.json`: full commits, original archive URLs, sizes and SHA256 hashes
  for mpv, FFmpeg, codecs, fonts/subtitles, shader/compiler dependencies.
- `headers.json`: the exact Vulkan headers referenced by libplacebo's gitlink,
  needed for public/stub types even with Vulkan rendering disabled.
- `toolchain.json`: Ubuntu base image digest and the exact additional compiler,
  headers, assembler, build-tool and Python package versions/URLs/hashes.
- `toolchain-sources.json`: the matching MinGW runtime source package and Ubuntu
  packaging patches. Runtime copyright/exception notices accompany the DLL.
- `Dockerfile.locked`: reconstructs the toolchain from the locked package files
  without network access. `Dockerfile` documents the initial bootstrap only;
  it is not the locked rebuild path.

The build runs only in its owned container, with a four-CPU / 7 GiB memory budget
and no network access. It does not stop WSL, Docker, or other containers. Cached
steps retain source/configuration keys and original configure commands.

## Build

Choose a new owned Linux work directory. A Linux filesystem is recommended for
compilation. Example from the Folderwing repository:

```sh
work="$HOME/.cache/folderwing/media-lgpl/my-build"
python3 tools/media-lgpl/acquire.py --work-directory "$work"
mkdir -p "$work/toolchain-context"
python3 tools/media-lgpl/fetch-toolchain.py --directory "$work/toolchain-context/toolchain-debs"
cp tools/media-lgpl/Dockerfile.locked "$work/toolchain-context/Dockerfile"
docker build -t folderwing-media-locked "$work/toolchain-context"
cp tools/media-lgpl/build.sh "$work/build.sh"
docker run --rm --network none --cpus 4 --memory 7g --memory-swap 9g \
  --mount "type=bind,source=$work,target=/work" \
  folderwing-media-locked bash /work/build.sh
# Preserve package/notice records in the same locked builder before packaging.
docker run --rm --network none --mount "type=bind,source=$work,target=/work" \
  --mount "type=bind,source=$PWD/tools/media-lgpl,target=/recipe,readonly" \
  folderwing-media-locked python3 /recipe/capture-toolchain.py
python3 tools/media-lgpl/package.py --work-directory "$work" \
  --output-directory build/media-lgpl-package
```

`acquire.py --offline` accepts only already present matching source archives.
No branch/tag/submodule revision is resolved during a rebuild. The library build
itself cannot fetch missing sources from the network. Use a fresh source work
folder when validating a changed compiler or configuration; cached intermediates
are development conveniences, not independent rebuild evidence.

## Selected features

Media Foundation remains the first backend. libmpv remains the fallback and uses
the existing client/D3D11 render API. The source revision is kept equal to the
previous dependency's mpv/FFmpeg baseline to limit compatibility changes.

The replacement disables GPL/nonfree media features, scripting, discs/Java,
network media, encoders/muxers, capture devices and unrelated render outputs.
It retains local-file demuxing/decoding, native FFmpeg codecs, dav1d AV1, Opus,
D3D11/DXVA2 decoding and D3D11 rendering/WASAPI. mpv still requires libass,
FreeType, HarfBuzz, FriBidi, libplacebo and shader dependencies; these are pinned
and their sources/notices are included. FreeType uses its FTL license.

The package checker rejects GPL/nonfree configurations and unexpected compiler-
or codec-runtime DLL imports. It records active mpv options and all source pins.
The helper accepts absent `load-scripts`, `osc` and `ytdl` controls only when
mpv reports that those compile-time script options do not exist. Errors for
configuration, file-reference and other controls remain fatal.

Static configuration and import checks are followed by actual rendering/media
acceptance; they do not substitute for playback evidence.

## Packages and restoration

The packager creates:

- `folderwing-media-lgpl-20261025.zip`: DLL, matching public headers, full prepared
  component notices, build metadata and a corresponding-source pointer.
- `folderwing-media-lgpl-sources-20261025.tar.gz`: exact original source archives,
  runtime sources/patches, recipes, locks, notices and rebuild instructions.
- `media-dependencies.json`: a proposed manifest with exact runtime/package hashes.

Keep the corresponding source archive available beside any public binary
release. Toolchain `.deb` files are local build inputs, excluded from the app and
source packages; their exact upstream URLs/hashes allow reconstruction. This
provides pinned inputs; a second-machine byte-for-byte rebuild is not claimed.

`tools/get-media.ps1 -ArchivePath <verified ZIP>` restores only the pinned bundle.
It also accepts `FOLDERWING_MEDIA_ARCHIVE` or the matching file under
`build/dependencies/`. It preserves the previous dependency directory during
replacement. No GPL fallback or changing upstream "latest" binary is accepted.
The DLL remains user-replaceable at runtime; acquisition-time verification does
not impose a runtime hash/signature restriction.

`tools/copy-media-assets.ps1` publishes every manifest-listed runtime notice with
the DLL. Build outputs are inputs to a later explicit release packager, rather
than an instruction to distribute the whole development directory.

## Focused acceptance

Generate synthetic fixtures using `tests/make_media_fixtures.py` first; see
[RELEASING.md](RELEASING.md). Then
`tests/media_preview.ps1 -BuildDirectory <candidate> -ResultDirectory <fresh> -FixtureDirectory <fixtures> -Audio`
runs the codec, preview/control, real host and real audio lanes. The codec lane
forces eight H.264/HEVC/AV1/VP8/VP9/MPEG-4 video/container fixtures through actual
libmpv D3D11 rendering, including colored-pixel checks, playback, seeking,
immediate close and reopening. A separate probe forces software HEVC/AV1/VP9
decoding without audio output; D3D11 pixel checks are covered by the main matrix.
The other lanes check default MF routing,
restricted fallback helpers, failures, source replacement, archive/WSL integration
and a real audio endpoint. All fixtures are owned; WSL stays running.

The audio lane must report an unavailable endpoint if the monitor/speaker is
asleep; a null sink is not substituted for an actual playback-device check.
