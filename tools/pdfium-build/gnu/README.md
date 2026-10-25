# Maintained Windows GNU PDFium candidate

This path builds PDFium 157.0.8086.0 with LLVM-MinGW 20261020, LLVM 23.1.4,
x86_64 GNU ABI and UCRT. Microsoft compiler/SDK/startup libraries are not build
inputs. Folderwing's own code still builds with Zig.

`graph.json` is the frozen minimal-feature GN action graph, translated from the
accepted upstream configuration. It deduplicates compiler flag groups and lists
all 1,016 target C/C++ units, 27 assembly units, and 84 build-only NASM units.
The 51 Chromium libc++ units are replaced by the matching LLVM-MinGW runtime.
The source graph, translation adaptations and original command-list digest are
retained. This is a deliberately pinned build recipe, not general support for
arbitrary future PDFium GN flags. A PDFium upgrade must regenerate and review the
graph and dependency notices. It requires no generated source headers, GN,
Chromium Clang, CIPD tool executions, Visual Studio installation or Python package.

Use Windows Python 3.12 or newer and Git for Windows. Source checkout uses
`core.autocrlf=true`, matching the per-file source hashes. The restorer creates a
new owned workspace, fetches only exact Git revisions, verifies the LLVM-MinGW
archive and extracted build inputs, and never resets an existing checkout.
`--source-cache` optionally borrows existing Git objects, then repacks and removes
alternates; no generated files or built objects are copied from the cache.

```powershell
$py = 'C:\Python314\python.exe'
$git = 'C:\Program Files\Git\cmd\git.exe'
& $py tools/pdfium-build/gnu/restore.py --workspace C:\Work\pdfium-gnu-source `
  --git $git
& $py tools/pdfium-build/gnu/build.py `
  --source-root C:\Work\pdfium-gnu-source\pdfium `
  --toolchain-root C:\Work\pdfium-gnu-source\toolchain --git $git `
  --jobs 3 --output-directory C:\Work\pdfium-gnu-result
& $py tools/pdfium-build/gnu/audit_runtime.py `
  --build-directory C:\Work\pdfium-gnu-result `
  --toolchain-root C:\Work\pdfium-gnu-source\toolchain `
  --baseline third_party/pdfium/bin/pdfium.dll `
  --output C:\Work\pdfium-gnu-runtime-audit.json
& $py tools/pdfium-build/gnu/package.py `
  --build-directory C:\Work\pdfium-gnu-result `
  --source-root C:\Work\pdfium-gnu-source\pdfium `
  --toolchain-root C:\Work\pdfium-gnu-source\toolchain `
  --baseline third_party/pdfium/bin/pdfium.dll `
  --output-directory C:\Work\pdfium-gnu-package
```

Restoration can use `--toolchain-archive` with the exact pinned ZIP. All commands
refuse existing result directories. Failures retain their owned logs. The build
records inputs before execution, clears inherited compiler include/library
overrides, rebuilds NASM first, and isolates the portability and public-export patches in
a VFS overlay. It accepts pristine pinned source trees or the exact earlier
controlled interface/build patches; it never changes source files. NASM's Windows
configuration additionally declares the `mempcpy` capability provided by MinGW.
It does not globally pretend the compiler is MSVC.

The release link enables section collection and identical-code folding, removes
debug data, and retains all 471 exports, ASLR, NX, high-entropy VA, CFG tables,
CET compatibility and GNU stack checking. `runtime-members.tsv` records the
archive/member extraction trace, resolved against pinned toolchain search paths;
`runtime-link-inputs.tar` retains the actual linker inputs for independent hash checks. `audit_runtime.py` checks this trace, exact API,
runtime imports, GNU startup objects, empty libssp stubs and PE mitigations.

The runtime ZIP has a flat bootstrap-compatible root: `bin/pdfium.dll`, C headers,
GNU import library, version, build information and notices. It excludes compiler
tools, NASM, external runtime DLLs, private settings and PDBs. The recipe ZIP
retains the exact recipes, patches, pins and immutable upstream source references;
it is **not** a full PDFium source archive. ZIP member timestamps are fixed.
See [RUNTIME.md](RUNTIME.md) for the incorporated runtime/source/license mapping.

These tools produce a reviewable candidate. They do not modify the installed app,
change the default release dependency or upload artifacts. Package adoption must
bind acceptance evidence to the exact DLL and archive hashes and update the
release pins/inventory together. Public artifact hosting is a separate step.
