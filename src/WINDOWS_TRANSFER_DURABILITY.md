# Windows transfer persistence

For Linux-to-Windows regular-file streams, the Windows writer keeps the
exclusive writable staging handle open while applying final timestamps and
attributes, flushing, publishing by handle, and flushing the same file handle
again. Normal copy/move streams also hold and flush the actual destination
directory before the final file ACK. The ACK follows this sequence and cleanup.
A successful ACK permits the Linux helper's guarded source removal, including
its checked source-parent flush for moves.

The first `FlushFileBuffers` runs after all streamed data and final metadata.
Failure prevents publication, cleans up the owned temporary object and retains
the Linux source. The second flush runs after a successful handle-based rename.
Publication is marked immediately after rename: if this flush fails, cleanup
retains the complete visible Windows destination, sends an error ACK and keeps
the Linux source. An overwritten old destination is not restored. Errors are
propagated; unsupported Windows operations map to Linux `ENOTSUP`, disk-full to
`ENOSPC`, access denial to `EACCES`, and other flush failures to `EIO`.

Cancellation is checked after each potentially blocking flush. Cancellation
before publication leaves an existing destination intact. Cancellation after
publication keeps the new destination and the source; it does not roll back the
rename. A flush error takes precedence over cancellation observed with it.
Existing write/metadata/source errors still prevent a successful commit ACK.

For recursive Linux-to-Windows copy/move jobs, the helper waits for a Windows
directory ACK before descending into each created or merged directory. Windows
opens writable directory handles and flushes both that directory and its parent
within the submitted destination. A root-only request flushes the endpoint
itself. A separate ancestry preflight fences the existing parents once before
the transfer starts; per-file and descendant requests stay within the endpoint.
Symlink moves also require a checked parent-directory ACK after publication.

At directory completion, the Linux helper closes its DrvFS directory handle
first. For a newly copied directory, Windows then applies and verifies the
original timestamps at its 100ns resolution before flushing directory and parent
and acknowledging completion. This avoids observed subsecond truncation in
DrvFS's directory `futimens` path. Existing merge directories keep their existing
metadata policy. Source-directory removal follows a successful final ACK.

Directory flush/metadata failure or cancellation retains the remaining source
tree and reports the result. Previously acknowledged children may already have
moved and remain at the destination; directory moves are incremental. New empty
directories may remain after an initial barrier failure. Unsupported flushes are
errors, never silently treated as a durable commit. Explicit root aliases are
allowed; a reparse point at a requested descendant directory is rejected.

The coupled helper/coordinator protocol is version 4. Directory requests and
ACKs use message IDs 54 and 55; pinned-guard sync/ACK use 56 and 57. Pinned Linux
destination ancestry uses request 58 and a checked ACK 57. Existing
search/index IDs are unchanged. The
HELLO version check rejects old/new mixed pairs before job mutation. Deploy the
Linux helper and Windows runtime images together and restart the application
to use the new coordinator.

Selected-file/tree copies flush the destination endpoint before copying, each
destination child and parent before descent, and the leaf's directory followed
by every captured ancestor within the endpoint after each file. Only then can
the job report a completion without errors. A barrier error stops remaining
selections and is reported through the existing job diagnostics. Published
files and all sources remain; an interrupted copy is not rolled back.

Windows guards retain a name pin denying deletion/rename and a separate read
barrier denying directory writers. During each synchronous flush only, all
matching read barriers owned by this walk are exchanged together. An empty-name
NtCreateFile relative to the name pin opens the exact held directory without
following reparse points. No pathname traversal or user callback occurs during
this exchange. The writable handle is closed, all read barriers are restored,
and directory identity/type are checked before continuing or checking
cancellation. An in-place reparse conversion or failed restoration aborts the
copy. External read barriers remain effective; there is no sharing fallback.
Ordinary traversal, comparison and search retain their read barriers.

Linux guard helpers fsync each child and its parent before returning a created
guard, and ACK final sync commands only after fsync of the pinned descriptor.
Only EINTR is retried. The coordinator requires checked ACKs and reports errors,
malformed replies, disconnects, timeouts and cancellation. Descriptor anchors
are retained through these barriers. A Linux destination-root guard also fences
existing parents through its mount root once before the selected-copy loop.
Sources are always retained for selected copies. See the Linux persistence
contract for mount boundaries, permissions, bounds and concurrent rename limits.

