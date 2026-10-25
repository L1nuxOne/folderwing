# Performance targets and optimization order — 2026-10-21

The initial order starts with **Linux bulk permissions/metadata**, then **cross-WSL small-file copy overhead**. The first metadata transport improvement is now implemented and checked; its results are recorded below. Next is cross-WSL small-file overhead, followed by search result latency, the remaining Close/resize stalls, and preview/folder work. Core selection, sorting and navigation do not justify an optimization project today.

These are the initial engineering goals for the measured machine and workloads. They are **not theoretical minimum times, proven achievable bounds, or newly enforced test limits**. Goals for expensive storage/mutation work are provisional until we measure setup, transport and equally durable storage controls. The existing 16 ms diagnostic and every other standing gate remain unchanged; the three old limit breaches remain open.

The [sealed baseline](PERFORMANCE_BASELINE.md) remains the source of measured values. The separate [annotated ledger](../build/performance_targets/20261021-review/targeted-ledger.csv), [full JSON view](../build/performance_targets/20261021-review/TARGETED_LEDGER.json), [machine-readable target policy](../build/performance_targets/20261021-review/TARGET_POLICY.json) and [complete target tables](../build/performance_targets/20261021-review/targets.md) assign a disposition to all 944 rows. Internal contributors and comparison controls are distinguished from user operations; they do not each receive an invented independent latency limit.

## How to use the numbers

**Ideal** means the typical time we want for that specific operation, normally its median. **Planning ceiling** means an observed-maximum review trigger for a comparable run. Neither changes the current test verdict. For individual UI handlers the ideal applies to the reported call mean; their ceiling applies to the maximum call. Engine microbenchmarks expose one internal median over 20 iterations, so both their diagnostic targets concern that median, not a nonexistent per-call tail.

