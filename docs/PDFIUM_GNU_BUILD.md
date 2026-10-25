# Controlled GNU PDFium candidate — 2026-10-25

The maintained LLVM-MinGW recipe now builds and packages a tested PDFium
candidate without Microsoft compiler, SDK headers or MSVC startup libraries.
This document records the accepted candidate before adoption. The default input
has since switched to this package; see [current adoption](PDFIUM_GNU_ADOPTION.md).
Artifact hosting is separate from the historical Microsoft-linked candidate's
distribution question.

| Item | Accepted candidate |
| --- | --- |
| PDFium | `chromium/8086`, `157.0.8086.0`, commit `2fd6cff57d9412cc42ef1a7e4e0a59b13a1e7cec` |
| Toolchain | LLVM-MinGW 20261020 / LLVM 23.1.4 / x86_64 GNU ABI / UCRT |
| Source restoration | All 28 Git entries restored into a new workspace from pinned cached Git objects; alternates removed; exact pinned toolchain archive verified and extracted |
| Build | 1,016 PDFium C/C++ units, 27 assembly units and 84 build-only NASM units rebuilt; 3 workers; 518 seconds, functional build timing only |
| Runtime DLL | 8,957,440 bytes; SHA-256 `0da63a26cb2d23fc8d61409b07ce267bc111832d9b6c8fe7a11ea1f7393e0832` |
| Runtime ZIP | 4,284,026 bytes; SHA-256 `143966b99f5ce2bf181e0dadf8f038f8dc9914e0b8195aebbe86af7242cbfb0d` |
| Recipe ZIP | 435,470 bytes; SHA-256 `74678cc9f7e2e7f37ee5075eab76c5db2e309c1a24f610ff225d0f2f9e59b59f` |
| API / PE checks | Exact 471-export set; ASLR, NX, high-entropy VA, CFG instrumentation and 4,375 table entries, CET compatibility, GNU stack protector |

The [maintained recipe](../tools/pdfium-build/gnu/README.md) restores pinned source
and toolchain inputs, rebuilds NASM itself, compiles the frozen minimal-feature
action graph and applies the portability/public-export patches in a VFS overlay.
It does not mutate the source checkouts. The graph preserves the upstream feature
configuration; upgrades require a new graph/notice review. It is not a general
GNU backend for arbitrary future PDFium configurations.

The linker reproduction archive retains the actual object/archive inputs.
Runtime audit binds extracted archive members to those captured bytes. LLVM
compiler-rt, libc++/libc++abi and libunwind, MinGW startup/compatibility routines,
and winpthreads retain their notices. MinGW `libmsvcrt.a` includes both UCRT import
thunks and MinGW compatibility implementations; these are included in the source
mapping. Both libssp archives are empty stubs. No VCRUNTIME/MSVCP DLL or Microsoft
startup archive is linked, and the runtime ZIP includes only `pdfium.dll` as a DLL.
Windows/UCRT remains an OS prerequisite.

The [runtime/source/license mapping](../tools/pdfium-build/gnu/RUNTIME.md) includes
78 selected MinGW implementation/startup member mappings, per-file notices and a
conservative header notice superset. The recipe ZIP retains immutable source
references and patches; it is **not** a full PDFium source archive. The original
Microsoft-linked candidates and inquiry remain historical alternatives.

Validation against the exact DLL passed:

- PDF helper protocol/render/fit/text/find/thumbnail: 85 assertions, three selectors.
- Complete affected native host module: 2,882 assertions, including those PDF assertions.
- Archive helper and previews, including PDF entries: 197 assertions, eleven selectors.
- Combined distinct total: **3,079 assertions / 73 selected selectors**, no failures.
- Package boundaries: exact member hashes, single runtime DLL, Unicode/space-path
  loading, repeat packaging with identical runtime/recipe ZIP bytes, preservation
  of existing outputs, and rejection of changed DLLs, headers and license notices.
- Native registry audit/self-check: 47 modules / 451 selectors, no debt growth;
  Python test identity lists unchanged. The new recipe surfaces are bound to the
  existing PDF/helper and archive-preview selectors.

The source restore used cached immutable inputs; a complete remote-download
restore and a second full compiler rebuild were not run. Repeated packaging was
byte-identical. No full application suite, live desktop-input test, performance
qualification or multi-version Windows qualification is claimed. WSL was not stopped.

Artifacts are under `build/pdfium-gnu-maintained/20261025/distribution/`.
The compact [adoption record](dependency-reviews/20261025/PDFIUM-GNU-CANDIDATE.json)
contains exact pins and evidence pointers. The existing dependency remains active
until the release bootstrap/development downloader, notice inventory and package
checks adopt these pins together. Do not silently overwrite an owned bootstrap
directory or substitute a moving upstream binary. Public hosting and publication
remain separate release steps.