Before native Windows, Linux-to-Windows or selected-file transfer mutation, the
coordinator resolves the explicit destination alias once, captures the physical
ancestor chain and flushes existing parents bottom-up through the drive, volume
or share root. The endpoint retains its existing file/directory commit fences.
Each captured directory keeps a name pin denying deletion/rename and a separate
read barrier. The chain is bounded to 256 parents plus its endpoint; unsupported
opens or barriers fail the preflight. Native source-parent flushes and moves
exchange matching ancestry barriers together with the walk's existing guards.
Selected children borrow their coordinator's chain rather than fencing it again.
The final job callback follows release of all job-owned ancestry/walk guards.

The ordinary token first attempts each parent flush through an empty-name,
handle-relative writable reopen. If that returns access denied, it restores the
read barriers before requesting Windows elevation for the sibling
`folderwing-durable.exe`. This disposable broker serves only this preflight and exits
after it. There is no resident service, administrator GUI, ACL change or
volume-wide flush. Windows may show a UAC consent prompt when elevation is
required; denial, a missing helper, unsupported access, bad replies, disconnects,
timeouts or cancellation fail before destination mutation or source removal.
An unsuccessful elevated attempt is not retried with weaker semantics.

The broker accepts fixed-size, versioned frames containing captured directory
handles and expected identities, never paths, data, delete or rename requests.
The random named pipe rejects remote clients and restricts access to the current
user and SYSTEM. The host checks the launched client's process ID; the helper
checks the server's process ID, creation time, user SID and session before
duplicating handles. It joins the caller's kill-on-close job before its HELLO.
Every request must have the next sequence number and a valid directory identity.
The helper rejects non-disk objects, files, reparse directories and non-local
volume paths. It reopens the exact held object, verifies identity/type, performs
a full FlushFileBuffers and closes all request handles before returning its ACK.
If ordinary elevated access is denied, it can enable its own restore privilege
and repeat that same handle-relative open. Sharing violations remain errors.
The host validates the complete reply before proceeding. Pipe operations have a
30-second deadline; Windows owns the interactive UAC consent interval. On a
broken exchange the host closes the broker job before restoring read barriers.
The broker is launched only after a local access denial, once per preflight;
there is no per-file elevation in a folder or selected-file transfer.

For a streamed file directly inside the endpoint, the root's read barrier stays
held while data is transferred. Commit opens and flushes that captured root in
a synchronous handoff, restoring its read barrier before the file ACK. Root
directory metadata and child-to-root directory fences use the same captured
root. Existing descendant, publication and source-removal checks remain checked.

The access requirement follows the documented
[FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)
and normal (flags=0)
[NtFlushBuffersFileEx](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntflushbuffersfileex)
contracts. The separate elevated capability experiment measured successful
directory-scoped flushing, including handles duplicated from the ordinary app;
this integration still reports any access or filesystem limitation as an error.

The ordinary native Windows regular-file writer duplicates the actual CopyFileEx staging
handle with writable access, retain its sharing restrictions, apply the existing
metadata/DACL policy and flush before handle-based publication. Source/version
and replacement-target guards are repeated after that blocking flush. The same
file handle is flushed again after publication. A failed pre-publication flush
removes only the owned staging object; a post-publication failure preserves the
new destination. Neither path removes a move source after a failed barrier.

For native copies with named data streams, the CopyFileEx callback retains a
separate duplicate of the current named destination stream with its original
access rights. Each stream transition checks a full flush of the preceding
stream; each named duplicate is closed with checked error handling. After a
successful CopyFileEx, the last named stream is flushed and closed before
publication. Empty named streams receive the same barrier. The first stream's
publication anchor remains held through the existing metadata/DACL policy,
pre-publication flush, rename and post-publication flush. Stream handles are
never reopened through names. Single-stream files keep their existing batch or
ordinary barrier sequence.

Stream flush, duplicate or close failures prevent publication and move-source
removal. A failed stream barrier also stops later selections. Copy failures and
cancellation close retained named handles without treating partial data as a
commit; pre-existing destinations and sources remain. A stream flush error takes
precedence over simultaneous cancellation and subsequent close errors. Named
streams remain excluded from the small-file batching path, including files
encountered in an otherwise eligible folder. Selected regular Windows children
use this same checked writer. Same-volume renames do not copy stream data.

