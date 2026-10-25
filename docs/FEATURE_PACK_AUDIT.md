# Feature-pack independent audit — 2026-10-24

This is the repair ledger for GPT Pro's review of the nine feature additions.
It is separate from the closed F01–F21, N01–N14 and M01–M05 cycle in
[Hardening status](HARDENING_STATUS.md). The reviewer established 11 findings
(9 P2, 2 P3), with no established P0/P1. Windows behavior was reviewed from source;
the reviewer did not run Windows executables. Direct integration was added after
that snapshot and was outside the independent review.

Reviewed first-party source: `65d6f664995ca105426caaf7925caa64de0e7b913ebeaff13593ba1a9cade835`.
Returned ZIP: `OpenPilot_Feature_Pack_Independent_Audit_20261024.zip`,
SHA-256 `d8c46d64a7337659e5275e89a290fbaab6d1e20553cdf129affb80693c8acc94`.
The original report and 30 manifest entries were verified at intake. Source
inspection confirmed the cited mechanisms remained present before repair.

**Current status: 11/11 original findings fixed and published.** The sections
below retain earlier per-step observations; final dispositions are in the table.

| ID | Priority | Finding | Status |
|---|---|---|---|
| NEW-001 | P2 | WSL-only settings changes do not revoke retained optional-consumer requests | Fixed and published; targeted acceptance passed |
| NEW-002 | P2 | Archive and ordinary Diff omit the required native-only path rejection before Windows I/O | Fixed and published; targeted acceptance passed |
| NEW-003 | P2 | Partial Linux Largest/Types analysis is published without surfacing its incomplete status | Fixed and published; targeted acceptance passed |
| NEW-004 | P2 | An unstaged Git diff treats a recreated working file as empty when the accepted status snapshot recorded deletion | Fixed and published; targeted acceptance passed |
| NEW-005 | P2 | Space keyboard pane focus can dispatch actions for the other pane's selection | Fixed and published; full Windows/WSL and host acceptance passed |
| NEW-006 | P2 | Lexical overlap removal drops an explicitly selected native mount root | Fixed and published; full Windows/WSL, modeled scope and actual isolated mount acceptance passed |
| NEW-007 | P2 | Normal shutdown can abandon queued deletion of retained archive extraction sources | Fixed and published; process-exit and archive/provider acceptance passed |
| NEW-008 | P3 | Workspace restoration leaves the previous folder watch active because its clear call is rejected | Fixed and published; workspace model/host acceptance passed |
| NEW-009 | P2 | The Duplicates dock button double-counts selected folders and silently refuses the enabled action | Fixed and published; button/overflow exact-root and real-helper acceptance passed |
| NEW-010 | P2 | Archive-nested media keeps playing when the outer archive enters linger | Fixed and published; MF/libmpv source-departure pause and media/host acceptance passed |
| NEW-011 | P3 | Space main-view Types can label retained group totals with the newly requested metric | Fixed and published; gated rendered-metric and full Space/WSL/host acceptance passed |

## NEW-001 and NEW-002 acceptance

WSL-only settings reload now explicitly notifies the five optional consumers.
Content search, Space, Duplicates, ordinary/mixed Diff and Git retire captured
WSL requests and invalidate their sessions; hidden or cached panels cannot start
new work from those requests. Windows-only roots retain their sessions/results.
Cold policy frames update surviving helpers' future work without launching a
helper. Each worker snapshots enablement before launch, so a policy update does
not race a running Windows worker. Re-enabling WSL allows a fresh request.
There is no unconditional WSL dependency on a Windows-only tool.

Archive and ordinary Diff now check the complete outer source path before any
Windows provider open or decoder launch. WSL paths containing the engine's
native-only quote marker are explicitly unsupported, including quoted ancestors,
legacy and extended UNC forms and resolved remote drive roots. Both Diff operands
are checked before opening either. Literal quote-codepoint names in ordinary
Windows namespaces remain legal. Unresolved remote mappings fail admission;
this is admission at request time, not an atomic filesystem/mapping snapshot.

