# Hardening status — 2026-10-22

## Current disposition and remaining list

The later nine-feature GPT Pro review has its own current repair list in
[Feature-pack audit](FEATURE_PACK_AUDIT.md). The F/N/M closeout below does not
claim that later feature findings are closed.

**The hardening cycle is closed for the current private-app release.** No known
functional repair remains outstanding from F01–F21, N01–N14 or M01–M05 within
their documented scopes. F19 remains parked performance work. This is a scoped
engineering closeout, not a guarantee that every possible fault has been tested.

The release is published and running with eligible Linux-to-local-NTFS small-file
copies using group8 by default. The complete integration gate, final nine-case
production-source guest/ACK cohort and installed default-mode copy smoke are
finished. Earlier entries below saying native modules, guest runs, default
selection or release publication are pending describe historical states; they
are superseded by the final closeout.

| ID | Remaining item | Current disposition |
|---|---|---|
| H01 | F19 Close latency and other UI stalls: permissions/comparison dispatch, Operations I/O waits, painting/timers and resize. | Parked performance investigation. The broad baseline retained Close at 22.86 ms and two resize dispatch maxima at 17.82/17.38 ms against the unchanged 16 ms diagnostic. Later passing drag checks do not erase those observations. |
| H02 | Transfer timing variability, including Windows-to-Linux small-file overhead, historical slow-copy outliers and broader workload/tail behavior. | Parked optimization/measurement. Queue/group improvements are implemented and qualified within their scopes; there is no general p95/p99 or all-workload timing claim. |
| H03 | Intermittent native WSL DELETE cancellation delay. | One historical miss of the unchanged 5 s deadline remains unexplained. Later 32- and 128-trial follow-ups passed with sources intact; no cause or timing fix is established. |
| H04 | Search latency and actual Linux indexed-search UI measurement. | Parked tuning/coverage. The broad Linux UI probe exercised live fallback, not the indexed UI. Everything integration itself passed functional acceptance. |
| H05 | Further bulk-metadata, preview, folder-size and comparison tuning against operation-specific goals. | Optional performance backlog. Bulk metadata and transfer improvements already landed; old baseline values must not be presented as current measurements of those changed paths. |
| H06 | Broader measurement coverage: cold-cache startup, exact Operations readiness, terminal/thumbnail/bulk-rename readiness, Stop response and physical input/presentation. | Optional future coverage, not known functional defects. The original N11 changed-view acceptance did not establish source-side physical mouse routing; later complete drag gates are separate evidence. |
| H07 | Physical host/controller/device power loss and interruption of the ordinary WSL environment. | Outside executed recovery scope. Isolated Linux/Windows guest interruption and ACK cases are complete. WSL and Docker were never stopped for these tests. |

All seven entries are **parked and nonblocking for feature development**. Reopen
one for a concrete regression or a requested performance/coverage task; no new
full-suite, benchmark or certification campaign is required before new features.
Historical intermittent PDF, clipboard and boundary-oracle failures retain their
causal uncertainty and later scoped results below; they are not newly open
functional repair tickets.

Current release evidence: [release closeout](../build/windows_group_release/20261022/RESULTS.md).
Prior complete gate: [integration closeout](../build/windows_group_integration/20261022/RESULTS.md).
Performance values and goals: [sealed baseline](PERFORMANCE_BASELINE.md) and
[target ledger with subsequent improvements](PERFORMANCE_TARGETS.md).

## Historical evidence and progression

The sections below retain the chronological evidence and its original scope.
Use the current remaining list above when deciding what is pending today.

This document consolidates existing evidence. It is not a new full-suite run or
an independent audit verdict. Historical failed reports remain immutable; the
machine-readable ledger states which later results qualify particular paths.

The original baseline ledger remains immutable:
`build/consolidated_closeout/20261021-f0893c70b9a6/STATUS.json`, receipt
`d832609da902fca456b4974a7c22ea42904dd5cc3a5202e00e425aaa7a3d11b6`.
Its three pending qualifications are superseded only within the scope recorded
in `build/remaining_ui_acceptance/20261021-615adeb316a4/STATUS.json`.

## Accepted baseline

