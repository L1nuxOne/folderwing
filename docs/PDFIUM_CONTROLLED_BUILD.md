# Controlled PDFium build result — 2026-10-25

**Latest follow-up:** the maintained [GNU recipe and candidate](PDFIUM_GNU_BUILD.md)
now build from pristine pinned sources, package the selected runtime notices, and
pass 3,079 distinct host/archive assertions plus package-boundary checks. Default
pin adoption and release acceptance are now complete; see [adoption](PDFIUM_GNU_ADOPTION.md).
Public artifact hosting remains. The Microsoft-linked route below retains
its separate unresolved redistribution question.

A pinned Windows x64 PDFium candidate has been built and tested with external
runtime prerequisites. Its main UCRT and VC runtime implementations are imported
from Windows/the installed Microsoft redistributable. **It is retained as a
candidate, and the installed Folderwing release is unchanged.**

The earlier proposal was incomplete as a way to remove all Microsoft runtime code:
`/MD` still incorporates CRT startup/support objects. The map identifies 31 such
objects, including `_DllMainCRTStartup`, TLS initialization, security-cookie checks,
thread-safe static initialization, and C++ allocation support. Microsoft's
[CRT documentation](https://learn.microsoft.com/en-us/cpp/c-runtime-library/crt-library-features?view=msvc-170)
explicitly describes this static startup layer with the dynamic CRT. The experiment
therefore **does not close DEP-04 or enable public binary release**. The follow-up
[distribution review](PDFIUM_DISTRIBUTION_REVIEW_20261025.md) distinguishes this
normal startup layer from the unresolved redistribution grant; its presence is
not itself evidence of an MIT incompatibility.

## Built artifact and prerequisites

| Item | Result |
|---|---|
| PDFium source | `2fd6cff57d9412cc42ef1a7e4e0a59b13a1e7cec` (`chromium/8086`, `157.0.8086.0`) |
| Resolved dependency graph | 39 pinned Git/GCS/CIPD entries |
| Compiler | Chromium Clang `llvmorg-24-init-7747-g62397f8b-57`, archive SHA-256 pinned in `inputs.json` |
| Microsoft build inputs | MSVC toolset `14.44.35207`, Windows SDK `10.0.26100.0`; 64 individually hashed payloads |
| Link policy | `/MD`, one PDFium DLL, no CRT-DLL copying |
| Observed compile commands | 1,151 dynamic-CRT commands; zero `/MT` commands |
| PDFium DLL size | 7,164,928 bytes; previous pinned DLL 7,494,656 bytes |
| PDFium DLL SHA-256 | `8b36e28dbc4e47f7f4dc182036d6a404fdc7ddfde5391c1976a317053edac9e3` |
| Export compatibility | All 471 exports retained, none added/removed |
| Image hardening | ASLR, NX and CFG retained |
| Runtime imports | Windows UCRT API sets, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`, plus the existing Windows API DLLs |
| Remaining Microsoft objects | 30 selected from `msvcrt.lib`, one from `vcruntime.lib` |

The candidate requires Windows 10/11 x64 and Microsoft's supported Visual C++ v14
x64 runtime, 14.44.35229 or newer. See the
[candidate prerequisite instructions](../tools/pdfium-build/PREREQUISITES.md) and
[Microsoft's compatibility guidance](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170).
No Microsoft runtime DLL is bundled. The test machine already had version
14.51.36247; no runtime installation was performed.

## Controlled recipe and provenance

[tools/pdfium-build](../tools/pdfium-build/README.md) contains the build, PE/link
inspection and candidate packaging scripts, exact source graph, payload lock,
compiler/build-tool hashes, build patches and notice sources. The maintained recipe
was run against the accepted workspace and produced the same DLL bytes as the
initial build. This verifies the recipe and candidate; it is not a second clean
compilation or a cross-machine reproducibility attestation.

The patches change build/interface configuration. They retain one shared PDFium
DLL with V8/XFA/Skia/Brotli/PartitionAlloc/Rust decoders disabled, select the dynamic
CRT, adapt the portable x64 SDK setup, keep ASLR/NX/CFG, and preserve link maps and
response files. SDK API declarations use `NTDDI_WIN11_GE`, matching SDK 26100;
`_WIN32_WINNT` and `WINVER` remain Windows 10. There are no PDF engine implementation
changes and no custom startup or security-check replacement.

The build-only toolchain was extracted into an owned workspace without registering
a system toolchain or changing global PATH. The delivered Microsoft discovery
manifest differed from its channel's advertised hash/size. Both values are retained;
that channel check is not claimed as passing. Its delivered bytes were separately
pinned, all 64 selected payloads passed their individual hashes, and cl/link/rc/mt
passed Windows Authenticode checks. Future restores use the frozen payload lock.
The toolchain, its private CRT sources, downloaded packages and caches are excluded
from the candidate ZIP.

## Focused acceptance

- **85 assertions / three PDF selectors passed:** text layout, page positions,
  actual helper framing, document/page opening, bitmap rendering, fit detection,
  text extraction, search and thumbnail protocol behavior.
- **197 assertions / eleven archive selectors passed**, including extraction and
  PDF entry preview through the ordinary helper path.
- A freestanding loader probe imports only KERNEL32 itself. Normal loading and
  PDFium initialization succeeded with the installed external runtime. A separate
  probe with restricted dependency search failed with the expected missing-module
  result when the VC runtime was unavailable to that search. Only the owned probe's
  search policy changed; no system DLL was renamed, removed or replaced.
- The PE/link audit retained all exported APIs and hardening flags, verified
  external runtime imports, and exposed the 31 startup/support objects.

The source/image keys and final acceptance receipt are under
`build/pdfium-controlled/20261025`. There was no broad UI/performance audit, physical
input, guest interruption, WSL shutdown, or public GitHub mutation.

## Distribution decision

The candidate ZIP includes the DLL, generated import library, C API headers,
notices, prerequisite instructions and build/runtime metadata. It is explicitly
marked `public_release_ready=false`. The default upstream dependency pin and
installed release remain in place; the general release packager still refuses
public-ready packaging while DEP-04 is open.

Two routes remain for public distribution: establish the applicable conditions
for the actual Microsoft startup/support portions and reflect them in the binary
package, or build and validate a variant with an open-source Windows CRT/toolchain.
Replacing startup/security/TLS code ad hoc would add ABI and initialization risk;
this build makes no such replacement. Folderwing's own source remains MIT.

## LLVM-MinGW feasibility follow-up

A subsequent configuration-only experiment with LLVM-MinGW 20261020 compiled six
representative units, then stopped during the full compile at PDFium's
compiler-selected aligned allocator. A Windows GNU build needs a platform-aware
allocation/deallocation portability fix before compilation can proceed. No GNU
variant DLL was produced or adopted, and no engine source was changed. See the
[follow-up evidence and limits](PDFIUM_DISTRIBUTION_REVIEW_20261025.md#community-2026-and-bounded-llvm-mingw-follow-up--2026-10-25).

## GNU portability follow-up

The [isolated portability patch](../tools/pdfium-build/patches/llvm-mingw-portability.patch)
selects Windows aligned allocation, its declaration and matching free by platform
rather than compiler identity. It also removes one redundant global graphics-type
forward declaration that conflicts with the existing namespaced alias. The patch
was applied only through a Clang VFS overlay of copied files; the accepted source
tree and all installed/default DLLs were verified unchanged.

LLVM-MinGW 20261020 / LLVM 23.1.4 compiled all **1,016 C/C++ units and 27 assembly
units**. Linking used the toolchain's static C++/unwind/stack-protector runtimes
and winpthreads, with GNU-driver COFF passthrough for CET compatibility. The map
identifies MinGW `dllcrt2.o` startup and winpthreads members; the link uses no
Microsoft MSVC startup/support archives. The DLL imports Windows/UCRT APIs, with
no VCRUNTIME, MSVCP, external libc++ or external winpthreads DLL imports.

The exact unstripped prototype is **40,368,640 bytes**, SHA-256
`6085648b6e8f4c031a92722a82ae57d35c444a15b08d406121df5869c14d9f3f`.
All **471 exports** match the existing dependency. PE inspection verified ASLR/NX,
CFG instrumentation/function table, and the CET compatibility marker. This is an
image/configuration check, not a comprehensive mitigation audit or performance
comparison.

Acceptance passed against that exact DLL:

- Actual patched PDFium allocator: **1,280** positive allocation/alignment/
  read-write/free cases and **32** null frees, using real PDFium dependencies.
- PDF text/positions/helper protocol/render/fit/find/thumbnail: **85 assertions**
  over three selectors.
- Archive preview, including PDF entries: **197 assertions** over eleven selectors.
- The conservative source-impact plan expands to both native host executables.
  The complete app host passed **2,882 assertions**; together with the archive
  executable this covers all **73** planned selectors. The focused PDF assertions
  are a subset of the complete host run and are not added to its total.
- Native registry audit/self-check passed for **47 modules / 451 selectors**;
  the new patch is bound to the existing PDF and archive-preview selectors.
  Python selector identity lists are unchanged, with no new skip/xfail/quarantine/
  automatic-retry debt.

The first complete-host attempt lacked its exe-relative image fixture directory.
That failure is retained. An owned fixture layout corrected the harness setup;
the whole module then passed without changing product code or weakening tests.

The [compact record](dependency-reviews/20261025/LLVM-MINGW-PORTABILITY.json)
references the consolidated receipt under
`build/pdfium-mingw-portability/20261025/RESULT.json`. Raw source overlays, build
commands, objects, DLL, maps and per-phase inputs/results are retained in
`C:\Work\fw-pdfium-mingw-portability-20261025`.

This establishes a working technical alternative to the Microsoft-linked
candidate. It does not switch the default dependency, qualify all Windows
versions, run the full application suite, or enable public release. This prototype phase is superseded by the
[maintained GNU candidate](PDFIUM_GNU_BUILD.md): recipe, runtime notices and
package acceptance are complete; adoption remains. WSL was not stopped.
