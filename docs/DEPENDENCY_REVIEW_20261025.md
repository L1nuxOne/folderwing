# CRT and PDFium dependency review — 2026-10-25

The application CRT notice review (DEP-03) is closed for the pinned build.
PDFium's open-source component/source mapping is complete and its missing
attributions are repaired. **Public binary release remains held:** the selected
upstream PDFium DLL also embeds Microsoft's static CRT, whose redistribution
conditions are outside the MIT/BSD notices. DEP-04 now records this specific
remaining decision. Folderwing's own source remains MIT.

## Application CRT: what is incorporated

The reviewed compiler is Zig 0.17.0, executable SHA-256
`f54b14f02a4d7d8ac62a94a0663486eb166bba3745d85b466238c1b93e723d46`.
Its binary and full source archives are independently pinned in
[release-inputs.json](../tools/release-inputs.json).

Diagnostic builds of the engine DLL, PDF, image and media helpers and Linux agent
captured the actual compiler/link commands. Replaying those commands through the
same bundled LLD produced archive-member traces and maps. These are diagnostic
links, not replacement release images. The four executable/ELF replay outputs
match their diagnostic build bytes; renaming the DLL changes its export-directory
module name, so its replay is not a byte-identical attestation. The diagnostic
build reached all five targets; its final whole-product asset assertion failed
because other helpers were deliberately excluded from that diagnostic target
list. It is not reported as a passing release build.

| Output family | Observed runtime inputs | Distribution treatment |
|---|---|---|
| Windows GNU executables | `crt2`, `libmingw32`, `zigc`, `compiler_rt`; UCRT/Windows import libraries | Preserve MinGW and Zig/musl file attributions; Windows supplies the imported DLLs |
| Windows GNU engine DLL | `dllcrt2`, same runtime archives | Same notice treatment |
| C++ media/image code | Same Windows runtime inputs; no libc++/libc++abi/libunwind library in these link commands | Do not infer a C++ runtime dependency merely from the source language |
| Static Linux agent | musl `crt1`, `libc.a`, `libzigc.a`, `libcompiler_rt.a` | Preserve complete musl COPYRIGHT and embedded file notices |

[crt.json](dependency-reviews/20261025/crt.json) records the compiler pin,
representative extracted archive members, trace/map hashes and the 442-entry
MinGW x86_64 candidate list from `src/libs/mingw.zig`. It distinguishes archive
members selected during linking from final section reachability. Attribution is
kept for the candidate families, so it does not rely on a fragile minimal symbol
list. Compiler-resource copies of all **1,842 notice-bearing source/header files**
match the reviewed source archive exactly.

The previous top-level copies missed BSD/Sun/Arm math notices, David Gay/Lucent
permissions, winpthreads MIT notices and the musl-derived portions of Zig's C and
builtin runtimes. The added source-comment supplements preserve full adjacent
legal comment groups, with filenames and source hashes. They also cover possible
standard-library/header contributions. This is a conservative attribution
superset, not a claim that every listed file reaches every binary.

