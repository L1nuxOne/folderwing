# Interrupted transfers and staging review

OpenPilot keeps files left by an interrupted transfer for review. It does not
automatically delete, publish, roll back or resume them on startup. Operations
results belong to the running explorer session; they are not a durable transfer
journal. Closing/crashing the Operations panel is different: the explorer still
owns its jobs and results while that panel restarts.

The current writers use these names next to their destination:

| Writer | Candidate name | Possible contents |
| --- | --- | --- |
| Linux native copy/import | `.openpilot-stage-<32 hex>` | `payload`, or an empty workspace after publication |
| Windows native copy | `.openpilot-copy-<32 hex>.tmp` | Unpublished file, including any copied streams |
| Linux-to-Windows stream | `.openpilot-host-<pid>-<serial>.tmp` | Unpublished streamed file |

A name, owner, timestamp, PID or exclusive-open failure cannot establish both
OpenPilot ownership and abandonment. Existing files have no durable ownership
record. Another operation can still be using a matching name; unrelated files
can have the same name. Even a full-size payload may not have passed hashing,
metadata, namespace guards or persistence barriers. Never infer a destination
name from one of these temporary names.

## Read-only review

From WSL/Linux, inspect an explicitly selected containing folder:

```sh
python3 /mnt/c/Work/openpilot/tools/staging-review.py /mnt/c/path/to/destination
python3 /mnt/c/Work/openpilot/tools/staging-review.py /home/example/destination --depth 3
```

The JSON report lists exact-format candidates with observed type, size, mode,
owner, timestamp and device/inode identity. Ownership is **unverified** and
activity is **unknown**. Ordinary completed filenames are excluded. No candidate
or payload is opened for content, deleted, renamed or changed. Symlinks are
reported without following them; child traversal uses pinned directory file
descriptors and rejects symlinks/substituted identities. Staging candidates are
never descended into. An explicitly supplied root may have aliased ancestors;
the final root component must be a real directory, not a symlink.

The default scope is one directory, with limits of 10,000 visited entries and
1,000 candidates. `--depth` permits up to 16 levels; `--max-entries` and
`--max-candidates` allow up to 1,000,000 and 10,000 respectively. Reaching a limit
or encountering an I/O error produces a partial report and exit status 1.
Directory depth defines the requested scope, not proof that deeper folders have
no candidates. Filesystem calls can still stall: these are work limits, not a
wall-clock timeout. Windows folders are inspected through their WSL mount; the
tool does not run under native Windows Python. Names are JSON-escaped, including
raw Linux filename bytes and control characters.

Reports describe a live observation, not a stable recovery journal or permission
to delete. Directory identity checks do not sandbox a hostile same-UID/root
process. The tool creates no report file itself; choose a separate output path
if redirecting stdout so it cannot overwrite something being reviewed.

## Recovery decisions

First establish that the affected operation has stopped. Compare the source and
visible destination; preserve both when their state is uncertain. If staged
bytes are worth recovering, copy them to a separately chosen new file for
inspection. Leave the original staging object intact until the recovered file
has been checked. A partial payload cannot substitute for a verified transfer.
Ambiguous/foreign objects stay untouched. No recovery action automatically
removes a move source or replays an old acknowledgement.

The following publication/source-removal sequence applies to staged copies and
copy-based moves. Same-volume native moves can atomically rename the source
itself: its old name disappears at that rename, before the namespace barriers.
They do not leave a staged copy or promise rollback after a barrier failure.

| Interruption point | Expected process-crash state |
| --- | --- |
| During staging/before publication | Old overwrite destination and source remain; staging may be partial or complete |
| After publication, before a successful final barrier/ACK | New destination remains; source remains; Linux workspace may be empty |
| Successful importer ACK lost before the exporter receives it | Destination and source both remain |
| After verified publication/ACK, before source unlink | Destination and source both remain |
| After source unlink, before its parent-directory flush | Destination remains; source can already be absent |
| During a recursive move | Completed children may have moved; remaining sources and staging stay available |
| During a Windows small-file batch | A published prefix and unpublished staging siblings can coexist; copy sources remain |

There is no all-or-nothing tree rollback. Replacement does not preserve an old
destination after a successful rename. Recovery must not delete the newly
published destination just because a following barrier or acknowledgement was
interrupted.

## Evidence and remaining scope

`tests/test_transfer_crash.py` links actual Linux writers into a private fixture
and uses SIGKILL at partial writes, file/directory barriers and source unlink.
It exercises regular files, literal symlinks, incremental trees, lost ACKs and
an importer restart. `tests/transfer_crash.ps1` builds private Windows writer
fixtures and terminates them with `TerminateProcess`, testing native cross-volume
moves, streamed publication and 32-file batches (0/16/32 published files). Its
stream fixture tests the writer directly; Linux lost-ACK tests cover exporter
source retention, and `tests/windows_transfer_tests.c` owns the real Windows/WSL
ACK/source-removal integration. The Windows script also checks the read-only
report against actual NTFS staging files through WSL.

These are process-crash and observable-state tests. They **do not certify**
host/VM interruption, controller caches, device power loss or filesystem journal
replay. Controlled isolated-VM interruption tests were subsequently completed for the
Linux/Windows and relay/metadata cohorts listed in [VM recovery evidence](VM_RECOVERY.md).
The original harness prepares eight Linux move checkpoints and guards interruption
to a newly owned Hyper-V guest. Physical host/controller/device power loss remains
outside that evidence. Local process/mock checks
do not count as executed VM recovery cases. Automatic
resume or verified cleanup would first require a durable ownership/transfer
journal; neither is implemented by this review tool. Linux destinations
preflight existing parents through their mount root. Windows transfers also
preflight existing parents, using a disposable elevated directory-flush broker
when ordinary access is denied. An unavailable or denied required fence stops
before transfer mutation; existing destinations and sources remain. POSIX
final-check/unlink limits remain as documented in the persistence contracts.
Process-crash observations do not certify mount or
storage recovery after a host/VM interruption.

## Legacy 9P transfer policy

The Windows/9P route cannot supply the required Linux directory flush barriers.
Copy/move jobs into Linux, selected copies into Linux, and moves removing Linux
sources are refused with `ERROR_NOT_SUPPORTED` before mutation when native WSL
integration is disabled. Enable native WSL transfers for those jobs. A read-only
Linux source can still be copied to Windows through 9P. This preflight policy does
not replace any file, directory, ancestry, source-removal or ACK barrier in a
supported transfer, and does not turn an error after publication into rollback.
`tests/wsl_compatibility.ps1` and the native ancestry selector verify this policy.
