# Linux transfer persistence

Selected-file/tree copies also use pinned-directory barriers. A guard creating
descendants fsyncs every opened child and its parent before returning its anchor.
Protocol v4 guard-sync requests act only on that live descriptor and return a
checked errno ACK after fsync; only EINTR is retried. The coordinator waits for
post-copy barriers from the containing directory through the submitted endpoint
before reporting completion without errors. Failure stops remaining selections,
retains every source, and preserves already published copies. No pathname is
reopened for a final Linux directory barrier.

Before native Linux copy/move mutation, the helper also fences the existing
parents of the held destination, bottom-up through its mount root. The importer
does this before its initial acceptance ACK; the selected-copy coordinator does
it once on the pinned destination-root guard before creating children. Selected
regular-only leaf requests inherit that coordinator precondition, avoiding a
second ancestry walk per file. This includes same-volume native moves, whose
first rename can otherwise remove a source immediately.

The walk opens `..` relative to held descriptors, not the submitted pathname.
It detects mount boundaries using mount IDs, including same-device bind mounts,
and does not flush the unrelated filesystem outside that mount. At most 256
parents are visited; a deeper chain, unavailable mount IDs, unreadable parent or
failed/unsupported flush is an error before transfer mutation/acceptance. Only
EINTR is retried, with cancellation checked around native-job barriers. A parent
relationship changed during a blocking flush is rejected. POSIX does not lock
the entire ancestry against later concurrent namespace changes by the same UID
or root; the preflight is not a filesystem-wide transaction. It does not flush
files that existed in ancestor directories or create missing destination roots.

Existing ancestors therefore need read/search access, not Linux write access.
An unreadable but traversable parent can now produce a truthful preflight error;
there is no elevation or fallback to an unfenced move. Windows destinations
retain the separate permission and persistence contract below.

Native copy jobs and cross-distro importers treat failed persistence barriers as
transfer errors. `fsync` retries `EINTR`; unsupported operations and other errors
are reported without silently falling back to an unflushed commit.

For a staged regular file, the writer applies the final metadata and flushes the
held file before checking the source/destination guards and publishing it. The
cross-distro importer also flushes before its SHA-256 readback; its second file
flush includes the final metadata. For symlinks, which cannot be flushed through
their `O_PATH` descriptors, the writer flushes the private staging directory after
creating the link and applying its metadata.

Publication atomically renames the staged object, then flushes both the
destination directory and the staging directory. Only successful completion
allows a committed ACK or copy-based source removal. Cleanup marks publication
immediately after the rename: a later flush failure leaves the complete visible
destination in place and retains the source. An overwritten old destination is
not restored after a successful rename, even if a later barrier fails.

Every entered destination directory and its parent are flushed before descending
into children, including existing merge directories. An existing interior entry
may have been created or renamed recently; fencing the endpoint's ancestry does
not replace these pre-descent barriers. Native recursion and streamed directory
acceptance both require the initial pair before a child can lose its source name,
including an atomic same-volume child rename. Errors prevent descent/acceptance
at that directory and retain its source children. Existing mount checks, metadata
compatibility checks, cancellation handling and final directory barriers remain
in force; only EINTR is retried.

Directory completion flushes final metadata and its parent before the final
committed ACK or source-directory removal. Regression fixtures exercise multiple
unsynced interior merge levels, each initial pair boundary, same-volume rename,
EXDEV copy-based moves, streamed imports, cancellation, symlinks and replacements.
They establish ordering and live retention; isolated-guest interruption recovery
without a pre-synced interior merge baseline is a separate batch qualification.
Moves remain incremental:
children may already have committed and left their source paths when a later
directory or sibling operation fails; the remaining source tree is retained.

After guarded source unlink/rmdir, the source parent is flushed. If that flush
fails, the job reports an error with the source already absent and the verified
destination retained. Retrying the original move may therefore find no source.
No rollback or all-or-nothing directory transaction is promised.

Same-volume native moves retain atomic rename and flush both participating
parents afterward. They do not recursively read or flush pre-existing file
contents. When no-replace rename is unavailable and a leaf uses link/unlink,
the destination parent is flushed before source unlink, then both parents are
flushed on completion. A failure between link and unlink can leave both names.

These guarantees assume the filesystem/storage stack correctly implements
successful `fsync` calls and the mount namespace is already established.
Private workspace removal is cleanup, not a separately fenced transaction; an
empty workspace may reappear after a crash. POSIX still provides no atomic
unlink-if-version-matches, so the documented final source-check/unlink interval
remains. Descriptor and namespace checks continue to apply around the barriers.

The Linux tests verify barrier ordering, returned errors, ACK behavior and
visible source/destination state using private fixtures. They do not establish
power-loss behavior for a distro, VM, device or network filesystem. The tmpfs
fixtures exercise cross-device control flow only. Windows destination writers
and their `FlushFileBuffers` policy are described separately in
[Windows streamed-transfer flushing](../src/WINDOWS_TRANSFER_DURABILITY.md).