The bundled Windows headers have their own exceptions. The observed Wine-derived
headers (`d2dbasetypes.h`, `d3d10_1shader.h`, `d3d10misc.h`, `propvarutil.h`) contain
LGPL-2.1-or-later terms. The used material is declarations/layouts; the image helper
does not call the optional `propvarutil.h` inline helpers. No Wine DLL or header is
shipped. The object/header distinction and small-material exception are described
in [LGPL 2.1 section 5](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html#SEC5).
The MinGW `stdarg.h` contains historical GCC GPL terms plus a GCC-worded exception;
its builtin-alias macros/type declarations are recorded separately. We do not
assume that exception grants a general exemption to Clang implementations.
Both license texts and the original header notices are retained. Compiler-supplied
Clang headers have Apache-2.0 with LLVM exceptions; that complete text is now retained.

## PDFium: source and component mapping

The unchanged runtime DLL is SHA-256
`1f2aee4286b8cf0413ab4562b7121aa454f9cfd9afd6ca8609bcef80927b3f80`.
The package is `pdfium-win-x64.tgz`, `chromium/8086`, VERSION `157.0.8086.0`.

Source mapping uses PDFium commit
[`2fd6cff57d9412cc42ef1a7e4e0a59b13a1e7cec`](https://pdfium.googlesource.com/pdfium/+/2fd6cff57d9412cc42ef1a7e4e0a59b13a1e7cec),
its DEPS and build graph, the release changelog, the pinned
[packager commit](https://github.com/bblanchon/pdfium-binaries/tree/5325fa6d0d9379329f10f98fdc4839b4e40f2e79),
and its [successful Windows x64 build](https://github.com/bblanchon/pdfium-binaries/actions/runs/37320989355/job/111799734394).
All **17 supplied notice files match their mapped source texts**, allowing only
line-ending and edge-whitespace normalization. This is source/provenance mapping,
not a locally reproduced or upstream-attested exact link manifest.

[pdfium.json](dependency-reviews/20261025/pdfium.json) maps each notice to its
exact DEPS checkout or in-tree PDFium fork. README version labels are retained as
labels; stale README revisions do not override DEPS commit hashes. The configuration
disables V8, XFA, Skia, PartitionAlloc, Brotli, Fontations and Rust decoders.
`googletest`, NASM and test fonts are build/test inputs, not shipped runtime code.

| Component | Version basis | Selected/preserved terms |
|---|---|---|
| PDFium / Foxit font data | exact PDFium commit | BSD-3-Clause and source authors' notices |
| Abseil | exact Chromium checkout | Apache-2.0 |
| AGG | 2.3 PDFium hard fork | Original AGG permission text |
| Dragonbox | DEPS checkout; 1.1.3 README label | Boost 1.0 selected; both alternatives preserved |
| fast_float | 8.3.0 checkout | MIT selected |
| FreeType | 2.14.3-85 checkout | FTL selected, acknowledgment retained |
| HarfBuzz | 14.5.0-10 checkout | MIT modern variant |
| ICU | 78.2 checkout | Unicode 3.0 and bundled third-party texts |
| Little CMS | 2.19 PDFium fork | MIT, complete LICENSE added |
| libjpeg-turbo | Chromium checkout; 3.1.0 label | IJG/BSD/zlib notices and IJG acknowledgment |
| OpenJPEG | 2.5.4 PDFium fork | BSD-2-Clause, complete/source-level attributions added |
| libpng | 1.6.58 checkout | Libpng/Libpng-2.0 |
| llvm-libc | exact DEPS checkout | Apache with LLVM exception and legacy NCSA text |
| simdutf | 7.7.0 Chromium checkout | MIT |
| zlib | 1.3.2 Chromium checkout | Zlib |
| libc++ | exact DEPS checkout | Apache with LLVM exception and full legacy notices |

Two material notice gaps were found and repaired:

- The upstream notice script recognizes `libc` as an ignored name; its folder-name
  extraction splits `libc++` at `+`. The GN configuration defaults to custom static
  libc++ for this shared PDFium build. Its full license was absent and is added.
  Windows does not use libc++abi or libunwind in this configuration.
- OpenJPEG's package excerpt comes only from `openjpeg.c`. It omits the HT decoder's
  Aous Naman/Kakadu/UNSW attributions and other source notices. The upstream job
  explicitly compiled `ht_dec.obj`; complete source-level notices are added.

## Remaining public-binary decision: Microsoft's static CRT

`is_component_build=false` selects Chromium's `default_crt` → `static_crt` → `/MT`.
The DLL imports only KERNEL32, ADVAPI32, GDI32 and USER32, consistent with an embedded
CRT. This is different from the UCRT imports in our own Zig-built binaries.
The upstream job used Windows image `20260927.320.1`, Windows SDK `10.0.28000.0`,
and its example build reports VS 2022 Enterprise toolset `14.44.35207` / compiler
`19.44.35229.0`. These are observed host/example facts, not proof of the precise CRT
object revisions incorporated into PDFium; the package lacks that link manifest.

Microsoft's [distribution guidance](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files?view=msvc-170)
and [Visual Studio redistribution list](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution)
refer to conditional rights under the applicable Visual Studio license.
The [runtime end-user license](https://visualstudio.microsoft.com/license-terms/vs2022-cruntime/)
is not an onward-redistribution grant. Visual Studio's distributable-code terms
also impose downstream protections. Adding another BSD/MIT notice cannot resolve
this boundary; the current portable package does not contain an agreement for it.
This finding does not establish that upstream is distributing unlawfully.

The recommended next engineering step is a pinned PDFium source build with a
controlled runtime policy: dynamically import the Windows/runtime prerequisites
and keep proprietary CRT binaries outside Folderwing's package. Validate the PDF
helper against the resulting DLL and record all compiler/link inputs. Alternatively,
retain the current DLL only after its applicable redistribution grant, conditions
and required downstream terms are established. No license acceptance or public
publication is performed by this review.

## Validation and boundaries

The inventory and portable package keep the public-release flag **false** for the
specific Microsoft runtime item. Added notices are consumed by the existing
explicit release packager. Package checks, source/notice hashes and WSL continuity
are recorded in `build/dependency-review/20261025/ACCEPTANCE.json` when complete.
Previously accepted media artifacts and installed release images are unchanged.
There is no UI/performance claim and no reason to rerun those suites for notice-only
changes. WSL is not stopped.
