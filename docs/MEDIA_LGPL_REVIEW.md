# libmpv LGPL distribution review

Historical pre-build investigation. The selected pinned LGPL build is now
documented in [MEDIA_LGPL_BUILD.md](MEDIA_LGPL_BUILD.md). References below to
"current" describe the earlier shinchiro dependency, preserved for provenance.

Reviewed 2026-10-25. Folderwing's own code remains MIT. This review prepares the
media dependency decision; it does not replace the installed backend or certify
a public release. Compact, hashed-build evidence and candidate metadata are in
[dependencies/media-review.json](dependencies/media-review.json).

## Current build: GPL features are confirmed in its build provenance

`tools/media-dependencies.json` pins shinchiro's
`mpv-dev-x86_64-20261021-git-e471496c56.7z`:

- Archive SHA256: `efc719eff3dc5954b4a5efc800632808bcec01cc9034940dd14e5eb7fe9ea633`.
- DLL SHA256: `04e4d27dc385dc18ddd93fbe69c75e635b864f3c6ec53a9122df54c2f7842f4a`.
- DLL size: 121,108,480 bytes.
- The archive digest matches GitHub's release-asset digest.
- The release links successful workflow run `39518569492`; its head matches the
  release tag's recipe commit `1f8d2e76255abc48f0456d9be99b4cab9c6566e6`.
- That exact FFmpeg recipe enables `--enable-gpl`, `--enable-version3`, x264,
  DVD libraries and rubberband. The mpv recipe enables GPL features and does not
  select `-Dgpl=false`.

This makes the current build unsuitable for our planned LGPL-only media
package. This conclusion comes from matching release/build provenance. The
static binary string scan did not recover FFmpeg's configure/license strings;
we do not claim to have queried the binary's runtime license functions.
A source-tree link and the existing small `SOURCE.txt` are not a complete
corresponding-source distribution.

