# Disk-space analyzer

The on-demand **Space** analyzer replaces the main browsing area with a drive
tree and two linked result panes. The **← Files** button, Escape, or closing Space
restores the existing file tabs and browsing layout. Choosing a different tool
restores ordinary browsing with that tool. The analyzer remains a separate helper.

## Use

Open **Size → Analyze space** from the action dock, **Space analyzer** (`space.show`)
from the command palette, or **Analyze space** on a folder/drive context menu.
An explicit folder command keeps that folder as its captured analysis root.
Opening Space for the first time through `space.show` starts at the current
Windows drive or WSL distribution root; reopening retains the previous root.

- The tree contains Windows drive roots and registered WSL2 virtual disks, with
  measured **free / total** capacity beneath each entry. Selecting a drive scans
  that root. Unavailable capacity is labelled and never presented as zero free.
- The first result pane lists the current scope's immediate children. A single
  folder click fills the second pane with that folder's immediate children,
  without rescanning or changing the first pane's scope.
- Double-click or Enter drills into a folder. Backspace or Up returns to its
  parent; Root returns to the captured analysis root. Right/Left moves between
  result panes. Tab cycles through drives, panes and toolbar controls.
  Copy path and keyboard context menus use the highlighted item in the focused
  result pane, including after Left/Right and Tab changes. Empty or filtered-out
  selections have no action target. Drive-tree, filter and toolbar focus do not
  reuse a previous pane's file selection.
- **Largest** and **Types** retain their existing largest-file and file-type
  queries. The filter, size/name sorting, and Logical/On disk metric remain
  available. The toolbar wraps in a narrow window.

Right-click or Shift+F10 offers Reveal left/right, Preview, Copy path and Analyze
this folder. Preview uses the existing file-type handlers. Reveal and Preview
return to the file view to show their result; they do not mutate the file tabs
merely by selecting analyzer folders.

Capacity queries run in a background catalog worker. They refresh when selecting
a drive, refreshing a scan, or reopening the analyzer. They are capacity samples,
not a continuous per-write display. WSL disk entries appear only with WSL
integration enabled and a WSL2 registration present. Their free/total values are
**inside the Linux filesystem**, rather than host VHDX file length, host disk free
space or reclaimable VHDX space. The tree labels that distinction. Scanning a WSL
root still uses the native Linux scanner. Disabling WSL removes its catalog entries
and retires an active WSL analysis.

**Pause/Stop** keeps explicitly incomplete results. **Refresh** restarts the
captured root. Hiding or switching away retains results and pauses NTFS polling;
returning reuses the helper/model. A new drive/root retires the previous scan before
starting its replacement. Closing OpenPilot or disabling the analyzer retires the
owned helper. No scan or capacity worker starts during ordinary file browsing.

[Main-view implementation and validation](../build/space_main/20261024/RESULTS.md).

## Cached NTFS volumes

