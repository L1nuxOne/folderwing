# Controlled Windows PDFium build

For the maintained GNU candidate without MSVC startup inputs, see
[gnu/README.md](gnu/README.md) and the [accepted result](../../docs/PDFIUM_GNU_BUILD.md).
The `/MD` recipe below is retained as a separate historical candidate route.

This builds a **candidate** of PDFium `chromium/8086` / `157.0.8086.0` using
external UCRT and VC runtime DLL prerequisites. It preserves the original 471
exports and the existing restricted helper architecture. It does not update the
installed Folderwing build, replace the default dependency pin, or publish files.

The experiment proved that `/MD` still incorporates Microsoft startup/support
objects. It does **not** close DEP-04. See the
[build result and remaining decision](../../docs/PDFIUM_CONTROLLED_BUILD.md).
Microsoft explicitly documents the static startup part even when the UCRT is
[dynamically linked](https://learn.microsoft.com/en-us/cpp/c-runtime-library/crt-library-features?view=msvc-170).

## Pins and restoration

`inputs.json` records exact build-tool hashes, compiler archive, source revisions,
SDK/toolset versions and patch hashes. `source-graph.json` is the actual resolved
39-entry gclient graph, including immutable CIPD instance URLs. `toolchain.lock.json`
retains all 64 official Microsoft payload URLs/hashes and the extracted layout.
The lock contains only build-tool dependencies; those packages do not belong in a
Folderwing runtime distribution.

The accepted build workspace is `C:\Work\fw-pdfium-20261025`. For another owned
workspace:

1. Restore the exact source commit and its DEPS using pinned depot_tools
   `36a8df4ad006eaa0572fb446edb8145fe5403592`. Use a `.gclient` solution named
   `pdfium`, `managed=False`, `target_os=['win']`, and custom vars
   `checkout_configuration='minimal'`, `checkout_rust=False`,
   `checkout_testing_corpus=False`, `download_remoteexec_cfg=False`. Sync with
   `--nohooks --no-history --shallow --revision=pdfium@2fd6cff57d9412cc42ef1a7e4e0a59b13a1e7cec`.
   Disable depot_tools self-update and automatic Windows toolchain acquisition.
   The build validates every Git revision and the actual compiler/GN/Ninja hashes.
2. Use the pinned Python 3.12.10 NuGet package in `inputs.json` for source sync
   and build commands. Python 3.14 is unsupported by the pinned gsutil. Do not set
   `PYTHONHOME`: GN invokes its separately pinned CPython 3.11 runtime. A local
   `python3.exe` copy beside `python.exe` avoids Windows' Store execution alias.
3. Restore the portable Microsoft toolchain from `toolchain.lock.json` into an
   owned directory, using the pinned portablemsvc source commit in `inputs.json`.
   Its `install-from-lockfile` command accepts an explicit output directory. Review
   the applicable Microsoft build-tool terms when using the tools. The original
   extraction used its pure-Python MSI path; it registered no system toolchain or
   global PATH. `setup_x64.bat` and the executable hashes must match the pins.
4. Create `.folderwing-owned-pdfium-build` in the source workspace's parent.
   Use Windows Git with `core.autocrlf=true` in these owned checkouts, matching the
   accepted Windows checkout. Do not apply reset/clean commands to another tree.

The discovery channel advertised a manifest hash/size different from the bytes
served at its immutable HTTPS CDN URL. The recipe records both hashes explicitly.
The delivered document was separately pinned, every selected payload passed its
own hash check, and cl/link/rc/mt all had valid Microsoft Authenticode signatures.
This is **not** described as a successful channel-manifest hash verification.
Future restoration uses the frozen payload lock, not a fresh moving manifest.

## Build, audit and package

From the Folderwing repository, using Windows Python:

```powershell
C:\Work\fw-pdfium-20261025\python312\python.exe tools/pdfium-build/build.py `
  --source-root C:\Work\fw-pdfium-20261025\pdfium `
  --toolchain-root C:\Work\fw-pdfium-20261025\msvc `
  --git 'C:\Program Files\Git\cmd\git.exe' `
  --python C:\Work\fw-pdfium-20261025\python312\python.exe `
  --jobs 3 --output-directory C:\Work\pdfium-result-new
```

The script refuses changed input hashes, unexpected source edits, incompatible
vcvars shims and existing result directories. It applies the exact patches,
records inputs before compilation, runs GN/Ninja, retains response files/maps and
full commands, and checks source stability afterwards. It performs no publication.

The patches keep `is_component_build=false`, export the PDFium C API, select
`/MD` for every translation unit, use the pinned SDK's `NTDDI_WIN11_GE` declarations,
and avoid Chromium's automatic CRT-DLL copying. The x64-only portable toolchain
setup is explicit; unsupported architecture requests fail instead of borrowing a
mislabelled x64 environment. V8, XFA, Skia, Brotli, PartitionAlloc and Rust decoders
remain disabled as in the selected feature configuration.

```powershell
python tools/pdfium-build/audit.py --dll C:\Work\pdfium-result-new\pdfium.dll `
  --map C:\Work\pdfium-result-new\pdfium.map `
  --baseline third_party/pdfium/bin/pdfium.dll `
  --commands C:\Work\pdfium-result-new\commands.txt `
  --output C:\Work\pdfium-runtime-audit-new.json
python tools/pdfium-build/package.py --build-directory C:\Work\pdfium-result-new `
  --source-root C:\Work\fw-pdfium-20261025\pdfium `
  --baseline-dll third_party/pdfium/bin/pdfium.dll `
  --output-directory C:\Work\pdfium-candidate-new
```

The ZIP has the DLL, generated import library, C API headers, notices, exact build
metadata and [prerequisites](PREREQUISITES.md). It contains no Microsoft runtime
DLLs, SDK/compiler packages, PDBs or settings. There is no public-release switch.
