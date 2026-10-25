# Folderwing dependency inventory

Reviewed 2026-10-25. Folderwing's own code remains **MIT**, as selected for the
public project. [LICENSE](../LICENSE) and its existing attribution are preserved.
Third-party components retain their own terms.

The machine-readable snapshot is [tools/dependency-inventory.json](../tools/dependency-inventory.json).
It records 31 runtime/header/toolchain component entries, five build-tool entries,
source/archive pins, observed binary/library hashes, 129 active notice files,
optional integration boundaries, and explicit remaining work. It is a release
preparation inventory, **not a formal SPDX SBOM or public-release
clearance**. Build inputs and notice copies are hashed so an update can identify
which dependency decision needs revisiting.

## Runtime and embedded components

| Component | Current version | Where it is used | License basis / packaging status |
|---|---|---|---|
| miniz | 3.1.2 | ZIP helper, static | MIT; archive notice already copied by development build |
| libarchive | 3.8.10 | General archive helper, static | BSD-style distribution with file-level exceptions; preserve complete notice |
| zlib | 1.3.2 | General archive helper, static | zlib license; notice already copied |
| bzip2 | 1.0.8 | General archive helper, static | bzip2 license; notice already copied |
| liblzma | XZ 5.8.4 | General archive helper, static | 0BSD library; do not confuse this with differently licensed XZ tools/scripts |
| libwebp | 1.6.0 | Image helper, static | BSD-3-Clause; COPYING and PATENTS already copied |
| PDFium package | chromium/8086 | PDF helper + pdfium.dll | PDFium BSD-3-Clause plus bundled component terms; full package notices included by release packager |
| libvterm | 0.3.3 | Terminal helper, static | MIT; notice included by release packager |
| SQLite | 3.54.0 | Native Linux agent, static | Upstream public-domain dedication; exact amalgamation hashes recorded |
| mpv API headers | e471496c56 | Media helper, compile-time | ISC; header notices prepared; this does not license the DLL |
| libmpv source build | folderwing-lgpl-20261025 / e471496c56 | Media fallback, dynamically loaded | LGPLv3 selected; pinned sources, build configuration and all media notices packaged |
| FFmpeg inside selected libmpv | 9a861e9687d30e657d59521817cad18405995983 | Static media dependency | LGPLv3; GPL/nonfree disabled, version3 enabled; exact source/configuration retained |
| musl libc | Zig 0.17.0 bundled source | Native Linux agent, static | MIT and embedded component notices; exact pinned source and file-level notice supplement reviewed |
| MinGW-w64 runtime | Zig 0.17.0 bundled source | Windows builds | ZPL-2.1 with file/header exceptions; reviewed candidate-family notices included |
| Zig compiler-rt builtins | 0.17.0 | Compiler-selected runtime routines | Zig MIT and musl-derived source notices retained |

The machine inventory conservatively retains libarchive's distribution-level
exceptions; this is not a claim that every source file is linked. PDFium's 17
supplied notices now match the exact mapped source graph; the review adds missing
libc++ and source-level attributions. Its statically embedded Microsoft CRT remains
a separate public-binary distribution blocker. The packager's top-level PDFium
LICENSE is MIT and does not replace the component terms. See the
[completed technical review and remaining runtime decision](DEPENDENCY_REVIEW_20261025.md).

The 119 source notice files live under [licenses/third-party](../licenses/third-party).
They preserve the downloaded/toolchain texts. Preparing these copies does not
change the current release directory or mean they are already included in a
user-downloadable archive.

## Build tools and external integrations

CMake 4.4.4 and Ninja 1.13.2 are pinned by `archive-dependencies.json`.
The current compiler reports Zig 0.17.0. 7-Zip is an external media-archive
extraction tool; its installation is not yet version-pinned. CairoSVG/Pillow
are icon-authoring tools, absent from normal app runtime requirements. These
tools are not shipped with the app; incorporated runtime code is listed above.

Windows APIs, WIC, Media Foundation, D3D11 and installed OS codecs are system
prerequisites. Windows system DLLs are not to be copied into the application ZIP.
Git and Everything are optional user-installed services/programs, not bundled
libraries. Their presence does not make their entire installations part of the
Folderwing download.

Direct is a separately developed Apache-2.0 companion. A native client is currently
beside the local development build, but it is not built by Folderwing's build
script. Keep its runtime, dependencies and release inventory separate. Including
it in a future distribution requires its own inventory; personal auth, runtime
state and configuration are outside the product archive.