Focused gates exercise the production Archive worker, Diff reader, all five
helper policy handlers and the real `settings_apply → fn_reload` route. Windows
and Linux fixtures are owned and marker-guarded; no physical input, WSL shutdown
or Docker shutdown is used. Existing Archive/Diff behavior is covered by their
module acceptance gates. The final cohort also runs Content, Duplicates, Git and
Space acceptance because their retained-policy handling changed.

Evidence: [batch closeout](../build/feature_audit_fixes/20261024/new001-002/RESULTS.md), including exact source/image
hashes and a scoped acceptance receipt. The conservative impact plan selects
more modules because of shared host/build bindings; these results are **not** a
full-suite or all-selected-modules claim. Earlier failed fixture setup is retained
as historical evidence, superseded only for the corrected setup by final results.

Original repair list complete: NEW-001–011 are fixed and published.
The NEW-005/NEW-006 validation deferrals closed under NEW-008. No functional
validation deferral remains from this returned audit; performance work is separate.

## NEW-003 acceptance

Linux Largest Files / File Types now visibly qualify known-partial detail
snapshots with their own error/exclusion counts in both layouts. Useful rows
remain available; independent folder-summary totals are preserved. Sorting,
filtering, metric changes and type drilldown keep the qualification. A fresh
complete analysis clears it.

[Scoped closeout](../build/feature_audit_fixes/20261024/new003/RESULTS.md): 255
assertions / 22 selectors passed, including injected partial-stream rendering and
actual owned WSL child access withdrawn after a complete summary, then restored.
An earlier run exceeded the unchanged 120 s deadline without retained stdout;
its cause remains unexplained. The runner now preserves timeout output. Unchanged
production/test images passed the next scoped run; that result does not erase
the earlier timeout or establish a timing fix. No full-suite claim is made.

## NEW-004 acceptance

A retained Git deletion row now acquires the working side through the existing
Windows/native guarded reader. Recreated regular files supply current bytes;
only a missing path for a captured deletion supplies an empty working side.
Unsupported types, access/read failures, cancellation and limits remain errors.
Staged sides still use the captured HEAD/index object IDs.

[Scoped closeout](../build/feature_audit_fixes/20261024/new004/RESULTS.md): 155
assertions / 16 selectors / 3 modules passed, including Windows and native WSL
recreation with identical and different bytes, nonempty content over an empty
index blob, still-missing paths, directory type changes, size bounds, cancellation
and the Windows sharing guard. Both repository indexes stayed unchanged; native
fixtures were removed through the existing guarded cleanup. WSL/Docker stayed
running. No full-suite or performance claim is made.

## NEW-005 acceptance and earlier WSL deferral

**Current disposition:** the deferral below is closed by the complete Space
Windows/WSL and host run recorded under NEW-008. Earlier failures remain
historical observations with unproven causes.

Space commands now resolve the highlighted row in the currently focused result
pane, rather than the independent legacy selected ID. Left/Right and forward or
reverse Tab retain the correct target. The row must still belong to that pane's
current result list. Empty/filtered selections and drive-tree, filter or toolbar
focus cannot reuse a previous file target. Legacy side-panel behavior is retained.

[Scoped closeout](../build/feature_audit_fixes/20261024/new005/RESULTS.md): **79
Windows assertions / 7 standing selectors / 2 executables passed**. A bounded
acceptance driver runs the actual main-view, drive-catalog and keyboard-action
selectors; the complete host executable covers its four selectors. The emitted
protocol packets contain the focused path, opcode, directory flag and session;
real host Reveal/Preview routing and root/session guards pass.

Two earlier complete Space-module runs remain non-passing. One reported an
initial native-page error 13 with four consequent pagination assertions; the
other passed pagination but missed four native-listing readiness assertions.
Observed Windows CPU was 74% and 99%; causes are unproven. The user explicitly
requested **deferring WSL checks and finishing this fix**. Those failed reports
are not reused as successful acceptance, including their passing page-replacement
routing checks. The final Windows-only gate does not close full Space/WSL module
validation. No timeout, retry or quarantine budget changed.