Native regular-file and symlink replacements retain the destination inode and
its resolved version through staging. After the payload barrier, both that
held object and its current name must still match the original identity, size,
mode, ownership, mtime and ctime. Changes, substitutions and disappearance fail
with ESTALE, preserving the current target and move source. An initially absent
target uses exclusive publication even under overwrite policy. POSIX provides
no atomic rename-if-destination-version-matches; the final check/rename interval
remains, just as the final source check/unlink interval does.
Content rewrites leaving every compared metadata field unchanged are outside
these metadata-version checks, as with the existing importer/source guards.

## Windows-source named streams

For explicitly submitted Windows sources, DrvFS reads only the unnamed data
stream. Protocol v5 therefore requires a Windows `SOURCE_CHECK`/`SOURCE_ACK`
exchange before copying a node, after a regular payload is flushed but before
publication, and immediately before copied-source removal. Directories are
checked before descent and after their children as well. A named stream,
including a zero-length stream or a directory stream, returns ENOTSUP. Failed
inventory access returns its error; neither is treated as an empty inventory.
Malformed, missing or cancelled ACKs cannot authorize publication or removal.
New-file templates and selected-file copies use the same checks.

A rejected regular file keeps the existing destination and source. A failure
at the removal boundary keeps both the completed destination and original
source. Ordinary siblings can finish; a failed child keeps its containing
source directory. This is refusal of an unsupported representation, not stream
translation or an implicit request to discard metadata. Native Linux-only and
cross-distro transfers do not acquire Windows stream checks. The coordinator
bounds source requests to the captured Windows root and literal relative suffix.

The existing source-version checks still run after blocking ACKs. There is no
atomic POSIX check-and-unlink, and no claim of exclusion against all concurrent
stream/namespace changes between the last checks and publication or unlink.
Windows paths reached only through a Linux-mounted alias are outside the
explicit Windows-source classification used by this protocol.


## Linux xattrs and POSIX ACLs

Protocol v6 carries each cross-endpoint node's metadata as sorted, bounded
name/value records following its name and optional link target. The aggregate
metadata cap is 64 KiB including record overhead. Names are NUL-terminated
Linux xattr names; values are binary and may be empty. Duplicate/out-of-order
names, malformed ACLs, invalid lengths and unsupported namespaces are rejected.
The existing regular-file SHA-256 now binds data followed by canonical metadata;
empty metadata retains the former data-only digest. Readback verifies both file
data and applied metadata before the committed ACK. The Windows relay forwards
these bytes unchanged and the required protocol version prevents older peers
from silently omitting metadata.

Local copies and imports share `fpmetadata.h`. Supported source metadata is the
visible `user.*` namespace and Linux POSIX access/default ACLs, retaining numeric
ACL subject IDs and Linux's v2 xattr ABI. Owner/group policy stays unchanged:
the default helper owns new copies; root mode retains numeric ownership. ACLs
are not translated between account-name databases or other ACL models.

Unknown visible namespaces, including security labels, capabilities, trusted
attributes and other system ACL models, are explicitly unsupported by copied
transfers. List/get/set/remove/readback failures are errors, not empty metadata.
Kernel access controls can hide names such as `trusted.*` from an unprivileged
caller; this contract covers metadata visible to the chosen helper identity,
not an elevated discovery of hidden attributes. Same-filesystem rename moves
retain their original inode and metadata without imposing the copied-transfer
namespace restriction. Symlinks remain literal leaves; visible symlink xattrs
are refused because that metadata cannot be safely represented by this writer.

A private staged file loses inherited supported metadata, receives source user
attributes and ACLs, then final mode/ownership/times are verified and flushed
before publication. Thus chmod cannot activate unrelated named-user permissions
inherited from the destination. A newly copied directory stays mode 0700 while
its user/default metadata is prepared and verified before the initial ancestry
fences or any child ACK. Access ACL and final mode are applied at completion.
Default-ACL preparation failure prevents child mutation. Existing merge targets
retain their metadata policy: nonempty source metadata must match before
children and again at completion, or the source directory is retained with an
explicit error. Empty source metadata does not erase an existing merge policy;
copied child files and new directories still normalize their own metadata.

Supported source metadata is snapshotted and checked again after blocking
preparation and before copied-source removal. Metadata changed during copying
cannot silently authorize source deletion. A remote change after destination
commit can leave the completed snapshot alongside the changed source with an
error; committed destinations are not rolled back. Directory moves remain
incremental, so safe child transfers can finish while a directory-level error
retains the remaining source directory and its metadata. Linux sources with
nonempty metadata are refused by the Windows bridge, whose node representation
cannot preserve Linux ACLs/xattrs. Windows-source stream checks remain required.

All previous data/namespace persistence fences, cancellation behavior, held
object checks and retained abandoned-stage policy apply. POSIX still has no
atomic metadata-check-and-rename/unlink, and a same-UID/root actor can race the
last checks. These are verified filesystem barriers, not power-loss evidence.

