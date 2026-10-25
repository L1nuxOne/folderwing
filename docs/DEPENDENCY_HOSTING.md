# Published build dependencies

The public [Folderwing repository](https://github.com/L1nuxOne/folderwing) now hosts
the [pinned dependency release](https://github.com/L1nuxOne/folderwing/releases/tag/dependencies-20261025).
The application source and application releases remain separate next steps.

The release contains the exact accepted artifacts:

- `folderwing-pdfium-gnu-8086-win-x64.zip`
- `folderwing-pdfium-gnu-8086-recipe.zip`
- `folderwing-media-lgpl-20261025.zip`
- `folderwing-media-lgpl-sources-20261025.tar.gz`

`DEPENDENCY-RELEASE.json` lists their sizes and SHA-256 hashes. `SHA256SUMS` covers
all four artifacts and that manifest. GitHub's uploaded digests and independent
unauthenticated public downloads matched the accepted local inputs.

Both repository Actions variables are configured to:

```text
https://github.com/L1nuxOne/folderwing/releases/download/dependencies-20261025
```

The variables are `FOLDERWING_MEDIA_BASE_URL` and `FOLDERWING_PDFIUM_BASE_URL`.
The PDFium runtime URL is also recorded directly in `tools/release-inputs.json`.
Local builds from the application checkout can use:

```powershell
$base = 'https://github.com/L1nuxOne/folderwing/releases/download/dependencies-20261025'
./tools/build-release.ps1 -MediaBaseUrl $base -PdfiumBaseUrl $base
```

The existing bootstrap verifies all artifact hashes, and the media sources and
PDFium recipe remain required release companions. This is a versioned GitHub
release, not a claim that GitHub enforces immutable releases; local hash pins
reject changed bytes. New dependency versions should use a new reviewed release.

The repository now includes the application source and build workflows described
in [SOURCE_PUBLISHING.md](SOURCE_PUBLISHING.md). A source push starts the hosted
Windows build; each run's result is visible in GitHub Actions. Application version
releases remain separately reviewed drafts. The earlier local application
candidate retains its original pre-hosting receipt.

Verification and publication records are under
`build/dependency-hosting/20261025/`. WSL and the installed application binaries
were unchanged during publication.