This follows Microsoft's [CopyFileEx per-stream flushing guidance](https://devblogs.microsoft.com/oldnewthing/20221007-00/?p=107261)
and [stream-switch callback contract](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nc-winbase-lpprogress_routine).
The native regression oracle tracks real callback stream IDs and their retained
destination handles, checking barrier/close ordering before successful publication,
individual failure injection, source and previous-target retention, cancellation,
empty and Unicode streams, selected children and batch exclusion. These checks
establish checked API barriers and live outcomes; interruption recovery for named
streams still requires a separate isolated-guest run.

A normal standalone copy flushes its containing directory before completion.
Recursive copies flush a created/merged directory and parent before descent,
then flush them again after all children and checked final directory attributes.
Per-file directory fences can therefore be batched within a source-preserving
folder copy; each regular file still has both file flushes. Selected regular-only
children use their owning selected-tree directory barriers. A reparse leaf uses
its containing directory for persistence, never the target of the link.

Local fixed-drive NTFS folder copies with at least 32 entries can group new, single-stream
regular files of at most 1 MiB into batches of at most 32 held staging objects.
Each stage receives a checked `NtFlushBuffersFileEx(FLUSH_FLAGS_NO_SYNC)`
writeout of both data and metadata. A full `FlushFileBuffers` on the first held
stage then synchronizes the underlying storage cache before any batch rename.
All stages are verified to be on the captured destination's volume. Only after
that barrier and a cancellation check can the job publish files by their held
handles, without replacing a target that appeared meanwhile.

Each published file receives another data-and-metadata writeout. The published
prefix always receives a checked full storage fence, including after a later
rename/writeout error or cancellation. Unpublished stages are removed by handle;
published files remain. The containing directory's original final barriers still
precede successful job completion. Progress counts committed files at batch
boundaries, and failed commit barriers stop later work. All operations and
callbacks remain on the job thread; no asynchronous flush worker owns paths or
outlives the job.