The Space validation follow-up is owned by this audit ledger: re-run the full
`tests/space_analyzer.ps1` gate with the current candidate and a fresh result
directory when resources are available and the user resumes WSL checks. Details
and both unresolved observations are retained in
[DEFERRED_WSL.json](../build/feature_audit_fixes/20261024/new005/DEFERRED_WSL.json).
WSL/Docker stayed running and no physical desktop input was used.

## NEW-006 acceptance and earlier native integration deferral

**Current disposition:** the deferrals below are closed by the complete
Duplicate Windows/WSL and host run and the actual private-namespace mount
fixture recorded under NEW-008.

Duplicate searches no longer prune explicit native child roots on lexical
ancestry alone. Parents execute first without changing caller root indices.
A native child is omitted only when a completed scan proves that exact directory
and its ancestors were entered and finished, within the same distro and with
exact decoded Linux component names. Excluded mount roots and scopes beneath
those boundaries retain their requested scan. Repeated root aliases, physical
identity dedup, mount exclusions, source checks and limits remain in place.
Ordinary complete overlap keeps its single scan and existing hardlink counts.

[Scoped closeout](../build/feature_audit_fixes/20261024/new006/RESULTS.md): **96
assertions / 10 selectors / 3 executables passed**. The complete three-selector
scope module injects native mount-exclusion records into the production root
selection code, observes scanner intake, and hashes/compares actual owned Windows
files. Cases include a same-device mount boundary, explicit mount/descendant
roots, reversed request order, UNC aliases, ordinary completed coverage,
unfinished/partial coverage, distro/case/prefix distinctions, encoded names and
cancellation. Four existing Windows duplicate selectors and the complete
three-selector host executable pass too.

Live WSL Duplicate-module and actual mount-fixture integration remain deferred,
consistent with the user's WSL-check deferral. No mount or namespace was created;
the modeled-boundary result is not a live-mount claim. Resume that validation with
available resources and an explicitly authorized isolated mount fixture. The
separate NEW-005 Space validation deferral remains open as documented above.
No WSL/Docker shutdown, physical input, full-suite or timing claim is involved.

## NEW-007 process-exit acceptance

Final shutdown preserves engine-before-source ordering, then drains owned
archive cleanup tasks and the stopped workers that can still queue them. The
wait is outside live window/message handling. Ordinary cleanup remains
asynchronous and exact-path-only; scheduling/deletion failures preserve owned
leftovers for review without recursive removal.

[Scoped closeout](../build/feature_audit_fixes/20261024/new007/RESULTS.md): **210
assertions / 14 selectors / 2 executables passed**. The complete Archive module
adds three owned child-process cases: a real pending ARC_EXTRACT Operations
source, a stopped worker releasing an unconsumed extraction late, and cleanup
queue scheduling failure. Private test-only gates prove final shutdown cannot
return/exit while a cleanup producer or callback is held; source files and
directories are gone after allowed exit. Scheduling failure preserves its exact
owned data and does not strand the drain. The parent validates the synthetic
GUID/leaf before removing that retained test data.

Existing cancellation/completed-copy source lifetime, actual copied bytes,
preview/decoder behavior and provider-admission checks pass. Production contains
no new test gate or diagnostic argument. No WSL/guest/mount checks were resumed,
no physical input was used, and no full-suite, performance or crash-recovery
cleanup claim is made. The NEW-005/NEW-006 validation deferrals remain unchanged.

## NEW-008 acceptance and resumed WSL validation

Workspace replacement and lazy restored-tab activation now use an explicit
per-pane watch clear. It invalidates the old identity before restored-folder I/O
and leaves native stop work on the existing background reaper. A test-only gate
holds actual native teardown while real host restore returns; queued and future
old-folder notifications are rejected, and replacement watches still deliver
real new-folder changes. Missing targets and disabled WSL retain no old watch.

[Scoped closeout](../build/feature_audit_fixes/20261024/new008/RESULTS.md):
**449 assertions / 42 selectors / 7 executables passed**. This includes complete
workspace, Space and Duplicate gates plus actual same-device Linux bind-mount
boundaries. Explicit mounted/deeper roots, reversed requests, UNC aliases and
ordinary overlap preserve exact counts and caller indices. All 24 helper mount
namespaces were private; shared mounts stayed unchanged. Synthetic fixtures were
removed through guarded cleanup. The released native helper is unchanged.

