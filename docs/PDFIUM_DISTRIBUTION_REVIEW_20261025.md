# PDFium Microsoft runtime distribution review — 2026-10-25

**Current alternative:** a maintained [GNU candidate](PDFIUM_GNU_BUILD.md) now
builds from pristine pinned sources, packages the runtime notices and passes
focused acceptance. It avoids the MSVC startup archives at issue here. The default
input has now adopted that GNU build; [current status](PDFIUM_GNU_ADOPTION.md).
Public artifact hosting remains. This historical Microsoft-linked
candidate's redistribution question is still unresolved; no MIT incompatibility
has been demonstrated.

The reviewed DLL is SHA-256
`8b36e28dbc4e47f7f4dc182036d6a404fdc7ddfde5391c1976a317053edac9e3`.
Its [build and acceptance](PDFIUM_CONTROLLED_BUILD.md) remain unchanged. This is a
distribution review, not a new build or a legal-clearance receipt.

## What the remaining objects mean

Microsoft's [CRT library reference](https://learn.microsoft.com/en-us/cpp/c-runtime-library/crt-library-features?view=msvc-170)
describes `msvcrt.lib` as the static startup library used with the DLL UCRT and
vcruntime, and explains that startup is always statically linked. The 31 observed
objects are therefore consistent with an ordinary `/MD` build. Their presence
does not by itself show that distribution is prohibited, that Folderwing must
change its source license, or that replacing the CRT is necessary.

Conversely, external runtime DLLs do not remove the need to establish the rights
and conditions for incorporated Microsoft code. The DLL is not entirely under
open-source licenses merely because its main runtime implementations are external.

## Grants checked against the actual acquisition