Sources: [release](https://github.com/shinchiro/mpv-winbuild-cmake/releases/tag/20261021),
[workflow](https://github.com/shinchiro/mpv-winbuild-cmake/actions/runs/39518569492),
[exact FFmpeg recipe](https://github.com/shinchiro/mpv-winbuild-cmake/blob/1f8d2e76255abc48f0456d9be99b4cab9c6566e6/packages/ffmpeg.cmake),
[exact mpv recipe](https://github.com/shinchiro/mpv-winbuild-cmake/blob/1f8d2e76255abc48f0456d9be99b4cab9c6566e6/packages/mpv.cmake).

## An LGPL candidate exists and preserves our header interface

Inspected the upstream mpv development build:

`libmpv-v0.41.0-dev-g703e59b0e-40117195812-x86_64-w64-mingw32-lgpl.zip`

- mpv commit: `703e59b0ecf659ba8062ecb876fb8ec25f73726f`.
- Archive SHA256 verified against GitHub:
  `4c9d0e3ff428cb12483d0d27736a17d7e14d161024052f526e90f447ef20b452`.
- DLL SHA256:
  `812165f8a581ad4cd21cd5c9547f2f5e6ee2c6d7511268e27cd60ea1f5d85cd1`.
- DLL size: 127,277,056 bytes. This candidate is not smaller than our current DLL.
- `client.h`, `render.h` and `render_d3d11.h` match the installed headers byte for
  byte. All 16 mpv exports requested by `app/mediaengine.cpp` are present.
- The recipe explicitly uses `-Dgpl=false` for this variant and enables D3D11,
  shaderc and spirv-cross. D3D11 is not the legacy GPL Direct3D output disabled
  by LGPL mode.
- Static import inspection finds Windows/CRT API dependencies. No playback,
  renderer initialization or DLL-replacement experiment was performed.

This is an **upstream development candidate**, not a stable mpv release or a
compatibility guarantee. The rolling `git-release` tag and asset names must
never be used as an unpinned download. Preserve the verified bytes in a durable
release/dependency source archive if adopting this candidate.

Its ZIP has ten files: the DLL/import library, five headers, LGPL 2.1 license
text and two libbluray Java JARs. It has no matching source bundle or complete
transitive notices. The upstream release describes the combined LGPL build as
LGPLv3; include the applicable LGPLv3/GPLv3 texts and notices as well, rather
than relying on the ZIP's lone LGPL 2.1 text. Folderwing does not need those Java
JARs for ordinary local-file preview; exclude them unless deliberately supported
and inventoried.

The recipe uses a rolling BtbN LGPL container and shallow clones of additional
repositories. Therefore the mpv commit plus recipe alone does not reproduce the
exact DLL. Collect the actual container digest, all dependency source revisions,
patches, generated configuration and build commands before adopting it.

Sources: [upstream development release](https://github.com/mpv-player/mpv/releases/tag/git-release),
[exact build script](https://github.com/mpv-player/mpv/blob/703e59b0ecf659ba8062ecb876fb8ec25f73726f/ci/build-mingw64-full.sh),
[exact workflow](https://github.com/mpv-player/mpv/blob/703e59b0ecf659ba8062ecb876fb8ec25f73726f/.github/workflows/build.yml).

## Requirements for the replacement

1. **Select the intended combined license.** LGPLv3-or-later is a viable target;
   do not assume LGPLv2.1 merely because mpv itself supports it. FFmpeg's
   `--enable-version3` alone does not make a build GPL; its enabled code and
   dependency licenses control the result.
2. **Build mpv with `-Dgpl=false`.** Keep the public client/render API, libmpv DLL,
   D3D11 render backend and the shader dependencies it requires. Our helper's
   normal D3D11 path must continue to work.
3. **Keep FFmpeg GPL/nonfree components disabled.** Use `--disable-gpl` and
   `--disable-nonfree`, with a reviewed allowlist of external libraries. Exclude
   GPL-only x264/x265 encoders, DVD navigation and rubberband from this build.
   Removing encoder libraries does not by itself remove FFmpeg's native H.264
   and HEVC decoders; verify the needed decoder set in the actual configuration.
4. **Audit the whole embedded dependency set.** A dynamically loaded libmpv can
   contain statically linked FFmpeg and other libraries. Preserve each required
   license/notice and corresponding source; a DLL boundary is not an exemption.
5. **Freeze rebuildable inputs.** Record full source commits or source archives,
   checksums, compiler/toolchain and container digests, patches, configure/meson
   output and build scripts. Host corresponding sources beside downloadable
   binaries and retain them independently of rolling upstream releases.
6. **Preserve replacement/relinking rights.** The current app loads the named
   DLL through `LoadLibraryExW`; it does not reject replacement DLLs by a runtime
   hash or signature. Keep this property. Include source/build material needed
   for the copyleft components inside the DLL, applicable license texts and
   library attribution in the package/About/download page. Avoid terms that
   prohibit debugging modifications to the covered libraries.
7. **Run focused integration acceptance before adoption.** Verify all required
   API exports, D3D11 frame rendering, hardware/software decoding, fallback
   routing, representative audio/video playback and seek, cancellation/unload
   and bounded failure handling. Header/export checks alone cannot prove this.

Sources: [mpv licensing at the installed commit](https://github.com/mpv-player/mpv/blob/e471496c56/Copyright),
[FFmpeg licensing and distribution guidance](https://ffmpeg.org/legal.html),
[LGPL 3.0](https://www.gnu.org/licenses/lgpl-3.0.html).

## Recommended next implementation

Create a minimal, independently pinned LGPL-only dependency build and collect its
source/notices alongside the output. Keep Media Foundation as the existing
first backend. Use the inspected upstream candidate only if its exact source
closure can be recovered and retained; otherwise use it as a compatibility
reference while building our own.

A smaller optional-feature set can omit scripting, disc navigation, network
clients, Java helpers and unrelated render outputs that Folderwing does not use.
Retain local-file codec coverage and D3D11. This could reduce size and maintenance,
but no reduction or compatibility result is claimed before measuring the build.

No installed DLL, acquisition pin, app source or release workflow was changed.
WSL and Docker were left running. Raw review evidence and the inspected candidate
are retained locally under `build/dependency_review/20261025`.
