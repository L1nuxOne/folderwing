# Public source and build workflows

The application source is maintained in
[L1nuxOne/folderwing](https://github.com/L1nuxOne/folderwing).
The initial application import uses the reviewed current source snapshot and
preserves the repository's dependency-release commits. The earlier development
repository and its generated evidence remain local; its private history is not
part of the public import.

Application/engine/helper source, tests, synthetic fixtures, build recipes,
branding assets and third-party notices are included. Downloaded libraries,
compilers, built programs, caches, settings and generated run evidence are
excluded. The old OpenPilot screenshot with a developer-specific home path is
retained locally rather than included in the public snapshot.

## Windows build

From the complete application checkout, use PowerShell 7 and Python 3.12 or newer:

```powershell
$base = 'https://github.com/L1nuxOne/folderwing/releases/download/dependencies-20261025'
./tools/build-release.ps1 -MediaBaseUrl $base -PdfiumBaseUrl $base
```

This restores hash-pinned inputs, builds the full feature set, runs the portable
functional smoke lane, seals the source/binary receipts and creates the portable
ZIP with notices and source/recipe companions. Optional focused WSL build scripts
use the bootstrapped `third_party/zig/zig.exe`, with a `ZIG` override supported.

## GitHub Actions

- Pull requests and pushes to `main` run the Windows build, native registry audit,
  portable smoke, package-boundary checks and artifact checksum validation.
- Successful eligible builds upload the portable application candidate, matching
  media sources and PDFium recipe as a build artifact.
- A `v*` tag must match the version compiled into the application. It builds and
  verifies the tagged inputs, then creates a **draft** application release for
  review. It does not automatically publish that draft.
- The separate media dependency workflow is manually dispatched. It builds a
  proposed LGPL media runtime/source bundle for independent acceptance.

The hosted Windows lane is a bounded functional lane. Physical-input UI tests,
audio/GPU tests, WSL/VM interruption tests and performance qualifications have
their own profiles; a green portable build does not stand in for those lanes.
The accepted dependency release is documented in [DEPENDENCY_HOSTING.md](DEPENDENCY_HOSTING.md).
