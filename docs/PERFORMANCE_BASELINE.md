# Performance baseline — 2026-10-21

Latest broad observational baseline: `20261021-3ed43ace954f`. The ledger contains 944 metric rows, including 115 primary measurements and 772 internal timing zones. Each row records its phase, unit, workload, sample count, observed value, existing limit and raw evidence. Ideal targets and acceptable ceilings remain unset in this sealed measurement ledger. The subsequent [target review and optimization order](PERFORMANCE_TARGETS.md) supplies separate planning annotations without rewriting the baseline or its verdicts.

Full [readable ledger](../build/performance_baseline/20261021-3ed43ace954f/ledger.md), [CSV ledger](../build/performance_baseline/20261021-3ed43ace954f/ledger.csv), [JSON ledger](../build/performance_baseline/20261021-3ed43ace954f/LEDGER.json) and [closeout](../build/performance_baseline/20261021-3ed43ace954f/CLOSEOUT.json).

One benchmark driver ran at a time against the unchanged release images. Machine: Ryzen 5 5600X, approximately 32 GiB RAM, Radeon RX 6600; reported display 3440 × 1440 at 75 Hz. OS caches were not cleared. CPU observations ranged 3–88% including workload/harness activity; external contention was not separately attributed. These measurements establish an observed reference, not a controlled idle-machine or tail-latency qualification.

| Scenario | Median | Observed maximum | Samples |
|---|---:|---:|---:|
| Startup: first frame | 51.25 ms | 61.10 ms | 10 |
| Navigate: Windows,10,000 files | 22.88 ms | 32.33 ms | 3 |
| Navigate: Linux,10,000 files | 35.16 ms | 35.29 ms | 3 |
| PNG preview: process first use | 141.52 ms | 144.91 ms | 3 |
| TIFF preview: process first use | 148.09 ms | 152.77 ms | 3 |
| Text preview: process first use | 132.88 ms | 142.82 ms | 3 |
| PDF: rendered page available | 168.86 ms | 193.87 ms | 3 |
| Everything query:10,000-file folder | 206.75 ms | 247.43 ms | 3 |
| Live Windows search:10,000 files | 193.70 ms | 196.82 ms | 3 |
| Live Linux search:10,000 files | 496.14 ms | 501.41 ms | 3 |
| Compare metadata:1,000 files/side | 76.34 ms | 76.54 ms | 3 |
| Folder size:10,000 Windows files | 372.22 ms | 424.73 ms | 3 |
| Folder size:10,000 Linux files | 177.59 ms | 182.35 ms | 3 |
| Linux permissions: read 256 files | 4.138 s | 4.161 s | 3 |
| Linux permissions: Apply 256 files | 4.123 s | 4.123 s | 3 |
| Copy: Windows to Windows,1,000 × 4 KiB | 1.366 s | 1.669 s | 3 |
| Copy: Linux to Linux,1,000 × 4 KiB | 2.902 s | 3.054 s | 3 |
| Copy: Windows to Linux,1,000 × 4 KiB | 7.183 s | 7.279 s | 3 |
| Copy: Linux to Windows,1,000 × 4 KiB | 6.646 s | 7.000 s | 3 |
| Copy: Windows to Windows,64 MiB | 305.29 ms | 311.33 ms | 3 |
| Copy: Windows to Linux,64 MiB | 669.62 ms | 695.61 ms | 3 |
| Copy: Linux to Windows,64 MiB | 494.54 ms | 516.26 ms | 3 |

Navigation starts at address Enter and ends at a request-specific committed folder snapshot; typing and physical pixel presentation are excluded. Preview timings use fresh app processes with warm/uncontrolled OS caches. Image/text endpoints are ready-reader states/titles; PDF uses a distinct rendered-page capture endpoint, including capture overhead. Search includes query debounce. Copy timings include full CLI process lifetime and current durability guarantees; exact source/destination hashes were verified outside the timed interval.

## Existing limits still exceeded

| Workload | Measured maximum | Existing limit |
|---|---:|---:|
| resize-grip-panel / `ui.dispatch` | 17.82 ms | 16.00 ms |
| resize-tree-panel / `ui.dispatch` | 17.38 ms | 16.00 ms |
| ui-extra / `permissions.command.close` | 22.86 ms | 16.00 ms |

The existing general budget lane passed:10 startup samples, three browsing workloads with a 10.114 ms worst dispatched message and no over-16 ms events; idle private memory 4.77 MiB. Other workloads above retain their failed limits. No threshold was changed. F19 and the performance optimization backlog remain open.

## Interpretation and retained limitations

- Three planned repetitions support medians and observed maxima; they do not establish reliable p95/p99. Internal aggregate profiles expose call means and maxima only.
- The private Linux indexed-UI attempt used live fallback (backend 2, Index.State 0). Native index measurements are separate: 137,981 entries built in 7.032 s; native query medians 0.276–8.001 ms; peak helper RSS 41.59 MiB, excluding kernel watch memory.
- Earlier invalid probes remain: missing permission-key binding, an unverified managed exit-code read, waiting for a revision after an identical settings rewrite, and the PDF empty-caption oracle. Corrected/continued phases have separate frozen runner keys and retained outputs; no product retry or threshold relaxation was introduced.
- A process/slot becoming visible is not proof of a ready Operations model. First terminal prompt, thumbnail-gallery readiness, bulk-rename Apply, cancellation/Stop response and physical source drag/presentation remain explicitly unmeasured.
- All 794 source files and 141 release files were unchanged during execution. No owned Windows app/helper remained afterward. WSL kept the same boot ID; no WSL or Docker stop/reconfiguration was performed.
- Completed copy destinations were hash-verified again and removed only within exact owned fixture roots. Sources/UI fixtures and failed-probe context remain for review.

No application code, performance thresholds, selector identities, classifications or skip budgets changed. This documentation was written after the execution snapshot was verified.