| Area | Accepted evidence and boundary |
|---|---|
| Broad native/Python integration | Recorded native inventory: 21 modules / 298 selectors; Python: 19 modules / 270 tests. Existing integration and separate owning lanes retain their own input keys and results. |
| Settings, smoke and shell | Latest cold child-session `All`: 160 smoke checks, eight Settings cases / 40 assertions, 10 shell checks. Another Shell run passed 10 checks without another sign-in. |
| Reuse and cancellation | One authenticated session served three fresh runs; competing Run was refused without another request; explicit active-run cancellation and owned cleanup passed. The cancelled third run does not qualify UI assertions. |
| Real Everything integration | Installed Everything 1.5.0.1409a: complete native app tests 2,882/0 with live IPC branch executed; release-app checks 18/0, including indexed results, zero matches, filters, syntax recovery and missing-instance live fallback. |
| Guest interruption follow-up | N03 NTFS named streams: 8/8; N04 Linux recent nested merges: 8/8. Historical accepted total across scoped cohorts: 104 scenarios / 136 abrupt endpoint power-offs. No physical device-power-loss certification. |
| Active controller interruption | Watchdog logged off the exact owned child after controller termination. Stale reuse was refused. Manual recovery removed only the matching task, restored the original feature state and retired the matching stale pointer. Interrupted acceptance remains unqualified. |

The follow-up adds one native UI-acceptance selector: the current inventory is
21 native modules / 299 selectors and 19 Python modules / 270 tests. The only
release image rebuilt for this follow-up is `drag_tests.exe`; production runtime
and launcher controls are unchanged. Existing integration and later owning lanes
retain their own exact input keys; an older full-suite receipt is not a current
complete-source receipt.

Key evidence:

- Broad integration: `build/functional_remaining/20261021/CLOSEOUT.json` and its linked frozen integration.
- Guest follow-up: `build/guest_hardening/CLOSEOUT-20261021.json`.
- UI reuse: `build/child_ui_acceptance/20261021-f1f38ca66a10/CLOSEOUT.json`.
- Everything: `build/everything_acceptance/20261021-47a21af93829/CLOSEOUT.json`.
- Controller recovery: `build/session_lifecycle/20261021-b8b76dddd4a2/CLOSEOUT.json`.

## Original F01–F21

These are implementation/verification summaries, not a claim that an independent
reviewer has re-audited every fix. The original report and exact per-item titles,
priorities and evidence references are retained in the canonical ledger.

| Finding | Current disposition |
|---|---|
| F01 verified staging substitution | First-pass repair plus guarded staging, ACK/source-removal and crash evidence. |
| F02 changed source removed after copy | First-pass repair plus changed-source and removal-order controls. |
| F03 cancelled overwrite removes old target | Guarded publication repaired; N03 separately strengthens named-stream barriers. |
| F04 reparse traversal during merge | COPY/MOVE scope repaired; N02 separately repairs DELETE. |
| F05 mounted-subtree recursion | Mount policy repaired; N07 separately closes comparison traversal. |
| F06 nonreversible Linux names | Exact reversible mapping repaired; N01 separately fixes Windows suffix normalization. |
| F07 terminal UTF-16 copy allocation | Allocation/output/failure controls implemented and verified. |
| F08 borrowed menu operands | Owned context repaired; N11 separately fixes pending gesture view identity. |
| F09 changed visible command definition | Revision-bound execution repaired; N10 separately fixes Settings recorder targets. |
| F10 queued WSL actor drift | Actor/configuration captured at job acceptance. |
| F11 session/terminal exact Linux paths | Exact-path transport and codec controls implemented. |
| F12 non-atomic settings conflict check | Atomic transaction and competing-writer controls implemented. |
| F13 partial shell-helper target framing | Complete framing and target-validation controls implemented. |
| F14 unbounded preview disposal/write | Bounded launch/writer cancellation and generation ownership implemented. |
| F15 PDF EOF leaves dead transport | Recovery repaired; ordinary PDF/smoke paths pass. Historical intermittent causes remain recorded. |
| F16 token setup fails open | Fail-closed token setup and failure controls implemented. |
| F17 PDF filename/content identity | Identity/change handling repaired; current PDF/smoke evidence retained. |
| F18 optional build publication mismatch | Build-output/publication contracts implemented. |
| F19 Close timing | **Still open as performance work.** The unchanged threshold and failed samples remain. |
| F20 incomplete/misattributed budget measurements | Completeness/attribution contracts repaired; performance failures stay failed. |
| F21 stale clipboard Operations oracle | Fresh operation/readiness-bound assertions implemented; owning Operations acceptance passes. |

