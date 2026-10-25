# GNU PDFium default dependency

Folderwing's default PDFium input is now the controlled GNU package in
`tools/release-inputs.json`. The accepted DLL SHA-256 is
`0da63a26cb2d23fc8d61409b07ce267bc111832d9b6c8fe7a11ea1f7393e0832`.
Its runtime ZIP is pinned to
`143966b99f5ce2bf181e0dadf8f038f8dc9914e0b8195aebbe86af7242cbfb0d`,
and its recipe ZIP to
`74678cc9f7e2e7f37ee5075eab76c5db2e309c1a24f610ff225d0f2f9e59b59f`.

Both the development downloader and release bootstrap use the same pinned
installer. The accepted artifacts are now [publicly hosted](DEPENDENCY_HOSTING.md).
Local archive inputs remain supported:

```powershell
./tools/get-pdfium.ps1 -Archive C:\inputs\folderwing-pdfium-gnu-8086-win-x64.zip `
  -MigrateExisting
./tools/build-release.ps1 `
  -PdfiumArchive C:\inputs\folderwing-pdfium-gnu-8086-win-x64.zip `
  -PdfiumRecipeArchive C:\inputs\folderwing-pdfium-gnu-8086-recipe.zip `
  -MediaArchive C:\inputs\folderwing-media-lgpl-20261025.zip `
  -MediaSourceArchive C:\inputs\folderwing-media-lgpl-sources-20261025.tar.gz
```

The migration switch only accepts the recognized original upstream DLL. It moves
the complete old input into a unique `build/dependency-backups/pdfium-legacy-*`
directory with a file-hash manifest. Unknown inputs are retained and refused.
A failed installation restores the old directory if the destination is absent.
Current owned installations are verified against their complete stamped tree.
Changed archive hashes are rejected before migration.

The build's existing `pdfium.dll.lib` filename is a byte-identical alias of the
GNU-generated `pdfium.dll.a` COFF import archive. It contains no MSVC startup code.
Both names are pinned to the same hash and recorded in the bootstrap tree receipt.
The unchanged PDF helper source uses the same 471-export C API.

The active notice inventory uses the PDFium and LLVM/MinGW/winpthreads notices.
The earlier Microsoft runtime note and Chromium libc++ notice remain in the
repository for historical review and are excluded from the active package.
The exact GNU recipe archive accompanies the application ZIP alongside the
matching LGPL media sources. Full source references, runtime/license mappings
and candidate acceptance are in [PDFIUM_GNU_BUILD.md](PDFIUM_GNU_BUILD.md).

Hosted CI additionally requires `FOLDERWING_PDFIUM_BASE_URL`, pointing to the
immutable directory containing the runtime and recipe ZIPs. Media uses
`FOLDERWING_MEDIA_BASE_URL` for its matching runtime and source archives.
No moving upstream or Microsoft-linked PDFium fallback is used.

The adoption run uses a new owned source/bootstrap workspace and records its
build, portable smoke, PDF/archive acceptance and package checks under
`build/pdfium-gnu-adoption/20261025/`. Hosting artifacts are prepared separately at
`C:\Work\folderwing-dependency-hosting-20261025`. Creating a public repository
and publishing those artifacts is separate from local adoption.

## Local acceptance and deployment

The fresh full-feature release build passed its portable smoke lane and 257
package checks. The 79 impact-selected native selectors passed with 3,147 distinct
assertions (PDF assertions are a subset of the host total); eight installer
boundary checks also passed. The working `build/release/pdfium.dll` now matches
the accepted GNU hash. The helper executable was already byte-identical to the
newly linked result. Previous files are preserved in the adoption run's
`release-backup` directory; no user processes or WSL instance were stopped.

The complete portable candidate is under
`C:\Work\fw-gnu-adoption-20261025\build\distribution`. These are local
acceptance results; no hosted CI execution or public app release is claimed.
Public dependency hosting is now complete in `L1nuxOne/folderwing`; see
[hosting and verification](DEPENDENCY_HOSTING.md). The application source and
application release are separate next steps.