## Work remaining before packaging releases

- **DEP-01 — media:** the pinned LGPL DLL and exact source/notices packages are
  built, focused acceptance passed and the backend was adopted locally; every future public download must
  retain the matching corresponding-source archive. See
  [the build and acceptance instructions](MEDIA_LGPL_BUILD.md).
- **DEP-02 — notice installation:** implemented in the explicit release packager,
  including PDFium package/component notices, libvterm, mpv API headers and all
  existing archive/WebP/media notices. Package acceptance verifies the output.
- **DEP-03 — toolchain runtimes:** closed for pinned Zig 0.17.0. Actual runtime
  families traced, 1,842 source/resource notice-bearing files matched, and
  conservative runtime/header attributions preserved.
- **DEP-04 — PDFium:** the controlled GNU runtime is now the default input, with
  active LLVM/MinGW/winpthreads notices and a pinned recipe companion. Fresh full-feature build, 79 affected selectors and 257 package checks passed.
  The [adoption record](PDFIUM_GNU_ADOPTION.md) tracks the local deployment
  and [completed public hosting](DEPENDENCY_HOSTING.md). The old Microsoft-linked candidate/inquiry is historical and
  does not gate this GNU route.
- **DEP-05 — companion boundary:** keep Direct separate until its own package
  dependency inventory is complete.

GitHub build/draft-release and separate proposed-media workflows are prepared;
a public repository, hosted dependency assets and public downloads are not yet
created by this preparation. Backend adoption is recorded separately in the media acceptance and
publication receipts; the package manifest does not replace those gates.

## Updating this inventory

On a dependency/toolchain change, update its acquisition pin first, preserve the
exact notice/source set, refresh observed hashes and this JSON snapshot, and
revisit affected open items. A future release packager should consume an explicit
product file list and dependency notices, not archive the development build
folder (which also contains test executables and retained old images).

## Media source-build closure

The selected media package retains 17 original source archives, including
libass, FreeType (FTL selected), FriBidi, HarfBuzz, libplacebo, shaderc, glslang,
SPIRV-Tools/Headers/Cross, dav1d, Opus, zlib, fast_float and Vulkan headers, plus
mpv/FFmpeg. The machine inventory lists each and the static GCC/MinGW runtime
notices separately. The full MinGW runtime source package/patches and exact
compiler-package locks are included. The runtime package carries 75 component
notice files and a hashed corresponding-source pointer.

The pre-build GPL dependency/candidate investigation remains available as a
[historical review](MEDIA_LGPL_REVIEW.md). The separate PDFium Microsoft-runtime distribution item remains open; the
application CRT review is complete for its pinned inputs.

The portable bootstrap and packager are documented in [RELEASING.md](RELEASING.md).
Dependency artifacts are [publicly hosted](DEPENDENCY_HOSTING.md).
Application source publication and each fresh release acceptance remain required.
The Microsoft startup-library question is no longer on the default build route.

## Controlled PDFium candidate

A [controlled source build](PDFIUM_CONTROLLED_BUILD.md) now imports UCRT and
VCRUNTIME140/140_1 externally and passed focused PDF/archive acceptance. It retains
31 Microsoft startup/support objects, as expected for `/MD`. Its redistribution
grant/package terms remain unresolved, so it is retained as a private candidate.
That historical candidate was not adopted; the controlled GNU package is now the default input.
Its SDK/compiler packages are build-only inputs; runtime DLL prerequisites are
installed separately, not added to the Folderwing archive.

The [GNU portability follow-up](PDFIUM_CONTROLLED_BUILD.md#gnu-portability-follow-up)
retains all 471 exports and passes the PDF, archive and conservative host-module
checks. It uses static LLVM/MinGW C++/unwind/stack-protector and winpthreads support
plus Windows/UCRT imports. That prototype is superseded by the [maintained GNU candidate](PDFIUM_GNU_BUILD.md),
with release linking, selected runtime source notices and package acceptance
complete. The active dependency now uses that accepted GNU package; see [adoption](PDFIUM_GNU_ADOPTION.md).

## Dependency hosting closeout

The four accepted runtime/source/recipe artifacts are published under
`L1nuxOne/folderwing` release `dependencies-20261025`; both GitHub Actions base URL
variables are configured. Their public downloads matched the pinned hashes.
The default-package dependency review gate is complete. This does not claim a
hosted application build, application source publication or public app release.