## Follow-up N01–N14 and metadata

| Finding | Current disposition |
|---|---|
| N01 Windows operand normalization | Implemented; targeted and integration evidence retained. |
| N02 DELETE substitution traversal | Implemented; targeted and integration evidence retained. |
| N03 unchecked named-stream barriers | Implemented; native controls plus eight accepted guest-interruption cases. |
| N04 Linux nested-merge ancestry | Implemented; public/native controls plus eight accepted guest-interruption cases. |
| N05 late destination guard reports success | Implemented; eight actual controlled-boundary cases / 452 assertions passed. Earlier unobserved RUNNING-boundary failure is not causally explained. |
| N06 incomplete successful Linux listing | Implemented; integration evidence retained. |
| N07 comparison bind-mount traversal | Implemented; integration evidence retained. |
| N08 Settings commit changes click meaning | Implemented; actual child-session click/save acceptance passed. |
| N09 Cut removes unpublished draft | Implemented; component, clipboard and actual host acceptance passed. |
| N10 stale recorder/capture after reload | Implemented; actual child-session reload/accelerator cases passed. |
| N11 stale pending mouse gesture | Implemented; six changed-view sequences pass on owned parent windows with the ordinary menu and real registered OLE target cancel/copy. Source-side physical mouse routing remains outside this qualification. |
| N12 delayed paste changes logical field | Implemented; same/switch/reopen/reload actual host cases passed. |
| N13 Copy path reports false success | Implemented; publication/failure and actual host controls retained. |
| N14 CLI operand overflow | Implemented; targeted and integration evidence retained. |

M01 Linux ACL/xattr preservation, M02 NTFS streams crossing into Linux, M03
streamed Windows overwrite DACL, M04 Windows directory streams and M05 one-unit
Windows names have implemented repairs and scoped verification. Their individual
closeouts and later metadata/recovery coverage are linked in the ledger.

## Functional follow-up and remaining work

The real five-minute idle expiry passed while 11 public Status calls did not
extend idle. The real one-hour maximum passed with a four-hour idle allowance
and no test requests. Both used unchanged clocks and public bounds; independent
receipt, process, child-session and feature-state verification passed. The
originally enabled child-session feature stayed enabled.

N11's six left/right x refresh/sort/filter sequences passed all 288 assertions
using newly owned parent Default windows, the actual ordinary context menu, actual registered
`IDropTarget`/`CF_HDROP` Escape cancellation and Copy here. Both selected files
completed with zero job errors and exact bytes. This observes stale source
dispatch refusal; it does not certify source-side physical `DoDragDrop` routing.

The new copy oracle initially read published destinations before their jobs
finished. Failed evidence remains; the corrected oracle waits for the actual
terminal event under the existing deadline before its one-shot byte checks.
The private child desktop rejected the existing elevated durability broker
launch. The final lane therefore uses the normal parent desktop with explicit
user input idleness; no durability or broker custody check was weakened.

Evidence and per-gate receipts:
`build/remaining_ui_acceptance/20261021-615adeb316a4/STATUS.json`.
Native governance audits passed with no unbound/unclassified selectors or skip
budget growth. This follow-up did not rerun the complete native/Python suites,
the full default drag suite, guest interruption cohorts or performance gates.

**Performance remains:** F19 Close latency, bulk-permissions/folder-comparison
dispatch failures, Operations I/O waits, painting/timer stalls, throughput and
index budgets. Existing limits and failed samples are unchanged.

A later broad observational run records current values in
[the performance baseline](PERFORMANCE_BASELINE.md). Its three remaining
limit breaches and explicit coverage gaps are retained; this measurement pass
does not close F19 or the optimization backlog.

The subsequent [operation target review](PERFORMANCE_TARGETS.md) keeps those
measurements sealed and sets separate planning goals. Its initial optimization
order is Linux bulk metadata, cross-WSL small-file overhead, search result
latency, Close/resize stalls, then preview and folder work. These goals do not
change standing thresholds or qualify unmeasured endpoints.

