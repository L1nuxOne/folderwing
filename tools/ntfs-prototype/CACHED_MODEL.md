# Cached disk-model experiment

Standalone Windows NTFS companion to the MFT scanner. It is not linked into
OpenPilot, does not replace the release, and does not install a service or a
scheduled task. The volume is opened for reading only. The runner creates and
changes only its own fixture/cache/evidence files; it does not stop WSL or Docker,
change security settings, lock/dismount a volume, flush a volume, or reset/trim
its journal.

## Accepted C: cohort — 2026-10-23

Initial model/totals ready: **8.014 s**. After controlled changes:
**0.834 s**; quiet cached reopen: **0.781 s**,
with zero full-MFT bytes in both cached paths. Quiet full command, including the
prefix CSV and cache save: 1.916 s. The cache is
294.9 MiB, peak cached RAM 405.2 MiB.
These are live-host samples, not stable percentile measurements.

32 parser + 40 cache diagnostics and 1,181 owned-fixture assertions pass.
An open writer reached 139,264 bytes from 40,960 without its USN advancing; extra
sampling observed the final size before close. Recovery checks cover interrupted
saves, corrupt/truncated input, refresh replay, invalid identities/checkpoints and
repeated gaps. Actual USN journal state was never altered.

[Exact measurements, frozen source/image and recovery evidence](../../build/ntfs_cached_model/20261023/validation-5/RESULTS.md).

## Model and checkpoint

`cached.c` reuses the existing raw scanner bootstrap and parser. The cache stores
plain record/name/text arrays, the geometry, volume serial, journal ID, next USN,
generation and bounded activity hints. It contains local filenames; keep it
local. The format is experimental, specific to this executable/ABI, and intended
for trusted local experiment files rather than imported caches.

Opening checks the version, file length, counts, SHA-256 over the header and all
arrays, name slices and model references before rebuilding in-memory name links.
A missing cache triggers a baseline. A corrupt/truncated cache is moved to an
owned `.rejected-PID` sibling before rebuilding, retaining it for review. A
changed serial/journal ID, cursor behind FirstUsn/LowestValidUsn, cursor beyond
NextUsn, or a journal-deleted read triggers a new baseline. No journal is created
if USN is unavailable; the experiment reports that missing capability.

A baseline captures its journal cursor **before** reading the MFT. Catch-up then
captures a finite NextUsn fence, reads every journal record up to that fence,
validates lengths/version/name bounds/cursor progress, and coalesces identities
by record slot. It queries the current record rather than replaying old filenames
or sizes. Full sequence references protect against deleted/reused slots;
FSCTL_GET_NTFS_FILE_RECORD's lower-record fallback is treated as deletion, not as
the requested record. A slot now occupied by an extension is no longer a base
file. Names from every refreshed base are replaced together, including aliases
that were previously unresolved. Directory totals are recomputed from the graph
in memory, including renamed/moved subtrees.

Changed files use FSCTL_GET_NTFS_FILE_RECORD for kernel metadata. That decoded
record is not raw disk input: retained USA values can be stale after kernel
updates, so raw-sector fixups must not be applied to it. Attribute-list metadata
is chased with bounded record/count/sequence checks; nonresident lists use
sector-aligned reads (the logical list length need not be sector aligned).
Extension data contributions are resolved once. An attributes-only OpenFileById
handle provides current default EOF for open writers; physical allocations retain
the MFT parser's resident/sparse/compressed semantics and ADS totals remain
separate. Resident data is not assigned rounded handle allocation.

The journal checkpoint advances only after the entire finite queue succeeds.
A failed record refresh produces `partial: true`, exit 3 and a retained old
checkpoint so a subsequent run can replay it. Failed sampling hints are retained
as dirty hints for retry. Cache saves hash the complete model, write a new
`.pending-PID` sibling, flush that cache file, close it and atomically replace the
previous cache. Failed/interrupted candidates are retained. This is cache
recovery, not a new OS-crash durability claim: a missing/invalid cache rebuilds.

## Adaptive refresh

