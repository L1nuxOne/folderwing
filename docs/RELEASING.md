# Folderwing build and release preparation

Folderwing's application code is MIT. Direct is a separate product and is excluded
from the portable package. The normal development build remains `./build.ps1`.
The active PDFium dependency is the accepted controlled GNU build. Its runtime
notice/source mapping and package pins are in [PDFIUM_GNU_ADOPTION.md](PDFIUM_GNU_ADOPTION.md).
Local release builds take the exact PDFium runtime/recipe ZIPs and media
runtime/source archives. Both GitHub dependency base URL variables are now configured; see
[public dependency hosting](DEPENDENCY_HOSTING.md). Application source and workflows are now published; see
[SOURCE_PUBLISHING.md](SOURCE_PUBLISHING.md) for the hosted lanes and release policy. Every application release
still requires its own accepted build/package receipts.

## Build a complete portable candidate

Use Windows x64 with its built-in curl (system TLS trust), PowerShell 7 and Python 3.12 or newer. From a fresh checkout:

```powershell
./tools/build-release.ps1 `
  -MediaArchive C:\inputs\folderwing-media-lgpl-20261025.zip `
  -MediaSourceArchive C:\inputs\folderwing-media-lgpl-sources-20261025.tar.gz `
  -PdfiumArchive C:\inputs\folderwing-pdfium-gnu-8086-win-x64.zip `
  -PdfiumRecipeArchive C:\inputs\folderwing-pdfium-gnu-8086-recipe.zip
```

The media input filenames and SHA-256 values are in `tools/media-dependencies.json`.
After hosting those exact accepted files, set `FOLDERWING_MEDIA_BASE_URL` to their
HTTPS directory instead of passing local paths. Both runtime and corresponding
sources are verified. A changing upstream binary is never substituted.

The bootstrap restores Zig 0.17.0 and its exact source archive, PDFium, libvterm,
WebP and SQLite into the checkout from checksum-pinned downloads. Archive
libraries/tools are built by the existing pinned archive recipe. No user-level
dependency cache is adopted. A later bootstrap verifies its owned extracted trees
and refuses changed or unowned inputs rather than replacing them. Windows system
libraries are supplied by Windows and are excluded from the package.

The wrapper builds every binary required by `tools/release-files.json`, runs the
portable smoke lane, seals source/compiler/image hashes, then packages only the
listed runtime assets and preserved notices. Missing PDF or terminal inputs fail
the release build. `-SkipBootstrap` is for reusing an already accepted bootstrap
in the same checkout, as the workflow does; use ordinary bootstrap for a fresh
checkout. A package output directory must be new; earlier candidates are retained.

Outputs under `build/distribution`:

- `Folderwing-0.1.0-windows-x64.zip`, including helpers, library notices, MIT
  license, quick-start instructions and build metadata.
- The exact media corresponding-source archive named by `libmpv-SOURCE.txt`.
- `RELEASE-MANIFEST.json` and `SHA256SUMS`.

No settings, sessions, Direct runtime/auth, tests, retained old executables, build
objects, or diagnostic logs enter the app ZIP. An existing local development
release directory is never packaged wholesale. The package builder does not
publish, change shortcuts, stop processes or shut down WSL.

Check the actual package boundaries and extracted executable paths:

```powershell
./tests/release_packaging.ps1 -BuildDirectory build/release-candidate `
  -DistributionDirectory build/distribution -ResultDirectory build/package-acceptance
python tools/check-release.py --directory build/distribution
```

The smoke lane records the native selector identities and source/image hashes.
It covers CLI, workspace persistence, NTFS model, shell protocol and branding/
settings migration using private fixtures. It needs no WSL, real audio, GPU,
elevation or physical input. The diff module requires its native Linux fixture and remains in the explicit
`tests/file_diff.ps1` lane. Full WSL/GPU/audio integration remains a separate
explicit lane; it is not silently skipped inside the portable lane.

For full synthetic media acceptance, supply an FFmpeg executable with the encoders
listed by the generator. FFmpeg is a developer test tool and is not shipped:

```powershell
python tests/make_media_fixtures.py --ffmpeg C:\tools\ffmpeg.exe `
  --output-directory build/media-fixtures
./build.ps1 -OutputDirectory build/release-candidate -Targets @('media_tests.exe','media_host_tests.exe','media_audio_tests.exe','media_codec_tests.exe')
./tests/media_preview.ps1 -BuildDirectory build/release-candidate `
  -ResultDirectory build/media-acceptance -FixtureDirectory build/media-fixtures -Audio
```

The generator records its own/tool/file hashes, bounds codec threading, validates
video-only tracks, and refuses to overwrite an existing fixture directory. It
uses synthetic charts and tones. Exact output bytes can vary with the supplied
FFmpeg build; the manifest binds the actual files exercised. The full lane needs
its corresponding test executables built and an installed WSL distro plus an
awake playback device. It never shuts down WSL.

## GitHub workflows

`.github/workflows/build.yml` prepares Windows x64 candidates on pull requests,
main pushes, manual dispatch and `v*` tags. It restores pinned inputs, audits the
native registry, builds the full feature set, runs portable smoke and package
checks, and uploads eligible candidates plus separate text diagnostics. Binary artifact
uploads are held while public-release dependency review is open; failure evidence
does not include extracted or tampered runtime copies. Actions are pinned
to verified full commits; the lock record is `.github/actions-pins.json`.

Before the first GitHub run:

1. Create the source repository and push the reviewed source tree and workflows.
2. Host the accepted media runtime ZIP and matching source archive, preserving
   their exact filenames and bytes. These can be dependency-release assets.
3. Set the repository variable `FOLDERWING_MEDIA_BASE_URL` to their hosting directory.

The tag job verifies the embedded version and exact tagged revision, all artifact
checksums, and `public_release_ready` before creating a **draft** GitHub Release.
Current dependency closure is false; public release creation refuses while those
recorded review items remain open. Changing that flag requires closing the actual
inventory items, not bypassing the check. No release is automatically published.

The separate manual `.github/workflows/media.yml` builds proposed media packages
from the locked Linux builder. Download caches contain checksum-verified inputs;
compilation runs without network. Toolchain package/copyright records are captured
inside the same builder. Candidate filenames contain the workflow run ID, and the
proposed manifest is uploaded for review. This workflow does not replace the
accepted media pin, upload release assets, or change application source. Run the
media acceptance lane before promoting a new dependency package/manifest.

## First public release

Finish the dependency review items, set a deliberate app version in `app/brand.h`,
commit the final source and inventory, and push a matching `v<version>` tag. Review
the generated draft and its assets before publishing it. Code signing, installers,
package-manager feeds and automatic app updates are separate future improvements.

## Controlled PDFium build status

The historical [Microsoft-linked candidate](PDFIUM_DISTRIBUTION_REVIEW_20261025.md)
retains its unresolved distribution question and is not the default dependency.
The accepted [GNU package](PDFIUM_GNU_ADOPTION.md) avoids that startup-library
route. Its development installer preserves recognized old inputs in a hashed
backup and refuses unknown or changed inputs. Release bootstraps use a new owned
workspace when changing pins. The GNU COFF import library has a pinned `.lib`
alias for the existing Folderwing build interface.

The application package includes the active PDFium/LLVM/MinGW/winpthreads notices
and is accompanied by the exact PDFium recipe ZIP and corresponding LGPL media
sources. Configure `FOLDERWING_PDFIUM_BASE_URL` and `FOLDERWING_MEDIA_BASE_URL`
with the directory containing their exact named artifacts after hosting.