The frozen Visual Studio 2022 manifest identifies the English Build Tools license
as `https://go.microsoft.com/fwlink/?LinkId=2179911`. On this review date it resolves
to the [2022 Diagnostic and Build Tools terms](https://visualstudio.microsoft.com/license-terms/vs2022-ga-diagnosticbuildtools/).
The Community product's corresponding link, `LinkId=2179811`, resolves to the
[Community 2022 terms](https://visualstudio.microsoft.com/license-terms/vs2022-ga-community/).
These match the reference agreements already preserved under
`docs/dependency-reviews/20261025/sources`.

| Source | What it establishes | What it does not establish here |
|---|---|---|
| Build Tools 2022, installation/use subsection (d), and build-device terms | Third-party OSI-licensed C++ dependencies can be built without a separate Visual Studio product license; limited build-enabling changes and build-process verification are covered | An explicit independent Distributable Code grant was not found in this agreement |
| Community 2022, section 1 | Use rights for an individual working on their own applications, and specified organizational scenarios including OSS | This portable toolkit acquisition is not evidence that the developer acquired and is using a valid Community product license |
| Community 2022, section 4 | Distribution rights for designated Distributable Code, including onward distribution as part of applications, subject to requirements | A blanket license for every file in MSVC, or proof that these exact startup/support portions are covered |
| Visual Studio 2022 REDIST list | Named distributable components under the applicable licensed product's terms | Explicit entries naming `msvcrt.lib` and `vcruntime.lib` were not found; the list alone does not finish the static-output mapping |
| VC Runtime 2015–2022 end-user agreement | Installation and use of the runtime | An onward redistribution grant for our DLL or a developer toolchain license |

The source CRT package contains Microsoft copyright notices; the desktop CRT VSIX
contains libraries/PDBs and package/signature metadata. No additional standalone
redistribution license was identified in that desktop payload. This is not a claim
that every Microsoft SDK/package license has been exhaustively interpreted.

Microsoft's [licensing guidance](https://www.microsoft.com/licensing/guidance/Visual-Studio)
distinguishes distributing designated `.lib` files themselves from distributing
application output linked with them. It supports the normal linked-output route,
but still requires that the code be designated distributable and that the
applicable agreement be satisfied. It does not replace the agreement or establish
our entitlement by itself.

The [2022 REDIST list](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution)
and [redistribution instructions](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files?view=msvc-170)
must be read with the product terms. Current 2026 agreements/lists are not silently
substituted for the pinned 2022 acquisition.

## Package requirements if that route is established

1. Keep Folderwing's own source under MIT. Preserve all PDFium/component notices
   and state that bundled third-party portions retain their own licenses.
2. Record the applicable developer/product licensing basis and the authority
   covering the incorporated release startup/support code. Download hashes,
   Authenticode validity, or reading an EULA are not substitutes for this.
3. Satisfy the applicable binary distribution requirements. Community section 4
   requires significant primary functionality and agreement to terms protecting
   Distributable Code by distributors/external end users. Merely adding a copyright
   notice is not demonstrated to meet that requirement. Any proposed binary terms
   must leave the MIT source and other components' granted rights intact.
4. For this `/MD` candidate, document Windows 10/11 x64 and the supported Microsoft
   Visual C++ x64 runtime, 14.44.35229 or newer. Direct users to Microsoft's installer;
   do not add Microsoft runtime DLLs/installers to the ZIP without establishing
   their redistribution basis separately.
5. Ship no Microsoft compiler, private CRT sources, SDK archives, debug runtimes,
   or loose Microsoft startup `.lib` files. The generated **PDFium API import
   library** in the candidate is a separate artifact, not `msvcrt.lib`.

## Decision and next action

DEP-04 stays open for **the applicable grant and package terms**, not for a proven
MIT conflict and not simply because static objects remain. Neither the upstream
default pin nor the controlled candidate is newly cleared by this review.

The next action is to establish the licensed Microsoft release-build route and
its static-output coverage, then prepare the corresponding binary terms and
adopt the already tested candidate. If that basis cannot be established, an
open-source CRT/toolchain build remains a fallback. Do not replace startup, TLS,
security-cookie or exception support ad hoc just to eliminate the object count.

Only documentation and inventory metadata change in this review. The prior PDF
and archive results remain evidence for their exact DLL/source cohort; they are
not relabeled as a new application test run. No runtime installation, toolchain
installation, public upload, release replacement, or WSL shutdown was performed.

## Follow-up: published support discussion and concrete inquiry

Further investigation located Microsoft's
[Developer Community issue 10111550](https://developercommunity.visualstudio.com/t/Distributing-application-statically-link/10111550),
raised specifically about Community 2022's static CRT distribution terms. Its
August 22, 2022 response relayed a C++ team discussion supporting static linking
and equal use scenarios across Community/paid editions, while acknowledging
uncertainty and possible EULA/list clarification. The questioner challenged that
uncertainty. The issue was subsequently closed at lower priority on November 30,
2023 without a definitive answer in the public discussion. This supports the
conventional interpretation, but is not recorded as a new license grant or a
resolution of the Build Tools-only acquisition.

The public page requires JavaScript. Its server-injected configuration identifies
the anonymous read API used by the site; the response and its hash were retained
under `build/pdfium-controlled/20261025/distribution-followup`. Only publicly
visible comments were used; no sign-in, posting, or account access occurred.

Microsoft's [deployment documentation](https://learn.microsoft.com/en-us/cpp/windows/deployment-in-visual-cpp?view=msvc-170)
also describes distributing statically linked applications. It is supporting
technical guidance, not proof of the applicable developer license or its terms.
The 2026 Build Tools agreement was checked as an alternative reference; it does
not supply an explicit independent Distributable Code grant either and is not
substituted for the 2022 agreement.

The [licensing inquiry](dependency-reviews/20261025/MICROSOFT-DISTRIBUTION-INQUIRY.txt)
now asks the C++ team about the exact Build Tools-only grant, Community fallback,
static-output coverage and external-user terms. The recipient is the
licensing-feedback contact published in Microsoft's
[Build Tools licensing announcement](https://devblogs.microsoft.com/cppblog/updates-to-visual-studio-build-tools-license-for-c-and-cpp-open-source-projects/).
The inquiry contains no credentials, personal paths, attachments, or source code.
The user explicitly authorized sending it on 2026-10-25. Gmail confirmed it in
Sent mail with the approved recipient and subject. A private send receipt is
retained beside the earlier review evidence; Microsoft has not yet supplied a
licensing answer. Sending does not close DEP-04.

The next dependency is that clarification; another EULA summary or an IDE
installation alone does not establish the missing grant. Package activation and
any additional binary terms remain deferred until the applicable basis is known.

## Subsequent Community product setup — 2026-10-25

The user requested the Community licensing route independently of the pending
Microsoft clarification. An actual Windows installer query found a complete
Build Tools 2026 instance, but no registered Community product. This observation
does not establish whether the user held rights on another machine or account.

Microsoft's signed Community 2022 bootstrapper was downloaded from the official
17/release endpoint and installed Community **17.14.42**, with the native desktop
C++ workload, MSVC **14.44.35207** (compiler **19.44.35229.0**) and Windows SDK
**10.0.26100**. Installation returned zero; the product is registered as complete
and launchable, with all three requested workload/component IDs verified. No
restart was requested or performed. The pre-existing Build Tools 2026 product
registration/version and the WSL boot ID were preserved.

The selected use basis is Community 2022's individual-developer provision for
the user's own application, subject to continued eligibility/compliance. No paid
subscription or product key was acquired. Setup evidence and the official license
reference are retained outside the product repository at
`C:\Work\fw-vs-community-20261025`; the repository does not redistribute that
installer or toolchain.

The installed `msvcrt.lib` and `vcruntime.lib` match the earlier portable build
inputs byte-for-byte. This establishes input identity, not retroactive clearance.
The PDFium candidate was not rebuilt or adopted, and its incorporated-code
redistribution conditions still await Microsoft's answer. The earlier inquiry
and review receipts remain historical snapshots of their respective dates/states.

## Community 2026 and bounded LLVM-MinGW follow-up — 2026-10-25

At the user's request, Community **2026 version 18.11.1** was subsequently
installed alongside Community 2022 and the existing Build Tools 2026 instance.
The native desktop workload, current MSVC **14.51.36231**, pinned MSVC
**14.44.35207**, and SDK **26100** are present. Setup returned zero; the requested
components are registered as complete, with no restart required or performed.
Earlier product registrations/versions and WSL's boot ID were preserved. The
2026 setup receipt is outside the product repository at
`C:\Work\fw-vs-community-2026-20261025\ACCEPTANCE.json`.
Use 2026 for general IDE work; existing release recipes still select their exact
pinned tools. This installation neither rebuilds the earlier candidate nor
answers the outstanding redistribution question.

The user also requested a bounded LLVM-MinGW PDFium experiment. LLVM-MinGW
**20261020 / LLVM 23.1.4 / x86_64 GNU ABI / UCRT** was downloaded from its upstream
release and verified against the published asset digest. The experiment read the
accepted PDFium source/generated inputs and wrote only to its own workspace.
Compiler flags were adapted mechanically; LLVM-MinGW's matching C++ runtime
replaced Chromium's MSVC-targeted libc++ objects. No PDFium implementation patch
was applied. The experiment allowed three configuration rounds and a 30-minute
build window, stopping if source-level implementation changes were needed.

All six representative translation units passed the final configuration probe.
The full compile then passed 17 units and failed at `core/fxcrt/fx_memory.cpp:103`:
the aligned allocator checks `COMPILER_MSVC`, sending the Windows GNU target down
the `posix_memalign` branch. Another 998 C/C++ units were not started after that
failure. The header uses the same compiler condition for its malloc declarations
and matching aligned free. Spoofing the Microsoft compiler macro globally would
not be an appropriate fix.

An independent native Windows allocator probe compiled/linked and passed 36
alignment/size cases using `_aligned_malloc` and `_aligned_free`. Its executable
has ASLR/NX/CFG and no VCRUNTIME/MSVCP imports. These are properties of the probe,
not acceptance evidence for a PDFium DLL. The next bounded scope is a platform
portability patch covering allocator selection, declarations and paired freeing
together, followed by resumed compilation to discover any further incompatibility.

**No LLVM-MinGW PDFium DLL was produced.** Export compatibility, PDF rendering,
helper/archive integration, full DLL hardening, and incorporated runtime/license
mapping therefore remain untested for that variant. Neither DLL adoption nor
public-release clearance is implied. Original source diffs and all three existing
default/installed/candidate DLL hashes were verified unchanged.

The compact [experiment record](dependency-reviews/20261025/LLVM-MINGW-EXPERIMENT.json)
points to the receipt under `build/pdfium-mingw/20261025` and the isolated workspace
`C:\Work\fw-pdfium-mingw-20261025`. The downloaded toolchain and bulky build logs
are not product release files.

That configuration-only experiment is historical. The later portability fix and
[maintained GNU build](PDFIUM_GNU_BUILD.md) produced the tested DLL, runtime/source
notice mapping and packages. Default dependency adoption remains a separate step.