All folders remain eligible through the volume-wide journal. Activity determines
ordering and extra sampling; it never filters journal coverage.

- Up to 4,096 file hints retain their full identity, parent, decaying heat,
  event count and last sample time. Heat has a 60-second half-life.
- Catch-up orders coalesced files by their own heat plus their parent's activity.
  Every identity at the finite fence is still attempted, including a new file in
  a previously quiet folder. The queue is bounded at two million events.
- Watch polling uses 250 ms after an active batch, otherwise 1 second. Due hints
  request 1/2/5-second sampling for hot/warm/cool entries, 30 seconds after two
  quiet minutes and 5 minutes after a quiet day. Observed size/allocation changes
  renew heat even without another USN. At most 128 hints are sampled per poll;
  oldest-due-first ordering prevents a single busy folder from monopolizing the
  retained hints. Failed samples retry and keep the result partial.
- Watch mode is explicit, runs for at most 300 seconds, and exits. There is no
  always-running watcher in this prototype.

These are **live estimates**, not a filesystem snapshot or a guarantee that each
write is visible within one second. USN coalesces repeated reasons while a file
stays open. Sampling mitigates this for retained hints; eviction, access failures
and scheduler load can delay visibility. It does not discover an old open writer
that has no retained hint and no new journal record. Cold coverage still relies
on USN, and an explicit fresh baseline remains useful. A raw baseline can reject
inconsistent live metadata rather than save a model it cannot validate.

## Reproduction

Build from WSL:

```sh
python3 tools/ntfs-prototype/build-cached.py
```

Run the standalone checks without elevation:

```powershell
& .\build\ntfs_cached_model\20261023\candidate\cachedprobe.exe --selftest
```

For raw-volume runs, use an elevated Windows PowerShell and fresh, owned output
paths (the shell can launch this script using the existing RunAs facility):

```powershell
& .\tools\ntfs-prototype\validate-cached.ps1 `
  -Probe C:\Work\openpilot\build\ntfs_cached_model\20261023\candidate\cachedprobe.exe `
  -ResultDirectory C:\Work\openpilot\build\ntfs_cached_model\20261023\validation-NEW
```

For a manual bounded run:

```powershell
& .\build\ntfs_cached_model\20261023\candidate\cachedprobe.exe C: `
  --cache=C:\owned\model.bin --watch=30 --json=C:\owned\result.json
```

Optional `--csv` and `--prefix` export selected paths from the in-memory graph.
CSV directory sizes are descendant logical entries; physical bytes are attributed
once per file identity. Physical attribution to an individual hard-link parent
is arbitrary, as in the original prototype. ADS totals are separate columns.
The diagnostic `--inject=gap|journal|volume|refresh|unstable` changes only process-local
metadata. `--interrupt-save` writes an incomplete owned candidate and returns 4;
it does not kill Windows or change the actual USN journal.

## Scope before product integration

This first version loads/checks and rewrites the whole compact cache. Aggregation
and name relinking also traverse the model in memory; they are not disk rescans.
A later integration can use a mapped cache/delta log and ancestor updates, plus a
background worker with cancellation and a UI that distinguishes cached, catching
up, live-estimated and incomplete results. The prototype has no analyzer UI,
cache migration policy, encryption, persistent daemon, view-specific priority,
concurrency arbitration between cache writers, or Linux/WSL cache backend. Its
activity hints currently include its own cache/report files; exclude those exact
identities from heat learning when integrating, while still updating their model
records. Do not generalize these NTFS results to Linux or network filesystems.

## API contracts

Microsoft references used while implementing:

- [Reading the USN journal](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-read_usn_journal_data_v0)
- [Journal identity and valid cursor range](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-usn_journal_data_v0)
- [USN record layout and change coalescing](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-usn_record_v2)
- [File-record lookup and its lower-index fallback](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_get_ntfs_file_record)

Measured results, exact source/image hashes and checks are in the linked cohort
receipt, separate from the product's standing test gates. No product test selector,
manifest or source-impact mapping is added or changed by this experiment.
