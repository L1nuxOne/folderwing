# NTFS MFT prototype

A standalone read-only whole-volume experiment, separate from the installed
OpenPilot analyzer. It uses Windows system APIs and a small native C parser;
there is no added library, service or product dependency. The existing analyzer,
release directory and WSL lifecycle are unchanged.

The [cached model / USN / adaptive refresh companion](CACHED_MODEL.md) extends
this experiment without changing the measured full-scan executable.

## Measured on C: — 2026-10-23

C: has a 2,332,819,456-byte MFT, 1,024-byte records and 10 mapped extents.
The experiment reads 2,278,144 record slots and reconstructs about 2.24 million
file/folder names, including extension records and hard links. The live drive
continues changing between independent scans.

| Measurement | Prototype | WizTree 4.33 |
| --- | ---: | ---: |
| Scan/model/totals ready | 7.21–8.00 s | Not directly exposed by the command-line export |
| Full scan and CSV export | 12.20–12.96 s | 27.14–27.25 s |
| Peak working set, final comparison | 350.1 MiB | At least 1,016.8 MiB, sampled |

These are uncontrolled development-host measurements, with serialized runs in
both orders across the exploratory/final comparison. WizTree sorts its export,
adds timestamps and builds more presentation data; the prototype emits MFT order
and does less work. Full CSV wall time is not a pure scan-speed claim. Process
I/O traces put WizTree's full MFT read at roughly 11 seconds in the final run,
before CSV output begins. A small file-type export still took 26.15 seconds;
it also performs classification/grouping and is not used as an exact scan timer.
No cache purge, background-process shutdown, volume lock, dismount, volume flush,
WSL stop or security-setting change was used.

Final comparison: 1,724,527 shared regular-file record identities. Logical sizes
match for 1,724,501 (99.9985%), with 26 differences on this live volume. Hard-link
name counts match for every shared identity. Default-stream allocation matches
for 1,724,127; 388 of the 400 allocation differences match the prototype's total
allocation across named and unnamed streams. Twelve allocation differences
remain across the separate live scans. These are retained as differences, not
an assertion of snapshot equivalence. The benchmark's own large CSV files and
active application data contribute to between-scan changes.

32 parser checks and 48 owned-fixture checks pass. Fixtures cover literal Unicode
names, resident data, hard links, alternate data streams, a 1 GiB sparse file and
a compressed file. They compare file IDs/sequences, current handle lengths,
allocation and independent WizTree results. The resident allocation oracle is
explicit: a three-byte resident value has zero separate data clusters, while
Windows FileStandardInfo reports 8 bytes and FileCompressionInfo reports 3.
The initial naive allocation assertion is retained in the evidence as a failed
check; the corrected checker verifies the different quantities separately.

[Final measurements and CSV comparison](../../build/ntfs_mft_prototype/20261023/comparison-2/)
include input/source/image hashes, phase timings, process I/O/memory traces,
parser output, frozen handle metadata and fixture verification. The exact
benchmark script used for the recorded run is retained as `benchmark-used.ps1`;
the reusable runner now also creates the resident-allocation witness itself.
CSV exports contain local filenames and remain local.

## Architecture and semantics

- Open only the chosen NTFS volume with GENERIC_READ. Query its NTFS geometry.
- Read/fix up MFT record zero, decode and validate the MFT's own extent map.
- Read those extents in 4 MiB blocks. Apply update-sequence fixups and validate
  bounded attributes; resolve base/extension references and parent sequences.
- Retain literal non-DOS names, suppress duplicate aliases, and reconstruct
  parents and paths. Never follow directory junctions or open files for their
  contents. Resident payload bytes are present in raw MFT records, but their
  contents are not decoded or exported.
- Derive default-stream logical length and allocation from DATA attributes,
  retaining named-stream totals separately. Count physical runs for complete
  sparse named streams such as `$BadClus:$Bad`, including the special case where
  the usual sparse flag is absent.
- Aggregate logical lengths per hard-link name; attribute default-stream
  allocation once per file record. Which subtree receives shared allocation
  depends on traversal/name order, as in the existing analyzer.

`Allocated` in the prototype CSV is the default stream only. `All Stream
Allocated` also includes alternate streams; WizTree generally includes those in
its allocated figure. Resident payload is accounted inside `$MFT`, not allocated
a second time as separate clusters. Five NTFS metadata files on this volume
have only named streams; a missing unnamed stream is a known empty default
stream, not an unreadable record. Named sparse streams can have enormous logical
lengths (the bad-cluster placeholder spans the volume); that virtual length is
not included in the default-stream folder totals.

## Bounds and integration limits

This is a feasibility prototype, not a replacement backend yet:

- Only NTFS 3.0/3.1 is accepted. MFT bootstrap attribute lists are explicitly
  rejected; C:'s bootstrap extent map fits in the base record. Ordinary file
  attribute lists/extension records are handled during the complete scan.
- At most 16 million MFT record slots and 512 parent levels are accepted.
  Allocation/path errors, stale references and malformed records are reported.
- Raw reads of a live mounted filesystem are not an atomic snapshot. Before
  integration, define the live-consistency/revalidation policy and handle MFT
  growth/relocation during a scan. No Volume Shadow Copy was created here.
- The current panel's 500,000-entry model is not reused. Integration needs this
  compact identity/name representation (or another deliberate memory design),
  rather than streaming millions of records into the old bounded model.
- Decide and display allocation semantics consistently across MFT and fallback
  scans: default versus alternate streams, resident data, directory/system
  metadata and hard-link attribution. Broad free-space reconciliation is not
  claimed by this experiment.
- Keep any elevation inside a read-only helper. Launch/result authorization,
  cancellation and panel integration remain future product work.

## Reproduce

From the WSL repository, using the existing installed Zig compiler:

```bash
python3 tools/ntfs-prototype/build.py --output build/ntfs_mft_prototype/next/candidate
build/ntfs_mft_prototype/next/candidate/mftprobe.exe --selftest
```

Then run the comparison in an elevated Windows PowerShell session, with a fresh
output directory on the measured volume:

```powershell
C:\Work\openpilot\tools\ntfs-prototype\benchmark.ps1 `
  -Probe C:\Work\openpilot\build\ntfs_mft_prototype\next\candidate\mftprobe.exe `
  -ResultDirectory C:\Work\openpilot\build\ntfs_mft_prototype\next\comparison
```

After the serialized measurements finish:

```bash
python3 tools/ntfs-prototype/compare.py PATH/own.csv PATH/wiztree.csv --output PATH/COMPARISON.json
python3 tools/ntfs-prototype/verify_fixtures.py PATH
```

The executable also supports `mftprobe C: --json=owned-output.json` without CSV
and `mftprobe --stat owned-file` for current default-stream handle metadata.
These built-in standalone diagnostic checks do not replace or claim acceptance
of the application's standing native suite. Product/test selectors are unchanged.

Primary references: [NTFS MFT](https://learn.microsoft.com/en-us/windows/win32/devnotes/master-file-table),
[record headers](https://learn.microsoft.com/en-us/windows/win32/devnotes/file-record-segment-header),
[attributes and mapping pairs](https://learn.microsoft.com/en-us/windows/win32/devnotes/attribute-record-header),
[WizTree export options](https://diskanalyzer.com/guide).