This closes the NEW-005 full Space/WSL and NEW-006 full Duplicate/WSL and live
mount validation deferrals. Earlier failed NEW-005 runs remain recorded; this
pass establishes neither their root cause nor a performance fix. Native inventory
and impact audit pass with 44 modules / 436 selectors; Python identities remain
unchanged. Only the updated host is published. WSL/Docker stayed running, no
physical input was used, and no full-suite or performance claim is made.

## NEW-009 dock duplicate selection acceptance

The dock now preserves the directory count already gathered with captured
MENU paths. Only lazy contexts count selected directory entries, so query and
invocation agree without counting folders twice. Existing frozen-selection,
revision, mixed-type and root-limit guards remain in place.

[Scoped closeout](../build/feature_audit_fixes/20261024/new009/RESULTS.md):
**90 assertions / 6 selectors / 2 executables passed**. Real button and overflow
invocations each emit one request containing the two exact selected roots and
produce a verified duplicate group in the actual helper. Mixed two-folder/two-file
selection remains unavailable; selection changes during overflow submit nothing.
Sixteen roots work, seventeen remain disabled, and single-folder/current-folder
behavior is preserved. The packet observer is confined to the host test build.

Native inventory/impact audit pass with 44 modules / 437 selectors; Python
identities are unchanged and no skip/retry budget increased. Only the updated
host is published. WSL/Docker stayed running and no physical input or full-suite
or performance run was used. Two original findings remain: NEW-010 and NEW-011.

## NEW-010 archive media source-departure acceptance

Both source-departure branches now recognize active top-level or archive
third-pane media and reuse the existing asynchronous pause. The archive layout
and its linger/debounce behavior remain intact; returning does not autoplay.

[Scoped closeout](../build/feature_audit_fixes/20261024/new010/RESULTS.md):
**106 assertions / 9 selectors / 2 executables passed**. For both MF and libmpv,
leaving for a nonpreview file, another archive or normal media receives a real
helper pause acknowledgement while the old archive and third pane are visible.
Matching helper/document/generation and a newer applied ticket distinguish this
from the client's optimistic flag; playback position stabilizes. An unchanged
outer selection keeps playing, and returning during linger stays paused.

Complete muted video/control and real-host acceptance also covers existing
rendering, restricted-helper lifecycle, archive scope/close, poster cache and
native WSL routing/FIFO admission. The test-only reply observer is absent from
production. Audio-device/output tests were not rerun; earlier accepted audio
evidence remains valid for its original scope and no new audio check is deferred.
Native inventory/impact audit pass with 44 modules / 438 selectors; Python
identities are unchanged and no skip/retry budget increased. Only the host is
published; WSL/Docker stayed running, with no physical input or full-suite or
performance run. NEW-011 is the sole remaining original finding.

## NEW-011 retained Types metric acceptance and audit closeout

Main-browser Types now renders each retained group's stored Logical or On disk
aggregate for the selected metric immediately, matching the toolbar and
percentage denominator. An updating notice states that the previous order is
retained until the background query publishes; partial-analysis warnings remain
qualified. Async query ownership, cancellation and the legacy panel are unchanged.

[Scoped closeout](../build/feature_audit_fixes/20261024/new011/RESULTS.md):
**294 assertions / 24 selectors / 2 executables passed**. A deterministic gate
holds the query while the actual renderer is observed in both metric directions.
Known group totals require different percentages and reverse the sorted order.
Before publication, labels/values/percentages agree and the old order remains
explicitly pending. After publication, the new order appears, selected identity
is preserved, and the notice clears. Existing Windows/WSL, partial-analysis and
real-host acceptance also pass.

Native inventory/impact audit pass with 44 modules / 439 selectors; Python
identities are unchanged and no skip/retry budget increased. Only fpspace.exe is
published, with all other release assets unchanged. WSL/Docker stayed running
and no physical input or full-suite or performance audit was used.

**All 11 original feature-pack findings are now fixed and published.** The
NEW-005/NEW-006 deferred validation closed under NEW-008; no new functional
validation deferral remains from this returned review. Each repair retains its
scoped acceptance receipt and historical observations above.
