# Archive preview engine review — 2026-10-22

Status: miniz + libarchive selected and implemented on 2026-10-22.
The comparison below remains the original engine experiment; feature behavior,
limits and acceptance evidence are documented in [Archive previews](ARCHIVE_PREVIEWS.md).

The intended UX is selection-driven: selecting an archive shows its contents in
the second normal pane; selecting a supported entry previews it in a third pane
using the existing content handlers. The archive listing has its own context menu
and starts read-only. The displaced pane state is restored on leaving the archive.

## Focused Windows comparison

All three are native Windows release benchmark executables. Sizes include a common
benchmark wrapper, reading and writing; these are proxies for helper size.
The general libarchive build includes zlib, bzip2, LZMA and Windows crypto;
minizip-ng includes DEFLATE and Windows-based ZIP encryption support. Both use
zlib 1.3.2. Miniz uses its own DEFLATE implementation.

10,000 small regular files, 12.2 MiB total. Values are observed warm-cache medians
of three measured repetitions; each operation uses a fresh process. Read timings
use identical reference ZIP bytes. Encoding uses requested DEFLATE level 6, whose
effort is implementation-specific. Decoding consumes bytes without writing output
files. Library timings exclude process startup and UI/rendering.

| Engine | Executable | List entries | Read last 2 KiB entry | Encode ZIP | Decode all |
|---|---:|---:|---:|---:|---:|
| miniz 3.1.2 | 152 KiB | 2.36 ms | 1.10 ms | 162.49 ms | 85.01 ms |
| minizip-ng 4.2.2 | 219 KiB | 5.66 ms | 6.40 ms | 292.49 ms | 213.36 ms |
| libarchive 3.8.10 | 815 KiB | 20.32 ms | 13.79 ms | 163.58 ms | 45.13 ms |

Results depend on workload: decoding a single 64 MiB text entry took 135.96 ms
with miniz, 32.10 ms with minizip-ng and 29.87 ms with libarchive. Resulting ZIP
sizes, compression levels 1/6, mixed/incompressible data, memory, process launch
costs, ranges and CPU observations are retained in the full report. Host CPU varied;
these are observations, not an idle-machine or tail-latency qualification.

## Selected choice

Keep the two-helper design: miniz for ordinary ZIP preview/navigation, libarchive
on demand for other supported formats and unsupported ZIP features. The general
fallback must explicitly report features it cannot handle. Minizip-ng remains an
alternative ZIP backend if ZIP encryption and faster large-entry decoding take
priority over minimum code size and the measured small-entry overhead. The tests
do not establish a universal fastest engine or qualify all archive features.

The integrated helper retains metadata/entry identities between selections;
these original fresh-open measurements do not represent warm selection in that UI.

The bounded run completed 240 operations: 60 warmups and 180 measured samples.
Independent Python ZIP checks passed for all 96 generated archives; 84 native
cross-reader and selected-entry checks verified exact names, lengths and SHA-256
contents. No app source, installed release, WSL or Docker state was changed.

[Full report](../build/archive_engine_review/20261022/RESULTS.md),
[CSV](../build/archive_engine_review/20261022/summary.csv),
[raw samples](../build/archive_engine_review/20261022/SAMPLES.json),
[verification](../build/archive_engine_review/20261022/VERIFICATION.json),
[receipt and input hashes](../build/archive_engine_review/20261022/RECEIPT.json).