Response, completion, smoothness and resource use are distinct. This follows [Microsoft's guidance to set scenario-specific goals for time, fluidity and efficiency](https://learn.microsoft.com/en-us/windows/apps/develop/performance/planning-measuring-performance); the numbers here are our own app-specific engineering choices.

The machine reports 75 Hz, giving a 13.33 ms refresh interval. A **4 ms mean / 8 ms maximum UI-handler goal** leaves headroom for other work. Handler time alone does not certify a presented frame. Presentation is a separate unmeasured goal: roughly one refresh interval ideal, two as the initial planning ceiling for simple interactions, with sustained cadence and dropped frames measured separately. Recalculate refresh-dependent goals for other displays.

Most operation samples have only three repetitions. Medians and observed maxima describe them honestly; no p95/p99 is established. The baseline included 3–88% observed CPU load, including the benchmark itself, so small differences and isolated tails need comparable reproduction before code changes. OS caches were not cleared, and process-first-use is not machine-cold.

## Operation goals

All values below are milliseconds unless marked otherwise. Baseline values are medians; handler rows explicitly show mean/max. The planning ceiling is not a pass/fail gate.

| Operation and fixed workload | Baseline | Ideal | Planning ceiling |
|---|---:|---:|---:|
| Startup, first frame, existing cache posture | 51.25 | 50 | 100 |
| Windows navigation, 100 entries | 3.51 | 8 | 16 |
| Windows navigation, 1,000 entries | 4.61 | 10 | 30 |
| Windows navigation, 10,000 entries | 22.88 | 25 | 50 |
| Linux navigation, 100 entries | 6.26 | 10 | 25 |
| Linux navigation, 1,000 entries | 10.78 | 16 | 40 |
| Linux navigation, 10,000 entries | 35.16 | 40 | 75 |
| Selection/end navigation, 10,000 entries | 4.93 | 8 | 16 |
| Size sort, 10,000 entries | 6.23 | 8 | 16 |
| Full nine-character filter sequence, 10,000 entries | 23.37 | 25 | 50 |
| New-tab command dispatch only | 5.64 | 8 | 16 |
| Owned context menu visible | 12.22 | 25 | 50 |
| Settings coherent snapshot | 37.54 | 50 | 100 |
| Operations slot/helper visible, not model readiness | 7.32 | 25 | 50 |
| Bulk rename open, 100-item model | 83.14 | 75 | 150 |
| Bulk rename preview, 100 reviewed results | 29.79 | 25 | 50 |
| Metadata comparison, 1,000 entries per pane | 76.34 | 50 | 100 |
| Windows folder size, 10,000 x 128 B files | 372.22 | 200 | 500 |
| Linux folder size, 10,000 x 128 B files | 177.59 | 150 | 300 |
| Linux permissions opening acknowledgment | 38.10 | 25 | 50 |
| Linux permissions read, 256 files | 4,138.49 | 250* | 500* |
| Linux permissions Apply and verification, 256 files | 4,122.60 | 500* | 1,000* |
| PNG preview, fresh app process | 141.52 | 100 | 200 |
| TIFF preview, fresh app process | 148.09 | 150 | 250 |
| Text preview, fresh app process | 132.88 | 100 | 200 |
| PDF first rendered page, fresh app process | 168.86 | 150 | 250 |
| Windows Everything search, 10,000 files, exact 10 hits | 206.75 | 75 | 150 |
| Windows live search, same fixture | 193.70 | 150 | 300 |
| Linux live search, same fixture | 496.14 | 300 | 600 |
| Idle Close, window destruction, qualified reader/core cases | 13.00–26.11 | 25 | 50 |
| Idle Close, full native process exit, qualified cases | 14.98–33.73 | 50 | 100 |
| Individual UI-handler work | case-specific | 4 mean | 8 max |
| Continuous window resize, SetWindowPos call | 9.8–10.0 run mean; 18.1 worst call | 8 mean | 16 max |
| Native Linux index build, 137,981 entries / 12,181 watches | 7,031.69 | 5,000* | 10,000* |
| Native Linux index queries, measured /usr corpus | 0.276–8.001 | 10 | 25 |
| Native helper peak RSS, same corpus | 41.59 MiB | 64 MiB | 96 MiB |
| Idle app private memory, original two-pane profile | 4.77 MiB | 5 MiB | 5 MiB |

The Close ranges above are ranges of case medians, not within-case percentiles. Unqualified original managed-exit probes are excluded. Process exit under active transfers and historical F19 scenarios require their own evidence.

Navigation ends at the committed folder snapshot. The filter figure covers all nine input characters, not one keystroke. Command acknowledgments generally have an 8/16 ms goal; folder-size command dispatch is separate from its background scan. The full filter-sequence acknowledgment uses the sequence budget rather than the single-command budget. Preview fixtures and reader-ready/raster endpoints must match before comparing; PDF capture overhead is included.

The raw ID `search-linux-indexed-10000` actually used **live fallback** (backend 2, Index.State 0): its 507.13 ms median belongs to Linux live search. It does not fill the indexed-UI coverage gap. The derived ledger explicitly relabels it while preserving the raw identifier and measurements.

## Durable-copy goals

These are provisional investigation goals (*), not demonstrated storage limits. Times cover full CLI lifetime through successful completion under the existing durability contract. Hash verification remains outside the timed interval. Keep all required metadata, file/namespace durability, publication, recovery and source-removal/ACK guarantees.

| Route and payload | Baseline median | Ideal* | Planning ceiling* |
|---|---:|---:|---:|
| Windows to Windows, 1,000 x 4 KiB | 1.366 s | 1.0 s | 2.0 s |
| Linux to Linux, 1,000 x 4 KiB | 2.902 s | 1.5 s | 3.0 s |
| Windows to Linux, 1,000 x 4 KiB | 7.183 s | 2.0 s | 4.0 s |
| Linux to Windows, 1,000 x 4 KiB | 6.646 s | 2.0 s | 4.0 s |
| Windows to Windows, one 64 MiB file | 305.29 ms | 200 ms | 400 ms |
| Windows to Linux, one 64 MiB file | 669.62 ms | 400 ms | 800 ms |
| Linux to Windows, one 64 MiB file | 494.54 ms | 400 ms | 800 ms |

The small-file payload is only 3.90625 MiB, so round trips, setup, metadata and durable publication are plausible cost centers. That is a reason to measure them, not proof that flushes or any one protocol stage caused the delay. The ledger's effective MiB/s is derived from the whole job and gets an equivalent derived target, never a raw NVMe bandwidth claim.

## Complexity and scaling

Keep the workload in the target's identity: backend and actual fallback state, phase/cache posture, entry count, total bytes, name lengths/types, tree depth, metadata contract, query selectivity, watch count, codec/image/page complexity, concurrency and display geometry. A 256-item permission request and a one-file request must not share one unexplained deadline.

For the next focused measurements, use 1/16/64/256 permission items; several folder sizes; and small-file versus large-file copies with the same durability. Treat these models as hypotheses to fit and validate:

- Listing/filter: fixed setup plus work proportional to entries and string processing.
- Sorting: setup plus comparisons approximately proportional to N log N, with comparison cost dependent on sort key/names.
- Folder scans: directories/entries visited plus actual metadata calls; a flat fixture does not qualify deep trees.
- Copies: setup + per-file/per-directory cost + bytes/effective transfer rate + metadata/publication/fence cost. Batching changes the shape; bytes alone do not explain the run.
- Previews: process/setup + read/decode/layout + first useful render; file bytes alone do not predict TIFF/PDF complexity.
- Indexed search: debounce + candidate lookup/verification + result publication; index construction is a separate lifecycle operation.

Do not extrapolate a single 10,000-entry or 64 MiB measurement into a universal deadline. Keep the fixed-workload goals above while calibration is incomplete. Where an equally durable reference floor makes an aspirational goal infeasible, revise the versioned engineering rationale explicitly; never quietly weaken a standing test threshold.

## Goals requiring measurement

| Endpoint/profile still unqualified | Ideal | Planning ceiling |
|---|---:|---:|
| Machine-cold startup, usable first frame | 250 ms* | 500 ms* |
| New tab with verified usable listing | 25 ms* | 50 ms* |
| Exact Operations model readiness | 50 ms* | 100 ms* |
| Actual Linux indexed UI results, 10,000-file fixture | 100 ms* | 200 ms* |
| Next preview using an existing reader | 50 ms* | 100 ms* |
| Bulk rename Apply, 100 same-directory files, verified | 200 ms* | 500 ms* |
| First terminal prompt, controlled ordinary shell | 250 ms* | 500 ms* |
| First 12 visible 256 px thumbnails, frozen image corpus | 150 ms* | 300 ms* |
| Owned drag feedback / drop-context menu readiness | 25 ms* | 50 ms* |
| Stop/cancel accepted and visibly acknowledged | 25 ms* | 50 ms* |
| Safe cancel quiescence, controlled local-copy fixture | 250 ms* | 1,000 ms* |
| Simple interaction to physical monitor presentation, 75 Hz | 13.33 ms* | 26.67 ms* |

An OS-blocked operation can exceed the cancel-quiescence goal; keep a truthful pending state and retain sources rather than pretending completion. Shell extensions, arbitrary user shell startup scripts and uncontrolled codecs/storage require separate profiles. Cold/sustained raw storage throughput has **no defensible numeric ideal yet**: measure an equally durable reference before setting one. The incomplete WM_NULL resize probe needs a successful-send observation before it can certify anything. These are measurement tasks, not assumed product failures or passing rows.

## Ordered optimization work

1. **Linux bulk permissions/metadata.** Read/Apply take 4.138/4.123 s for 256 files, while the Apply handler averages 4.27 ms. In the observed source, `permission_wire` waits on process exit for a nominal 10 ms between pipe-availability checks (`src/wsl.c:1636`). The roughly 16 ms per-item overall rate is consistent with wait granularity, but causal attribution is still unproven. Split launch, request write, native work, reply wait, model publication and teardown first. Then replace avoidable per-item waits or protocol overhead while keeping bounded cancellation, exact reply validation and stale-file checks. Do not introduce busy spinning or global timer-resolution changes.
2. **Cross-WSL small-file transfers.** The largest total delays remain 6.6–7.2 s. Attribute protocol turns, path/metadata work and durable fences independently, then choose batching/session reuse only where evidence supports it. Linux-local small files are the adjacent comparison. All transfer guarantees stay in force.
3. **Search current-result latency.** `app/palette.c:420` deliberately sets a 160 ms query delay. Measure debounce, backend and publication separately; then consider a shorter/adaptive indexed-query delay while bounding live scans and rejecting stale results during rapid typing/backspace/cancel. Qualify the actual Linux indexed UI route separately.
4. **Close and resize stalls.** Preserve the existing failures: permission Close maximum 22.86 ms, grip-panel dispatch 17.821 ms, tree-panel dispatch 17.3754 ms against 16 ms. Repeat affected paths under comparable conditions and attribute teardown/layout/painting and sent-message work. Current ordinary idle process exits do not close historical F19.
5. **Preview setup and folder work.** Look for process reuse, first-use setup and redundant scans after the larger wins. Defer already fast engine loops, native index queries, basic browsing and arbitrary binary-size reduction.

For an implementation change, use focused measurements and the required source-impact/acceptance checks for that surface. Run the consolidated comparison after a coherent batch. This review itself changes documentation and planning artifacts only; it does not need another interactive or full-suite run.

The [review check](../build/performance_targets/20261021-review/REVIEW_CHECK.json) verifies all original ledger fields and statuses are preserved, every row has an explicit disposition, and all 291 artifacts inside the sealed baseline still match their recorded hashes. It is a data-integrity check, not an application test receipt.

## First metadata transport improvement

Completed 2026-10-22; evidence directory retains the 2026-10-21 run-start date.

The nominal 10 ms process wait between metadata pipe checks actually imposed approximately 15.56 ms per steady reply in the before benchmark. The replacement uses one small reader thread per active metadata session and waits for its completion event. The reader blocks on pipe input, validates the exact reply type/ID/size before reading the bounded body, and is cancelled and joined before its handles/storage are released. The existing 30 s request deadline, captured actor, stale-file checks, result verification and helper protocol are preserved. No global timer change or busy polling was introduced; no reader starts until a metadata helper is actually used.

Three repetitions per operation produced these full-editor results on the same private test desktop, with identical host UI source and helper bytes. The old transport was rebuilt as a separate owned control executable; the release app retained the new transport throughout. OS caches were not cleared. Control and new runs were sequential, not an idle-machine or physical-presentation qualification.

| 256-file operation | Old transport median / observed max | New transport median / observed max | Speedup of median |
|---|---:|---:|---:|
| Open editor and read all metadata | 4,131.29 / 4,139.98 ms | 301.73 / 307.34 ms | 13.69x |
| Apply actual mode changes and verify all results | 4,143.35 / 4,144.68 ms | 242.70 / 287.86 ms | 17.07x |

The direct API benchmark includes session/helper startup and teardown. Its 256-item read median dropped from 4,094.90 to 157.57 ms; Apply dropped from 4,092.22 to 173.05 ms. Steady replies dropped from a pooled observed median of 15.56 to 0.260 ms. Batches of 1/16/64/256 distinguish the fixed startup cost from per-item overhead; these samples do not establish tail percentiles or precise CPU attribution.

The full editor's read median remains above the aspirational 250 ms goal and below its 500 ms planning ceiling. Apply meets the 500 ms ideal. Leave those goals unchanged and prioritize the larger remaining transfer cost next.

Validation passed the complete Windows transfer fixture (25 selectors, including four new metadata transport cases), engine and default app executables, 22 Linux permissions/ownership helper contracts, 53 bulk-editor checks and 57 ownership-editor checks. New transport cases cover fragmented/repeated replies, malformed headers without bodies, truncated input, timeout and drained shutdown. All four new native selectors are explicitly classified and impact-bound; Python inventory and existing mappings are unchanged, with no skip/retry budget growth. The private UI checks used owned message targets without switching the visible desktop or sending physical input.

The remaining 12 units in the conservative 15-unit native impact plan are left for batch-end integration under the agreed testing approach; these focused results are not a full selected-suite or full-suite closure. Earlier invalid UI probes and incorrectly profiled helper invocations are retained separately. Corrected UI packaging uses explicit argument vectors and captured fixture output; standing product assertions and timing limits were not weakened. This isolated profile does not erase F19 or the three original baseline limit breaches.

Measurements, exact evidence/profile limits and the remaining execution plan are in the [operation result ledger](../build/metadata_latency/20261021/OPERATION_RESULTS.json), [readable results](../build/metadata_latency/20261021/RESULTS.md) and [run summary](../build/metadata_latency/20261021/CLOSEOUT.json). WSL kept its boot ID; no WSL/Docker stop or reconfiguration was performed.


## First small-file transfer improvement

The 2026-10-22 pass removes redundant checked opens/closes and Windows-source EOF reads. Exact 1,000-file Windows-to-Linux traces remove 1,000 opens, 1,000 closes and 1,000 reads while retaining all 3,008 fsync calls and all source stat/version checks. Skip access/conflict ordering, metadata, publication, ACK and source retention remain intact. Native pseudo-files still stream to EOF.

Three quieter alternating before/final pairs observed a Windows-to-Linux median of 7.256 to 6.605 s (9.0% lower). Linux-to-Windows was inconclusive: its final median was worse, 4.492 to 5.234 s, with wide overlapping ranges; a separate instrumented pair reversed that order and located most variable time in Windows destination end/commit work. Keep that unresolved result visible. CPU across the quieter comparison had a 19.7% median and 54.8% observed maximum; this is not an idle-host or tail-latency qualification. Earlier CPU-saturated samples are retained separately and do not support a speedup claim.

Both cross-endpoint small-file routes remain above the 2 s ideal and 4 s planning ceiling. Further batching/pipelining requires an explicit durability and ACK/source-retention design. All standing limits and original failures stay unchanged. Functional focus passed 126 Linux tests, all 25 Windows transfer selectors / 4,323 checks, and exact 64 MiB copies both directions. Four new Python selectors are explicitly classified and impact-bound; native inventory remains 21 modules / 303 selectors with no skip/retry growth. The remaining 8 of 9 current native execution units stay for batch-end integration (14 pending modules when combined with the earlier metadata step); this is not full selected-suite closure.

Exact samples, source/helper identities, call counts, diagnostic limits and remaining coverage are in the [transfer ledger](../build/transfer_latency/20261022/OPERATION_RESULTS.json), [readable results](../build/transfer_latency/20261022/RESULTS.md) and [closeout](../build/transfer_latency/20261022/CLOSEOUT.json). The original broad baseline stays sealed. WSL and Docker stayed running.


## Native destination reference and commit timing

The 2026-10-22 attribution run used resident-payload destination references calling the current production per-file writers and durability fences. Native core medians for 1,000 x 4 KiB were 2.785 s on Linux and 4.474 s on Windows. Payload and namespace flushes account for 95.5% and 85.1% of those representative core intervals. Source reads/checks, protocol, tree setup and whole-tree ancestry are outside resident-reference scope; process wall values include preload separately. These are serial algorithm cost references, not hardware floors. Arrival cadence/writeback changes flush blocking time, so do not subtract these profiles to infer IPC overhead.

Complete-transfer probes split metadata, prepublication flush, rename, postpublication file/directory flush, cleanup and ACK writes without disabling checks. The representative logged Linux-to-Windows run spent 2.726 s in Windows commit; all ACK writes took 5.37 ms. Sender ACK wait overlaps that commit. 82.6% of the fastest-to-slowest logged run's wall difference occurred inside the first file flush. That identifies a boundary, not a raw device-latency cause. The uninstrumented release medians in this cohort were 6.833/4.461 s for Windows-to-Linux / Linux-to-Windows. Three repeats and varying fence costs do not tightly bound probe overhead or close the earlier reverse-route regression observation.

Keep both 2 s aspirations and all existing limits unchanged. The next experiment is bounded preparation/commit overlap (2/4/8 files) with all required per-file guards and fences retained, ACK only after durable completion, and explicit cancellation/failure/recovery requalification before integration. No production implementation or installed image changed in this measurement turn. All 24 copies passed exact byte/mtime checks; diagnostic copies passed 98 Linux tests and 25 Windows transfer selectors / 4,323 checks. Broader integration remains pending.

See the [reference and commit ledger](../build/transfer_reference/20261022/OPERATION_RESULTS.json), [stage breakdown and scope limits](../build/transfer_reference/20261022/RESULTS.md), and [closeout](../build/transfer_reference/20261022/CLOSEOUT.json). WSL/Docker stayed running; original sealed evidence and failures remain retained.


## Bounded native preparation and commit overlap

The 2026-10-22 isolated reference experiment compared 1/2/4/8 slots, group preparation with serial commit, and concurrent per-file pipelines. Three repetitions per endpoint/configuration (42 timed runs) retained all 3,000 required file/namespace flush calls per 1,000 x 4 KiB copy. Fresh cohort serial medians were 2.759 s Linux and 5.478 s Windows. Eight Linux pipelines reached 0.904 s (3.05x ratio of medians); Windows's best observed mode was eight prepared stages with serial commit at 2.546 s (2.15x). Linux prepare-ahead alone was not consistently beneficial. Windows retains a 15.366 s four-stage preparation outlier; all ranges/samples remain recorded.

20 private qualification cases passed normal scheduling, reported failures at each persistence step, cancellation before/after publication, and termination of owned probe processes. All timed copies verified bytes/mtime and Linux mode, with completion markers only after all required per-file barriers. Failure injection reports errors after real flush calls; owned process termination is not OS/VM power-loss evidence. Abandoned staging remains for review.

These are resident-payload destination core timings. Source guards/removal, real ACK protocol, full-tree ancestry/metadata/overwrite integration are outside scope. Threaded stage totals overlap; source/installed images and test inventory are unchanged. The end-to-end 2 s aspirations and existing limits remain open. Native results support endpoint-specific bounded scheduling, with per-file ownership/version checks, distinct staged/durable state, correlated durable ACKs, cancellation draining and race/recovery requalification before production integration. No production concurrency was enabled.

See the [bounded-overlap ledger](../build/transfer_overlap/20261022/OPERATION_RESULTS.json), [full timing table and scope](../build/transfer_overlap/20261022/RESULTS.md), and [closeout](../build/transfer_overlap/20261022/CLOSEOUT.json). WSL and Docker stayed running.


## Opt-in Linux commit queue prototype

The 2026-10-22 production-source prototype overlaps native Linux payload and namespace fences while one owner retains source reads, Windows validation RPCs, late source/overwrite checks, publication and progress. Per-file held descriptors, version/metadata snapshots and private stages survive until completion. Every successful queued file still requires payload, destination-parent and staging-directory flushes. Cancellation drains published work and joins workers; duplicate-name and directory boundaries drain/retire stages before conflict resolution or final metadata. Moves, Windows destinations, empty/unallocated files and files over 1 MiB remain serial. Explicit Linux-helper environment width 2/4/8 opts in; unset/1 remains serial. Payload staging is bounded to K MiB across queued slots, with separately bounded metadata/paths/handles and the owner's preparation state.

Twelve complete Windows-to-Linux CLI copies (1,000 x 4 KiB; three repetitions per width) include real source reads/guards, native Windows stream-validation replies, tree ancestry and durable completion. They used an unchanged Windows coordinator and a private helper build; installed release images remain unchanged. All copies verified exact source/destination bytes, timestamps and destination modes with no leftover completed-run stages.

| Files in flight | Median | Observed range |
|---|---:|---:|
| 1 | 6.747 s | 6.573–7.143 s |
| 2 | 4.477 s | 4.453–4.788 s |
| 4 | 4.113 s | 4.068–4.372 s |
| 8 | 4.028 s | 3.982–4.115 s |

Eight slots lower the observed median by 40.3% (1.67x ratio of medians); four slots reach almost the same result. Three samples cannot establish a preferred production width or tail guarantees. Whole-cohort CPU median/max was 25.3/83.2%, including setup/verification/resource sampling; no idle-host qualification is claimed. The 2 s ideal remains open and the eight-slot median remains above the 4 s planning ceiling. No standing threshold or historical failure changed.

Eight new standing Linux integration selectors cover scheduling/barrier counts, overwrite/name/directory ordering, source/destination races, failed barriers, cancellation, owned-process interruption, thread-start/FD lifetime and source-ACK rejection. Exact focused outcomes and broader pending native/guest qualification are in the [prototype results](../build/linux_commit_queue/20261022/RESULTS.md), [operation ledger](../build/linux_commit_queue/20261022/OPERATION_RESULTS.json) and [focus receipt](../build/linux_commit_queue/20261022/FOCUS_RECEIPT.json). No parallel move/source-removal or new wire ACK flow is enabled. Full selected integration and guest power-loss qualification remain open; WSL/Docker stayed running.


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

### 2026-10-22 — production integration acceptance, group8

The opt-in group path is in production source; serial remains default and the
installed release is unchanged. The complete native and Linux functional gates
passed (see `build/windows_group_integration/20261022/RESULTS.md`). In one fresh
1,000×4KiB ext4→NTFS acceptance copy per mode, serial took 3843.5784 ms, ordinary8
3331.1946 ms and group8 2068.1121 ms. Exact source/destination bytes, names, mtimes,
root directory time and absence of stages were checked for all three copies.
Group8's production profiler recorded 1,000 pre + 1,000 post NO_SYNC data/metadata
writeouts and 125 pre + 125 post + 125 namespace full fences; tree/ancestry barriers
remain additional and unchanged.

These are correctness-acceptance smoke observations with no new timing admission
or CPU series, not a new benchmark or tail qualification. The preceding private
balanced width8 experiment remains separately sealed. The 2000 ms ideal and 4000 ms
review threshold, historical outliers, original cancellation miss, F19 and
reverse-route uncertainty remain unchanged. No default-on rollout is implied.

### 2026-10-22 — release decision

The private-app release now defaults to eligible group8, following the final
nine-case production-source interruption/ACK cohort and a default-mode installed
copy smoke check. No further benchmark campaign was run or required: the owner
explicitly closed this optimization cycle in favor of feature development. The
previous measurements and targets stay historical reference points, with no new
median/tail claim. Current release evidence lives in
`build/windows_group_release/20261022/RESULTS.md`.