This retains durable data before publication and checked file/namespace barriers
before completion while amortizing full storage-cache flushes. It follows the
writeout-then-full-fence group-commit strategy documented by
[Git](https://git-scm.com/docs/git-config/2.54.0.html#Documentation/git-config.txt-corefsyncMethod).
Microsoft distinguishes data-and-metadata writeout without storage synchronization
from a full synchronized flush in
[NtFlushBuffersFileEx](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntflushbuffersfileex).
Writeout alone is never accepted as a durable commit. An actual writeout or
full-fence failure is reported, including unsupported-operation errors, without
silently retrying through a weaker path.

Moves, replacements, large files, additional streams, reparse leaves, selected
regular-only children and nonlocal/non-NTFS destinations retain the ordinary
per-file barriers. When the optional native writeout API is unavailable before
work starts, copies use the ordinary writer. No helper protocol or user setting
changes, additional threads or administrative volume handles are involved.

Cross-volume moves require destination file and containing-directory barriers
before guarded removal of each source file. All source handles close before
flushing the source parent. Final destination-directory/parent barriers precede
source-directory removal. Directory cleanup opens the final leaf without
following reparses, compares its identity with the captured directory, and
removes that held object; its captured ancestors stay pinned. Directory moves
are incremental: acknowledged children can already be gone when a later barrier
fails. A source-parent flush failure after removal reports an error with the
source already absent and the verified destination retained.

Same-volume moves retain the fast, atomic handle rename. The two captured
parents are flushed before rename, source/version is rechecked, then both
parents are attempted again after rename. A failure before rename leaves the
source; a failure afterward reports an error with the new destination name
retained. Cancellation never attempts rollback of a successful rename. This is
a namespace-persistence contract for pre-existing content, not a recursive data
flush of an existing directory tree or of concurrent external writes.

The native directory writer uses the same held-object reopen mechanism as the
selected-copy guards. Matching read barriers are restored and their identities
validated before continuing. Failed barriers stop remaining selections; final
job errors are preserved rather than presented as a successful copy or as a
user cancellation. There is no volume-wide flush or ignored unsupported error.

These are checked filesystem persistence barriers, not a transaction over an
entire directory tree or a power-loss certification. Pre-existing endpoint
ancestry, filesystem/device write-cache behavior and OS/VM/storage recovery
still require separate validation. No administrative volume-wide flush is
performed. Native Windows-to-Windows jobs use the writer contract above.

The implementation preserves the exclusive staging handle, leaf-relative
rename and existing destination-parent guards. It does not close/reopen the
staging path or replace the operation with a path-based move. Faults are
injected only by the native transfer/directory test executables, which include
the actual writer and link the remaining production engine. The shipped app has
no test endpoints, fault switches or new elevation requirements.

The streamed replacement writer retains the target identity and version from
stream start. Like native Windows replacement, it copies the existing target's
DACL and inheritance protection onto the held staging file before its first
flush. The target handle requires READ_CONTROL, and the replacement stage
requires WRITE_DAC; a security read or write failure prevents publication,
returns a failed ACK, and retains the existing target and move source. The
already granted staging handle rights allow cleanup even after a restrictive
DACL is applied. New destinations retain normal parent inheritance. Owner,
SACL, and Linux ACL translation are outside this DACL retention contract.
After the final staging flush it opens a target handle denying
new content writers, validates identity, size, attributes, creation/write times
and change time, and holds that handle through publication. Changed targets
produce ERROR_FILE_INVALID (ESTALE in the Linux ACK), and open writers or missing
targets fail explicitly. The current destination and move source are preserved.
An initially absent destination still uses exclusive, non-replacing publication.
The containing-directory guards retain their existing namespace scope; this
version check does not promise a transaction against arbitrary external changes.
As with the other source/destination version guards, a content rewrite that
leaves every compared metadata field unchanged cannot be detected by this check.

Microsoft documents that `FlushFileBuffers` requires a handle opened with
`GENERIC_WRITE`, and that its failure is reported through `GetLastError`.
The writer already opens its staging handle with that access. See
[FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers).


## Windows sources sent to Linux

Protocol v5 Windows-source checks enumerate streams through a held object
handle using `GetFileInformationByHandleEx(FileStreamInfo)`. Only the unnamed
default data stream is supported by this transfer route. Named streams (even
empty streams and streams on directories), reparse sources and failed/unknown
inventories produce explicit path errors. Filesystems which report no named
stream capability require no stream enumeration. Windows-native copies retain
their existing stream-preserving writer.

The helper requests these checks before node mutation, before regular-file
publication and before copied-source deletion, with additional final directory
checks. The host validates the source root, operation and literal relative
suffix before querying; it returns an errno ACK. Rejected copies keep old
regular targets, and rejected moves retain their source. Later refusal before
removal keeps an already committed destination too. Protocol-version mismatch
refuses old helpers rather than silently omitting these checks. POSIX final
check/rename/unlink races remain subject to the existing version-check limits;
this is not an exclusive lock over all NTFS streams or an OS-crash certificate.

API contracts: [stream inventory](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getfileinformationbyhandleex),
[stream records](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_stream_info),
[NTFS stream sharing and timestamps](https://learn.microsoft.com/en-us/windows/win32/fileio/file-streams).


Linux transfer protocol v6 adds bounded ACL/xattr records to cross-distro nodes;
the Windows coordinator relays their canonical bytes without name conversion.
Linux regular-file digests bind data plus metadata, and Linux importers verify
metadata before the normal publication/ACK fences. The Windows streamed writer
has no Linux ACL/xattr representation; its helper refuses nonempty Linux-source
metadata rather than reporting a lossy move. This does not change the existing
Windows destination-DACL retention policy or named-stream refusal checks.

## Native Windows directory named streams

The native directory copy/merge writer cannot preserve a directory's own named
streams. It refuses a source with any named stream, including an empty stream,
with `ERROR_NOT_SUPPORTED` before target creation or child mutation. Inventory
errors also fail the operation; unknown inventory is never treated as empty.
Checks use held directory handles and run again after destination barriers and
on the source-removal handle. A failed final check retains the source directory.
Previously completed children may already have moved; moves remain incremental.
Existing destination directory streams are retained during a supported merge.
Child-file streams continue to use the stream-preserving file writer.

A same-volume rename that keeps the original directory object preserves its
streams and remains supported. The shared inventory also guards Windows-to-Linux
sources. Enumeration uses bounded growing buffers, validates record bounds and
fails explicitly on unsupported inventories. These checks do not create an
atomic lock over all NTFS streams: a concurrent stream writer can race the final
check and deletion. Use stable sources for a complete fidelity guarantee.


## Opt-in Windows preparation credits (protocol v7)

The Windows coordinator environment variable
`OPENPILOT_WINDOWS_PREPARE_WIDTH=2/4/8` requests bounded, copy-only preparation.
Unset, 0, 1 or invalid values retain serial behavior. The helper and runtime
require protocol v7 together; mixed versions fail HELLO before job mutation.

New ordinary Linux-source files of 1..1 MiB with allocated payloads and ASCII
leaf names can hold up to K private destination stages. FILE_PREPARED is only a
staging credit, not durable completion. Monotonic tokens correlate ordered
FILE_COMMIT and FILE_COMMITTED records. Linux retains source/parent descriptors
and version snapshots through the durable result, repeating source version and
empty-metadata checks before requesting commit. Windows keeps every ordinary
metadata, prepublication file flush, exclusive handle publication, postpublication
file flush and containing-directory barrier. No parallel committer or source
removal is introduced. Only durable success advances completed bytes/items.

Moves, selected leaves, replacements, Windows sources, empty/unallocated/large
files and non-ASCII leaves use the existing writer. Pending case aliases, name
conflicts/autonumbering and directory boundaries drain before namespace or final
metadata work. Unsupported Linux ACL/xattrs still fail explicitly. Cancellation
and errors drain/abort accepted stages; failed ACKs keep sources, postpublication
failures keep completed targets, and abandoned stages remain for review.
Payload staging is bounded to K MiB, plus bounded names/handles and owner buffers.

The prototype remains opt-in and uninstalled. Focused real-writer/protocol and
coordinator-process interruption checks pass; broader v7 native integration and
Windows guest power-loss qualification remain pending. See the [prototype
results](../build/windows_prepare_queue/20261022/RESULTS.md).

## Windows group commit (protocol v8)

Eligible small-file copies from Linux to a local fixed NTFS drive default to
bounded group commit at width 8. Set `OPENPILOT_WINDOWS_PREPARE_WIDTH=0` to use
serial transfer, or set `OPENPILOT_WINDOWS_GROUP_COMMIT=0` to retain ordinary
per-file commit. Widths 2, 4 and 8 remain explicit preparation overrides. Invalid
override values disable that option. The helper and Windows engine must both speak protocol v8;
version checks still reject mismatched peers. This is not a persisted app option.

The job's preparation-width field adds `WSLP_GROUP_FLAG` (0x100). The masked
width must be 2, 4 or 8, the operation must be Copy, and the destination must be
Windows. Staging credits still mean only that a bounded stage was retained.
Messages 65–68 carry a bounded group command, ordered fresh source checks,
correlated source-check answers and the final durable per-file result array.
Count, size, order, token, request ID, padding and errno checks fail closed.
Linux validates the complete result array before crediting any member, retains
its source handles until that result, and never removes a source on this route.

Eligible members are new ordinary files with held stages and the same captured
NTFS parent and volume. Replacements, ancestry handoffs and unsupported groups
use the ordinary per-file path before any group writeout. Each eligible stage
receives final metadata and a checked `NtFlushBuffersFileEx(NO_SYNC)` writeout
of data **and** metadata. One full `FlushFileBuffers` fence completes after those writeouts and before
any rename.
Each rename requires a fresh held-source version/metadata check, uses exclusive
publication and receives a checked post-publication writeout. The published
prefix then receives a full file fence and held-parent fence before a durable
result is returned. These final fences also run after cancellation, a later
writeout/rename failure or a broken source-check channel. Published files are
retained. A failed final fence denies success credit to every published member.
There are no parallel committers, volume handles, relaxed barriers or automatic
transfer retries. Existing directory and destination-ancestry barriers remain.

Standing coverage lives in `tests/windows_transfer_tests.c` (real held NTFS
objects and post-API fault injection) and `tests/test_windows_prepare.py` (real
Linux helper with a synthetic Windows peer). Synthetic peer results do not
establish NTFS persistence. The earlier isolated-guest power-off qualification
is recorded under `build/windows_group_commit/20261022`; production integration
has its own source, binary and result records under
`build/windows_group_integration/20261022`. The private-app release enables eligible group8 by default after the final
production-source recovery cohort in `build/windows_group_release/20261022`.