References: [Linux xattrs](https://man7.org/linux/man-pages/man7/xattr.7.html),
[POSIX ACL inheritance and chmod](https://man7.org/linux/man-pages/man5/acl.5.html).


## Checked read-handle reuse and bounded Windows reads

The local helper copy path reuses a successful mount-checked regular-file read
handle instead of closing an `O_PATH` probe and opening the source again. Move
paths keep their original pre-rename mount checks. Skip policy also keeps its
original probe so an existing target does not trigger an eager data-access
request. If the checked read open fails, the original mount check and conflict
resolution run before a later copy read; access failures do not override Skip
or an existing-target refusal. Unconsumed handles are closed on every return.

For explicitly classified Windows regular-file sources, copying reads exactly
the captured nonnegative length. A premature EOF fails with `ESTALE`; unchanged
held-object and pathname/version checks before and after reading, after payload
sync and before publication reject growth, edits, truncation or replacement.
Native Linux sources still stream to EOF, including pseudo-files whose stat
length is zero. This optimization changes neither metadata checks nor flush,
publication, ACK or source-removal sequencing. It does not extend Windows-source
classification to unmarked Linux aliases or remove the existing final-check race
limitations. See the [focused transfer results](../build/transfer_latency/20261022/RESULTS.md).

## Experimental bounded native copy commits

`OPENPILOT_LINUX_COMMIT_WIDTH=2`, `4` or `8`, set in the Linux jobs helper's
own environment, enables an opt-in copy prototype. Unset, `1` and other values
retain the serial path. It does not change the wire protocol or enable parallel
Windows writes or moves. Empty/unallocated files and files larger than 1 MiB
stay serial. The owner still streams native sources to EOF; an admitted copy
exceeding 1 MiB fails before publication. At most K queued files hold K MiB of
payload staging and K bounded metadata snapshots (64 KiB each), plus the owner's current preparation buffer/snapshot, held
source/parent/overwrite descriptors and literal paths. A single owner reads
sources and job-control messages, validates Windows source replies, emits
progress/errors and publishes files. Workers never access mutable job state.

Each slot holds its own private staging workspace, read descriptor, source
parent/name/version, metadata snapshot and optional overwrite descriptor/version.
A worker flushes and validates the prepared payload. The owner then repeats
Windows stream checks or Linux metadata checks, source version/path checks and
overwrite checks immediately before the guarded rename. After publication a
worker attempts both destination-parent and staging-directory flushes and
finishes the payload. Only successful completion of those steps adds committed
bytes to progress; error items retain the existing processed-item accounting.
There is no early durable ACK or source-removal authorization.

Duplicate pending names and auto-number conflicts drain before resolution.
Directory boundaries drain and retire cached workspaces before final directory
metadata and barriers, so cleanup cannot invalidate finalized directory times.
Cancellation stops admission/publication, settles published work through both
namespace barriers and joins every owned worker before releasing handles.
Inflight kernel flushes may delay draining. Failed or cancelled copies retain
sources; published destinations survive subsequent errors. Process interruption
leaves unacknowledged staging/publications for review, without automatic cleanup
or replay. Existing POSIX final-check race limits still apply.

This is a guarded prototype, not a replacement for the serial default. Focused
queue tests and exact complete-transfer comparisons do not close conservative
native integration or guest power-loss qualification. See the
[prototype results and remaining gates](../build/linux_commit_queue/20261022/RESULTS.md).


## Copy-only Windows preparation credits

Protocol v7 accepts an explicit Windows preparation width of 0 (serial), 2, 4 or
8 in a copy job with a Windows destination. Other combinations fail admission.
The opt-in coordinator prepares bounded small new files while Linux retains
source/parent descriptors, literal names and version snapshots. A staged credit
is not a durable result and never enables source removal or completed accounting.
Before each ordered commit request, the owner rechecks source identity/version
and empty Linux metadata. It closes each retained source only after the matching
durable/failed result or a broken connection. Cancellation drains accepted credits.

Moves and unsupported queue cases keep their existing serial writer/ACK path.
The queue has no source-removal operation and no shared job-state worker thread.
Source version checks retain their existing nontransactional POSIX race limits.
All original Windows per-file/directory fences remain required. See
[Windows persistence and queue scope](../src/WINDOWS_TRANSFER_DURABILITY.md) and
[focused prototype evidence](../build/windows_prepare_queue/20261022/RESULTS.md).

### Optional Windows group commit, v8

Windows requests `WSLP_GROUP_FLAG` with preparation widths 2, 4 or 8 for Copy
to eligible Windows destinations. The Windows private-app release defaults to
width 8; the Linux helper enables grouping only when the job negotiates it. `fpwingroup.h` retains all held source descriptors through the complete
correlated durable result, validates sources once before the group command and
again for every ordered fresh publication check, and validates the entire result
array before adding any completed bytes. Bad tokens, order, sizes, padding,
request IDs, errno values or unvalidated successes fail closed. Cancellation is
drained, and persistence errors remain reported even when cancellation also
occurs. This route never removes sources. Windows eligibility and persistence
barriers are documented in `src/WINDOWS_TRANSFER_DURABILITY.md`.