Selecting the root of a local fixed NTFS volume (for example `C:\`) starts the
on-demand `folderwing-ntfs.exe` metadata worker. Windows may request administrator consent
for that worker; OpenPilot and the Space panel stay unelevated. Declining consent,
a missing worker, unsupported raw metadata or unavailable USN capability falls
back to the standard scanner, with that source shown in the status line. Ordinary
folders, network roots, substituted folders and WSL roots keep their existing
scanner and do not start the NTFS worker.

The first NTFS scan builds a disk model. Later opens validate the saved model and
catch up from the volume's USN journal. The status distinguishes loading, cached,
catching up, rebuilding, live estimates, paused and incomplete results. A replaced
volume/journal or an expired checkpoint causes a new baseline. A failed refresh
keeps its older checkpoint for replay. Directory moves, renames, file deletion,
hard-link changes and default/named stream metadata are reconciled by full NTFS
record identity, including its reuse sequence.

First-time scans read the MFT sequentially in batches of up to 32 MiB, clipped
to each validated extent and the MFT valid-data length. If that buffer allocation
fails, the worker retries with 4 MiB. The extra 28 MiB is temporary and released
before model delivery. Record fixups, attribute parsing, full-reference resolution,
runtime indexes, journal catch-up and cancellation/deadline checks remain in the
scan path. Checkpoint and journal IO retain their existing 4 MiB chunks; cached
opening and periodic refresh do not allocate the larger scan buffer.
[First-time scan evidence](../build/ntfs_baseline/20261024/RESULTS.md).

After the initial result, changes update the existing display rather than resend
the whole tree. Selection and scope remain attached to retained entry IDs where
possible. **Pause** keeps the displayed estimate and releases the owned NTFS
worker; **Refresh** reopens its cache and catches up. Hiding the panel pauses
journal polling and extra sampling while retaining the worker/model. Showing it
catches up changes made while hidden, without another sign-in/elevation for that
retained worker. Choosing another root or closing OpenPilot ends the owned worker.

The journal covers every folder. Learned activity prioritizes refresh work and
adds bounded metadata sampling for files whose writes coalesce into an unchanged
USN. Heat decays, quiet hints are sampled less often, and exact cache/lock file
identities are excluded from learning. Sampling is bounded to 128 retained hints
per poll from at most 4,096 hints. These remain live estimates: writers without a
retained hint, hint eviction, contention and access failures can delay freshness.
No filesystem snapshot or per-write visibility guarantee is claimed.

Caches live in `%LOCALAPPDATA%\OpenPilot\SpaceCache\v2`, keyed by volume GUID
and serial. Cache IO uses the caller's normal token. Files carry version/geometry,
volume/journal identity, cursor and SHA-256; saves replace a completed sibling
atomically. Corrupt input and interrupted candidates are retained as `.rejected-*`
and `.pending-*` siblings for review. An exclusive cache lock prevents concurrent
writers; a busy cache falls back to the standard scanner. The whole model is
loaded and periodically rewritten in this first integration, rather than using a
delta log. Cache failure does not modify destination/user files or the USN journal.

Unchanged model/checkpoint save requests skip compaction, hashing and file IO.
A changed journal cursor still requires a save even when displayed sizes did not
change; this preserves the cursor/model checkpoint contract. Routine successful
polls retain the live status instead of cycling through a catching-up label.
Journal polling and bounded hot-file sampling continue normally while visible.

Small NTFS refreshes update the retained model incrementally. Size/stream/attribute
changes with unchanged names preserve name storage and valid links. Renames,
moves and hard-link changes replace only that record's names. Runtime indexes
track all aliases (including unresolved ones) and incoming parent references,
so a changed parent identity or traversal eligibility revalidates affected
children without scanning every name. Full MFT references include the reuse
sequence; reusing a parent or child slot cannot revive an old alias.

The indexes are private runtime state; existing v2 checkpoints remain readable.
Baselines and cache loads still perform full resolution and index construction.
Checkpoint saves reuse that preparation only while its validated name/reference
layout remains unchanged; otherwise they run full preparation again. Substantial
rename churn can also trigger compaction when
at least 65,536 retired names occupy a quarter of the name or text arena. Small
steady-state refreshes do neither. Append-arena or MFT growth may still resize
backing arrays. The indexes consume additional memory: four bytes per MFT slot,
16 bytes per name-capacity entry, plus a sparse parent hash. They trade memory for
local refresh work. Checkpoint cursor validation, checksums, completed sibling
replacement, and failed-candidate retention retain their existing behavior.
[Incremental refresh evidence](../build/ntfs_incremental_refresh/20261023/RESULTS.md).

Checkpoint loading hashes the exact v2 header and payload bytes in read-sized
chunks, verifies the complete SHA-256, then performs the existing structural
validation. Saved name-link fields are checked and discarded; the unchanged
full-reference resolver and runtime-index validator rebuild their derived state.
Packed checkpoints skip the redundant name/text compaction pass. Legacy text gaps
and tombstones retain the original compactor. Partial reads and cancellation
release unread storage before graph traversal; allocation failure keeps a valid
checkpoint available instead of classifying it as corrupt.

On the same warm 309 MB checkpoint, three alternating process pairs measured
loader medians of 377.515 → 345.486 ms. Complete loaded-model and runtime-index
fingerprints matched. A separate live C: panel comparison remained around one
second and did not establish a whole-panel speedup. Cache format, checksums,
reference validation, journal validation and checkpoint-write behavior are retained.
[Checkpoint loading evidence](../build/ntfs_checkpoint_load/20261023/RESULTS.md).

Checkpoint saves retain the prepared layout after a successful full preparation
or validated load. Size/stream/attribute-only updates with unchanged full
references, names and traversal eligibility, plus journal-cursor-only changes,
can reuse it. Namespace/reference/type/reparse changes, slot growth, index resets,
unresolved or bad-sequence references, pending extension data and retired name/text
storage force the original compaction, full resolver and index builder. One full
preparation establishes a new eligible layout after a namespace change.

Both save paths still hash the complete v2 payload, write every byte to a new
sibling, flush that file and atomically replace the checkpoint with write-through.
Failed publication preserves the previous generation and save debt for retry;
interrupted candidates remain for review. No delta log, weaker checksum or new
cache format is introduced. [Checkpoint save evidence](../build/ntfs_checkpoint_save/20261024/RESULTS.md).




The worker builds its display projection from pooled storage: stable link objects
come from 4,096-object slabs, and copied UTF-16 names use reusable size classes
in 64 KiB blocks. Renames acquire the replacement before returning the old name;
retirement releases names immediately, and completely empty name blocks are
freed. Link IDs and tombstone objects remain stable until a view reset, which
releases all projection blocks at once. Names remain independent of the cache's
movable text arena. Per-record temporary alias arrays are no longer allocated;
the current epoch identifies the aliases to update, preserving unique hard-link
allocation ownership.

On a synthetic one-million-file model, three alternating old/new samples measured
median projection construction at 204.180 → 99.328 ms, with 3,007,801 → 1,613
heap calls and about 20.4 MiB less incremental private memory. These measurements
exclude cache reading, parsing, actual IPC and UI work. Small models can use more
memory because slabs and size-class blocks allocate in chunks. No latency
threshold or cache/wire format changed.
[Allocation pooling evidence](../build/ntfs_projection_pool/20261023/RESULTS.md).

NTFS delivery uses one reusable overlapped-read event instead of probing the pipe
and sleeping for data. Cancellation and worker exit wake the pending read; any
cancelled operation is drained before its buffer/event can be reused or freed.
Hidden/shown state is checked during bounded waits so pause/resume controls still
work on a quiet connection. The wire format and authenticated worker ownership
are unchanged.

The panel applies at most 256 validated records per model-lock hold. Status,
reset and completion boundaries flush immediately, and completed records are
published before waiting for an incomplete next frame. A malformed frame or
worker error preserves the preceding valid records. The single-record client API
remains available through an adapter. New completion bypasses the partial-result
refresh throttle on the next normal UI tick; unchanged live completion messages
continue to avoid unnecessary row rebuilds.

Three alternating cached C: comparisons measured median delivery/ingestion at
942.599 → 636.203 ms, and request-to-finished-row painting at 1,848.340 → 1,463.076
ms. Both variants used the same saved checkpoint and the actual panel's model,
query/timer and painting code with an owned private bitmap. CPU was reported at
97–100%; these are indicative results under contention. Physical monitor
presentation was not measured.
[Delivery and ingestion evidence](../build/ntfs_delivery/20261023/RESULTS.md).



## Size semantics

**Logical** counts regular file lengths per directory entry, including repeated
hard-link names. **On disk** uses filesystem-reported file-data allocation. A
(volume/device, file identity) table attributes hard-linked allocation to the first
encountered name; later names display shared allocation. Subtree/type allocation
therefore reflects that attribution within this scan. Unknown allocation is
labelled explicitly and is not silently treated as a measured zero.

NTFS MFT results use physical clusters for resident/sparse/compressed data; a
small resident stream can therefore have zero separate data clusters even when
an ordinary Windows handle reports a rounded allocation.

These are live metadata measurements, not an atomic filesystem snapshot or an
exact reconciliation with volume free space. Windows measures the default file
stream; additional named streams are retained by the NTFS engine but are not added
to these displayed totals. The MFT path also includes visible NTFS metadata-file
default streams. Directory-index and other internal filesystem allocations are
not a complete reconciliation of volume used space. Filesystems with shared extents may report overlapping allocation
for distinct identities. WSL reports allocation inside the Linux filesystem,
not the Windows host allocation or reclaimable size of its VHDX.

Windows uses metadata handles relative to each held parent and does not follow
reparse points. Linux runs natively through `folderwing-wsl --analyze-space`, keeps directory
descriptors, and excludes symlinks, special files and mount boundaries, including
same-device bind mounts. The explicitly selected root may itself be a mounted
filesystem. Linux uses the captured configured WSL user; there is no automatic
elevation. Disabled/unavailable WSL integration produces a visible partial result
without falling back to provider traversal.

Scanning says **so far** while running. Unreadable entries/subtrees remain visible
with partial totals; excluded items and errors are counted. Stop retains the
metadata collected so far. The panel has no percentage-of-scan or invented ETA.

## Architecture and bounds

### Linux folder summaries and focused file listings

The main Space view uses `folderwing-wsl --analyze-space-summary` for Linux roots. It still
visits each eligible file to calculate sizes, but retains only directory names,
identities and aggregate logical/allocated bytes, file/folder counts, exclusions,
errors, unknown allocations and shared-allocation counts. Files do not consume
retained tree entries. A native bounded identity table attributes multiply linked
file allocation once across this scan; singly linked identities are discarded.
Linux directory-index and filesystem metadata allocation remain outside these
file-data totals. This is a live estimate, not an atomic snapshot.

During the initial walk, folders and partial totals are browsable. Once that walk
finishes, the two focused folders load their direct files asynchronously through
`--analyze-space-page`, without recursing into their child folders. File rows
never contribute to the retained folder totals. Late results from a replaced
root/session, or a different device/inode at the requested folder path, are
discarded. Cached listings are observations until pane Reload, whole-root Refresh
or root replacement.

Each pane offers **Prev**, **Next** and **Reload** beneath its listing, plus its
page number and file-entry range. Ctrl+PageUp/PageDown changes the focused pane's
file page; enabled controls also participate in Tab/Enter navigation. Each page
contains at most 4,096 direct file/special entries and 4 MiB of decoded names.
Directory summary rows remain independently available across pages. Sorting and
filtering operate on the displayed file page, alongside the retained immediate
folder rows; the footer labels this scope. Largest/Types still provide the
separate subtree-wide analysis. A filtered-empty page can still have a Next page.

The listing LRU retains at most eight folders, 65,536 file/special-entry rows and
8 MiB of filenames, with both displayed folders pinned. It keeps one active file
page per folder; replacing a page frees its old rows/names and reuses their slots.
Both panes share that page if they show the same folder. One staging worker has
its own 4,096-row / 4 MiB name bound. Previous pages are re-enumerated rather than
kept in an unbounded history; reaching a distant page therefore visits earlier
directory entries again. Filesystem-provided entry types avoid opening skipped
files when possible. Page requests do not inherit the legacy 500,000-entry cap.

Page partitions follow directory enumeration order, with both row and name-byte
limits. Requests bind to directory device/inode and nanosecond mtime/ctime, checked
before and after enumeration. Membership changes invalidate a page; previously
accepted rows remain visible with a **Folder changed · Reload files** message.
Reload starts a fresh first page; whole-root Refresh also recomputes summaries.
Payload modifications can leave directory dates unchanged, so this is not an
atomic snapshot of all file contents or sizes. Cancellation or an invalid page
never replaces an accepted page. File allocations are fresh, listing-local
observations; multiply linked files are marked shared. They need not add up to
the earlier scan's global hard-link attribution.

[Pagination implementation and validation](../build/space_pagination/20261024/RESULTS.md).

Largest Files and File Types run a separate full-file analysis only when requested.
That analysis temporarily retains file metadata under its own eight-million-entry
and 512 MiB filename bounds; it can use substantially more memory than folder
browsing. Its immutable snapshot is reused for sorting, filtering, metric changes
and type drilldown while the analysis view remains open. Leaving for Folders,
root replacement and shutdown release it, with outstanding workers holding leases
until cancellation drains. Analysis limits report unavailable results rather than
silently presenting a truncated complete analysis.

Nonfatal detail traversal failures retain useful Largest Files and File Types
rows, with an explicit **File analysis incomplete** warning and the detail
scan's own error/exclusion counts in both analyzer layouts. A successful stream
with no fatal error does not imply a complete enumeration. Sorting, filtering,
metric changes and type drilldown reuse that qualification with the immutable
snapshot. Folder-summary totals remain independent. Returning to Folders drops
the detail snapshot; a fresh analysis can clear the warning once access is restored.

The directory summary tree itself allows one million folders and 128 MiB of names,
with the existing 128-level traversal bound. Entry/name/memory cutoffs stay visibly
incomplete. Configured-user permissions, mount exclusion and disabled-WSL behavior
remain enforced; there is no automatic elevation. The NTFS MFT/cache path retains
its existing model and node layout.

- `folderwing-space.exe` owns presentation, a background scanner, metadata model and views.
  The main app only launches it and authorizes result actions.
- `app/space.c` aggregates ancestor totals, hard-link identities and view queries.
- `app/spacescan.c` provides Windows traversal and the native WSL stream bridge.
- `app/ntfsclient.c` owns an authenticated, local, nonce-named pipe and disposable
  job for `folderwing-ntfs.exe`. Both peers verify the exact process; the worker also checks
  the owner creation time, SID and session. Raw-volume access stays in that worker.
- `app/ntfscache.c` and `app/ntfsmft.c` own MFT parsing, checked persistence, USN
  catch-up and adaptive sampling. Only metadata records cross into the panel.
- `linux/fpspace.c` is an on-demand mode in the existing static Linux helper.
- `spacewire.h` is a bounded private metadata stream. Paths are binary data, never
  interpolated into shell commands. Decoder/file payload content is not scanned.
- Host-issued root/session IDs and exact descendant checks bind result actions.
  Late actions from a replaced root are refused. Normal Settings RUN/SET authority
  is not granted to this helper.
- The legacy standard scan retains at most 500,000 entries and 64 MiB of names; the NTFS
  presentation allows 4,000,000 entries and 512 MiB of retained names. Both allow
  128 levels of descendants. The NTFS reader also bounds attribute-list metadata
  to 1 MiB and 256 extension references per refreshed file; unsupported metadata
  leaves an explicit incomplete estimate. Reaching a limit produces incomplete results. Expansion and view
  changes query collected metadata and do not rescan the filesystem.

Treemaps, named scan history and cross-scan comparisons remain future work. The
NTFS cache is operational state, not a saved historical snapshot.

## Query and idle-work costs

Flat views follow captured child/sibling links through only the chosen subtree,
including files moved into a folder whose ID is newer than theirs. Largest Files
sorts compact size/name/ID snapshots before building its display rows. File Types
aggregates normalized extensions in a hash table and sorts only the resulting
groups. Full results, filename filters, extension drilldown, logical/allocated
metrics and deterministic file ordering are retained; results are not truncated
to a top-N list.

The panel distinguishes metadata invalidation from visible status changes.
Unchanged live ticks do not rebuild rows, paint frames or resend identical host
state. Hiding/showing an unchanged model reuses its rows; changes received while
hidden invalidate them for the next show.

Largest Files, File Types and extension drilldown now query on one disposable
background worker. It copies at most 256 captured nodes per model-lock hold,
then filters/groups/sorts outside that lock. Immutable name-slab leases survive
scan replacement without copying every filename. Version changes during capture
or before publication discard the result and schedule a fresh query. New filter,
sort, metric, scope and view requests cancel obsolete work and coalesce to the
latest request. Sorting and row materialization have bounded cancellation checks.

The previous rows remain visible while results update. Their published view
kind is retained until replacement, so pending group queries do not reinterpret
old file rows. The legacy panel retains their published metric as well. In the
main browser, File Types renders each retained group's stored Logical or On disk
aggregate for the currently selected metric immediately, using that same metric
for percentage denominators. A pending query is labelled with “Previous order
retained”; the replacement publishes the new sorting order. Publication preserves selected identity
and viewport position where possible. Hiding cancels in-flight work and defers a
replacement until shown; reset/root replacement invalidate old work. Closing joins
query ownership before freeing the model. Folders remains a synchronous tree query.
A continuously changing model can delay publication until a consistent snapshot
finishes; this is a live estimate rather than a filesystem snapshot.

A paired, alternating three-sample benchmark on a synthetic 500,000-file model
previously measured median Largest Files queries at 491.287 → 191.773 ms and File Types at
254.751 → 27.799 ms (200 extensions). A 100-file subtree query within that model
fell from 5.445 → 0.015 ms. Before/after result fingerprints matched. These are
model query timings, not disk scans or end-to-end click-to-frame measurements.
[Optimization evidence](../build/space_query_optimization/20261023/RESULTS.md).

## Focused validation

`tests/space_analyzer.ps1` builds no product code itself. Given a candidate build,
it runs native model/scanner/panel checks plus actual host/helper acceptance using
owned windows and fixtures. It checks Windows sparse files and hard links, native
Linux sparse/hard-link/literal-name handling, excluded links/FIFOs, unreadable
subtrees, stopping, sorting/filtering/type drilldown, shared scope, hidden results,
real reveal/preview routing, stale actions and feature deactivation. No physical
keyboard/mouse input is injected and WSL/Docker are not stopped.

[Implementation evidence](../build/space_analyzer/20261022/RESULTS.md).

`tests/ntfs_analyzer.ps1` additionally runs the cache/journal/delta-model contracts
and the actual unelevated panel with its elevated worker, a whole C: model and an
isolated owned cache. It checks live changes, exact action paths, hard-link
allocation, hidden pause/resume, cache reopening and owned-process teardown. Its
native window messages do not inject physical keyboard/mouse input. The existing
Space acceptance remains responsible for standard Windows and native WSL scans.

The asynchronous implementation was exercised on a separate 500,000-file model
with real owned panel rendering and input handlers (no physical input). View
requests returned in 1.198 ms (Largest Files) and 3.754 ms (File Types); the maximum
sampled input handler was 3.862 ms across 27 input events. Complete results arrived
in 350.074 ms and 63.495 ms while input remained available. This is one live-host
probe, not a latency guarantee or a direct comparison with the earlier pure-query
benchmark. [Async query evidence](../build/space_async_queries/20261023/RESULTS.md).
