# GNU PDFium runtime provenance

This candidate uses PDFium 157.0.8086.0 and LLVM-MinGW 20261020, x64 UCRT.
It uses the public PDFium C API. Folderwing's own source remains MIT; the
following incorporated components retain their respective terms and notices.

| Incorporated code | Source | Notices / terms |
| --- | --- | --- |
| PDFium and selected engine dependencies | PDFium `2fd6cff57d9412cc42ef1a7e4e0a59b13a1e7cec`, exact dependencies in `source-graph.json` | BSD and other permissive component terms in `licenses/pdfium/`; conservative upstream notice bundle |
| LLVM compiler-rt builtins | LLVM `7a941dfb2877a0b8cf30323fefa85ad019e0b0f5` (`llvmorg-23.1.4`) | `compiler-rt-LICENSE.TXT`: Apache-2.0 with LLVM exceptions and retained legacy notices |
| libc++ / libc++abi (combined static `libc++.a`) | Same LLVM revision | `libcxx-LICENSE.TXT`, `libcxxabi-LICENSE.TXT`; Apache-2.0 with LLVM exceptions and retained legacy notices |
| libunwind | Same LLVM revision | `libunwind-LICENSE.TXT`; Apache-2.0 with LLVM exceptions and retained legacy notices |
| MinGW startup, compatibility code, GNU import libraries and stack protector | MinGW-w64 `0d6dfbc348dfad714761c0a7151615767f96bcb5` | `COPYING`, `COPYING.MinGW-w64.txt`, `COPYING.MinGW-w64-runtime.txt`: ZPL-2.1 and the retained BSD/public-domain/per-file notices |
| winpthreads (static) | Same MinGW-w64 revision | `COPYING.winpthreads.txt`: MIT and BSD notices |

The toolchain release recipe is pinned at LLVM-MinGW
`e08a31fbfe5cfdad07b698299af22d66e5608383`. Its
[LLVM source selection](https://github.com/mstorsjo/llvm-mingw/blob/e08a31fbfe5cfdad07b698299af22d66e5608383/build-llvm.sh),
[MinGW source selection](https://github.com/mstorsjo/llvm-mingw/blob/e08a31fbfe5cfdad07b698299af22d66e5608383/build-mingw-w64.sh),
[C++ runtime build](https://github.com/mstorsjo/llvm-mingw/blob/e08a31fbfe5cfdad07b698299af22d66e5608383/build-libcxx.sh)
and [winpthreads build](https://github.com/mstorsjo/llvm-mingw/blob/e08a31fbfe5cfdad07b698299af22d66e5608383/build-mingw-w64-libraries.sh)
are immutable source references. The exact binary toolchain archive and extracted
headers/libraries/tools are hash-pinned in `inputs.json` and
`toolchain-files.json`. License source URLs/hashes are in `notices.json`.

The linker extraction trace identifies the archive and member for incorporated
runtime code. GNU `libmsvcrt.a` supplies MinGW-generated import thunks to Windows'
UCRT and MinGW compatibility implementations, including stdio adapters and sincos.
Those implementation members are included in `runtime-sources.json` and the
supplemental source notices. It is not Microsoft's `msvcrt.lib` startup archive. No MSVC SDK headers,
compiler, startup libraries, VCRUNTIME or MSVCP DLL are inputs to this build.
The Windows OS and UCRT remain prerequisites. Windows system DLLs are not bundled.
Both `libssp.a` and `libssp_nonshared.a` are verified empty archive stubs;
GNU stack checking comes from the MinGW runtime, with `-fstack-protector-strong`
on compiled units. There is no GCC libssp code in this candidate.

NASM is built from the pinned Chromium NASM source with this GNU toolchain. It is
a build-only executable and is excluded from the runtime ZIP. No busybox, make,
Python, compiler executable, SDK or compiler DLL is shipped in that ZIP.

Binary redistribution must retain all accompanying copyright, license and
disclaimer texts and satisfy their conditions. The LLVM runtime exception covers
portions embedded into compiled objects; notices are retained conservatively.
No LLVM or MinGW runtime sources were modified. PDFium's small Windows allocator
and type-declaration changes are recorded in the accompanying portability patch.
The recipe ZIP carries that patch and exact source revisions; it is not a full
PDFium source archive. The previous Microsoft-linked candidates and their pending
licensing inquiry remain historical alternatives, separate from this candidate.

Public hosting, Folderwing acceptance and default-dependency adoption are separate
release steps. Possession of this candidate ZIP does not assert those steps passed.