The [first metadata transport improvement](PERFORMANCE_TARGETS.md#first-metadata-transport-improvement)
removes per-file polling waits. In matched private-desktop editor samples,
256-item reads improved from 4.131 s to 302 ms and verified Apply from 4.143 s
to 243 ms. Bulk/ownership acceptance and focused native/helper checks passed;
the conservative broader native plan remains for batch-end integration.
Original performance failures and limits remain recorded.

The [first small-file transfer improvement](PERFORMANCE_TARGETS.md#first-small-file-transfer-improvement)
removes 3,000 redundant filesystem calls per 1,000 Windows-to-Linux files while
retaining all measured flush and version-check calls. Three quieter alternating
pairs observed 7.256 to 6.605 s in that direction. Linux-to-Windows remains
inconclusive: its final median was worse (4.492 to 5.234 s) and variable Windows
commit time dominates an additional diagnostic pair. Both 2 s transfer goals
remain open. Final functional checks passed 126 Linux tests, 25 Windows transfer
selectors / 4,323 checks, and 64 MiB byte/hash checks in both directions. Four
new Python tests are classified and impact-bound; no skip/retry growth. The
remaining 8 units of the current native impact plan remain for batch-end integration
(14 pending modules across both optimization steps).
This focused pass does not close the full selected gate or historical failures.
See the [transfer results](../build/transfer_latency/20261022/RESULTS.md).

The subsequent [native-reference/commit attribution](PERFORMANCE_TARGETS.md#native-destination-reference-and-commit-timing)
measured resident serial-writer medians of 2.785 s on Linux and
4.474 s on Windows for the 1,000-file fixture. Flushes dominate;
ACK packet writes are small and sender ACK waits overlap destination commit.
These are scoped cost references, not hardware floors. All 24 copies and the
98 Linux / 25 Windows diagnostic test cases passed. No production code/image,
threshold or test inventory changed. Both 2 s aspirations, the earlier reverse-route
variance/regression observation and batch-end integration remain open. The next
experiment was subsequently tested in isolated references: eight Linux pipelines
reached 0.904 s versus 2.759 s serial, and eight prepared Windows stages
with serial commit reached 2.546 s versus 5.478 s. All required flush calls
remained. The 42 timed copies and 20 private qualification cases passed; a
15.366 s Windows outlier is retained. Source/ACK/removal and full-tree production
integration remain unimplemented/unqualified by these resident references.
No production image, protocol or timing limit changed. See the
[bounded-overlap results](../build/transfer_overlap/20261022/RESULTS.md).


The subsequent 2026-10-22 Linux copy prototype keeps source/protocol/publication
under one owner and overlaps only per-slot durability work. Serial mode stays
the default; 2/4/8 slots opt in through the Linux helper environment. Every
queued file retains its held handles and snapshots through all three fences.
Complete 1,000-file Windows-to-Linux medians were 6.747/4.477/4.113/4.028 s for
1/2/4/8 slots, with all twelve copies verifying exact bytes/metadata. Eight slots
observed 40.3% lower median time, but the 2 s ideal and 4 s ceiling remain unmet
at the median. Installed release images are unchanged. Eight new queue selectors
have exact ownership and impact contracts; thread-aware fixture recipes are
explicit. Broader integration now carries 15 pending native modules on the
latest snapshot, plus guest interruption qualification. Parallel moves and
Windows preparation queues remain future work. See the
[guarded prototype closeout](../build/linux_commit_queue/20261022/RESULTS.md).


For ordinary work over several hours:

```powershell
pwsh -NoProfile -File C:\Work\openpilot\tools\child-ui.ps1 -Action Start -IdleMinutes 240 -SessionHours 8
```

Run commands reuse the live authenticated session. Windows authenticated the
latest sessions automatically after the user's independent Credential Guard
change; that observation is conditional, not a universal sign-in guarantee.
No password or authentication token is saved by the tool. Both expiry sessions
are closed and independently verified.

Historical A01–A06 and earlier symlink, PDF paragraph/layout/empty-copy, menu,
clipboard Cut and N05 boundary observations remain retained wherever their cause
was never established. Current passing paths are evidence for those profiles;
they do not erase old failures. Selective independent source-review coverage
also remains selective.

## Preserved contracts and environment

Abandoned/ambiguous staging stays for review. There is no automatic transfer
resume, ACK replay, staging deletion or all-or-nothing tree rollback. Completed
children can coexist with retained unfinished sources. Documented POSIX final
check/unlink and storage/cache limitations remain.

Private-child UI results do not certify physical-console/GPU/DPI behavior or
other Everything versions. Google Drive and canvas/detachable panels are separate
future work.

WSL and Docker stayed running with the same WSL boot ID. There was no guest
interruption, production runtime/launcher-control change or skip budget increase.
Only the native modal test image was rebuilt; its new selector has explicit
classification and impact bindings. This documentation update follows the
sealed execution evidence.


## Linux queue selected integration and guest recovery

The 2026-10-22 follow-up passed all 15 pending native modules (287 selectors,
35,430 checks) on fresh private builds, plus 12 actual abrupt same-guest Linux
queue-copy recoveries. Widths four/eight cover payload, publication/parent,
staging-directory and committed-file boundaries with 32-file overwrites, exact
bytes, modes, nanosecond mtimes, POSIX ACLs and user xattrs. Previously committed
files survived and all sources remained intact. The complete parent drag run
passed after explicit idle readiness; UI dispatch maximum 4.6056 ms satisfies
the unchanged 16 ms limit. Fixture/layout, child-elevation and private serial
harness failures and repairs remain recorded. This closes the old 15-module
native batch plan; full-suite, physical storage power loss, parallel moves and
future Windows queue coverage are not implied.

The Linux queue remains opt-in, and installed release images are unchanged.
The recorded guest is Off with its original resources/services restored; the
owned NTFS test VHDX is detached, staging/data are retained and the authenticated
child session remains reusable within its existing expiry. WSL and Docker were
not stopped or reconfigured. Existing goals/limits, F19 and reverse-route timing
uncertainty remain open. See the [integration and recovery closeout](../build/linux_queue_acceptance/20261022/RESULTS.md)
and [current batch closure](../build/linux_queue_acceptance/20261022/BATCH_CLOSURE.json).


## Opt-in Windows preparation queue prototype — 2026-10-22

Protocol v7 separates bounded FILE_PREPARED credits from correlated, serial
FILE_COMMITTED durable results. Small new-file copies can opt into widths 2/4/8;
all per-file/directory guards and fences remain. Moves, replacements, selected
files and other unsupported cases stay serial. Sources remain held/validated
through durable results, and cancellation/error paths drain accepted credits.
Installed binaries and the default serial policy are unchanged.

Focused qualification passed 167 Linux executions, all 28 Windows transfer
selectors in each of serial/queue modes (10,039 checks), four actual owned
coordinator process interruptions, and twelve whole-CLI copies with exact
source/destination byte/mtime verification. All sources and abandoned stages are
retained. Native inventory is 21 modules / 306 selectors, with eight new Python
selectors and three new native selectors explicitly classified/bound. WSL and
Docker stayed running, no desktop input was used, and prior sealed evidence
remains unchanged. This is focus/prototype evidence, not integration acceptance.

The 1,000 x 4 KiB Linux-to-Windows medians were 6.203 s serial, 4.145 s width2,
3.609 s width4 and 3.559 s width8. Serial ranged 3.101–22.687 s; its fastest sample
beats all queued samples. The observed median improvement is provisional; no
reliable speedup/production width or tail claim is established by three repeats.
The 2 s ideal remains open and width8 maximum 4.417 s crosses the unchanged 4 s
planning review ceiling. Existing timing failures, F19 and reverse-route variance
remain open.

Remaining before rollout: 14 selected native modules / 262 selectors, actual
Windows guest queue interruption recovery, and a steadier timing comparison.
The old v6 receipts do not qualify the new v7 helper/runtime pair. See the
[prototype results](../build/windows_prepare_queue/20261022/RESULTS.md),
[ledger](../build/windows_prepare_queue/20261022/OPERATION_RESULTS.json) and
[pending integration plan](../build/windows_prepare_queue/20261022/BATCH_INTEGRATION_PLAN.json).


## Windows preparation queue integration progress — 2026-10-22

The follow-up has complete passing results for 13 of the 14 remaining native
modules: 234 selectors /30,186 checks. App/watch used the existing authenticated
child session. The complete normal-desktop drag module (28 selectors) awaits the
user's idle-readiness reply; it has not run. The batch therefore remains open.

One directory test observer was updated to count v7 FILE_COMMITTED durable
results while continuing to ignore FILE_PREPARED credits; its error-count and
source-retention assertions are unchanged. The repaired full module passed
548 checks. Cross-endpoint/image fixture packaging was corrected. Core engine
first missed its unchanged 5s native WSL DELETE cancellation deadline, then
passed its complete diagnostic rerun (1049 checks) without a deadline or product
change. Terminal cancellation and source retention passed both times; the
intermittent latency cause remains unresolved. All original failures are kept.

Individual immutable complete-module receipts and unchanged compiled input/binary
checks support the coverage ledger. No reusable successful aggregate selected
or full gate is claimed. All 34 installed images and 1968 older sealed artifacts
are unchanged. The owned NTFS VHDX is detached, fixture/staging data retained,
child session retained and no owned test processes remain. WSL/Docker were not
stopped, and the WSL boot ID is unchanged.

The default stays serial. Windows guest queue interruption qualification, the
intermittent cancellation observation and a steadier timing comparison remain
open; existing F19/reverse-route/timing observations and targets are unchanged.
See the [integration results](../build/windows_queue_integration/20261022/RESULTS.md)
and [current batch status](../build/windows_queue_integration/20261022/BATCH_CLOSURE.json).


## Windows queue unattended follow-up — 2026-10-22

The remaining drag module passed, completing passing evidence for all 14 pending
native modules (262 selectors /31,337 checks). Dispatch maximum was
4.1449 ms against the unchanged strict 16 ms gate; clipboard restored.
The existing 13-module evidence was independently revalidated, not rerun or
converted from failed aggregate receipts.

Thirty-two private cancellation trials and an instrumented complete engine run
(1049 checks) passed. The original 5 s deadline miss remains unresolved and kept;
no product patch/deadline relaxation was made.

Ten actual isolated Windows guest destination-queue power-offs passed, widths 4/8
at five staging/flush/publication/durable-result boundaries, with exact source
and committed-prefix bytes/mtime and final reobservation. 28 invalid oracle
controls were rejected. An initial hidden-stage reader failure was corrected in
a private reader and preserved; fresh case IDs were used. The real destination
writer uses a controlled source driver: full real-Linux/two-guest v7 wire/ACK
interruption and physical power-loss qualification are not claimed. Guest is
Off with original 4 GiB/two CPUs, no network/media; all stages/data retained.

The subsequent 21 whole-CLI copies verified all 21,000 file instances (one warmup,
five timed repetitions per serial/2/4/8 width). Median/range details, matched CPU
samples and unchanged 2 s ideal / 4 s planning ceiling are in the
[follow-up ledger](../build/windows_queue_followup/20261022/RESULTS.md).
No preferred production width/tail guarantee is certified. Default remains
serial and 34 installed images are unchanged. All 2,275 prior sealed artifacts
were reverified. WSL/Docker stayed running; no inventory/skip budget change.

The original intermittent cancellation delay, further workload/tail and target
work, F19/reverse-route uncertainty and the explicitly narrower guest scope
remain visible. [Current native batch closure](../build/windows_queue_followup/20261022/BATCH_CLOSURE.json).


## Real v7 endpoint queue closeout — 2026-10-22

All 11 isolated Linux/Windows queue wire/ACK cases passed after actual power-offs
of both owned guests, with later independent final observation: 90 sources exact,
20 expected public destination files exact, 44 invalid observations rejected,
no application replay, staging retained. Full production Linux helper and native
Windows writer; private serial launch adapter and encrypted SMB3 namespace bridge
replace WSL launch/DrvFS. Copy-only queue scope; physical power loss and ordinary
WSL shutdown are not claimed. Both guests Off with original resources, no NIC/DVD;
owned ephemeral networking/account/credential removed. No WSL/Docker stop.

128 native DELETE cancellation trials (64 unchanged-runtime, 64 instrumented)
passed the original 5s deadline with sources intact. The original intermittent
miss remains unexplained and retained. 17 instrumented CLI copies independently
verified 17,000 file instances. Width4 median/range: 3.144 s /
3.085–3.237 s. The earlier 15.502s sample had
moderate CPU readings throughout; no syscall cause or product fix is established.
Call timings and exact limitations are in the [completion ledger](../build/windows_queue_completion/20261022/RESULTS.md).
The 2s ideal/4s planning ceiling, serial default, 34 installed images and prior
2,598 sealed artifacts are unchanged. Original latency/F19/reverse-route and
broader workload/target observations remain open; no new aggregate/full pass.


## Private Windows group-commit experiment — 2026-10-22

A private paired v8 CLI/helper prototype groups data+metadata writeouts around
checked full pre-publication, post-publication and captured-parent fences. Fresh
held-source checks precede each publication; durable replies follow all fences.
The published prefix is fenced on cancellation/later failure; sources stay held.
Production v7 source, serial default and 34 installed images remain unchanged.

24 real-peer cases, 16 modeled-peer protocol cases, 14 malformed-command checks
and nine paired isolated-guest power-offs passed, with 36 invalid observations
rejected and independent final recovery reobservation. One pipe-admission failure
was retained before any application forwarding; a new identity was used after
a bounded transport admission repair. No armed transfer was replayed.

26 CLI copies independently verified 26,000 file instances (including matched width8). Preparation-only4
median: 3.389s; group4: 2.183s;
group8: 1.959s. Balanced cohort values, phase costs,
resource profile, exact scopes and limitations are in the
[private group ledger](../build/windows_group_commit/20261022/RESULTS.md).
The 2s ideal/4s review ceiling and original intermittent observations remain.
No product selected/full gate or rollout is claimed. Guests restored Off with
original resources and no NIC/DVD; owned ephemeral credentials/network removed;
all test data retained; WSL/Docker stayed running. Older 2,997 artifacts unchanged.

### 2026-10-22 — opt-in Windows group commit integrated

Production source now contains the copy-only bounded NTFS group-commit path
behind `OPENPILOT_WINDOWS_GROUP_COMMIT=1` plus preparation width 2/4/8. Serial
remains the default; installed release images were not changed. Engine/helper
protocol v8 adds strict correlated groups and fresh per-publication source
checks. No source-removal or durability barrier was relaxed.

The integration gate passed all 21 native modules (310 registered selectors;
46,178 checks including the additional group8 writer run), 339 Linux test
executions (294 unique selectors), native governance audit/self-check (32) and
recovery-controller contracts (40). Three production CLI copies independently
verified 3,000 destination file instances and 1,000 retained sources, with exact
bytes/times/names and no stages. Recovery's stale protocol-v6 oracle and missing
bundle-header inventory were repaired strictly to v8. Retained failed resource,
fixture and compile attempts are explicit non-passing records.

Evidence: `build/windows_group_integration/20261022/RESULTS.md`, per-gate receipts
and sealed closeout. WSL/Docker remained running; all 34 installed images stayed
unchanged; the owned VHDX was detached with data retained, the external clipboard
restored, and the authenticated child session retained. 3,632 prior sealed
artifacts were rechecked. No new guest power-off cohort was run against these
production binaries; the prototype's nine-case power-off evidence stays under
its own exact snapshot. Default enablement, installed publication and further
controlled performance/tail qualification remain separate work. Earlier timing
outlier/cancellation/F19/reverse-route uncertainties are not closed by this gate.

### 2026-10-22 — private-app release and hardening closeout

The final production-source v8 recovery cohort passed all nine paired isolated
Linux/Windows guest interruption and ACK cases, with 36 rejected invalid
observations and a final independent recovery observation. All 74 sources
remained exact; 31 recovered public files were verified and
22 staging files retained for review. WSL and Docker were not stopped.

The release at `C:\Work\openpilot\build\release` was updated, and the running
OpenPilot window closed normally and reopened with its saved session. Eligible
Linux-to-local-NTFS small-file copies now default to group8. Ordinary transfer
fallbacks and serial/ordinary environment overrides remain available. The final
installed launch and one default-mode copy verified 64 source/destination files
exactly, including names, bytes and timestamps. The previous release is retained
under `build/windows_group_release/20261022/previous-release`.

At the owner's explicit request, this closes the hardening/optimization cycle.
No additional full-suite, benchmark or tail-qualification campaign is a feature
development prerequisite. Historical performance observations remain recorded;
revisit them only for a concrete regression or a requested performance task.
Evidence and release hashes: `build/windows_group_release/20261022/RESULTS.md`.
