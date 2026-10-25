# Duplicate finder

Choose **Find duplicates** from the folder context menu or command palette. With
folders selected it scans those folders (up to 16); with no selection it scans
the current folder. **Find duplicates in both panes** scans both displayed folder
roots. Selection in either pane works. The **Duplicate finder** command reopens
retained results; ordinary browsing never starts an analysis helper.

The separate `folderwing-duplicates.exe` panel scans recursively, narrows candidates by
logical size, computes SHA-256, and compares complete bytes before publishing a
group. Equal hashes alone are not an equality conclusion. Larger files appear
first; empty files can form a group but contribute zero duplicate bytes.

Select a row and use **Preview**, **Left**, **Right** or **Copy path**. Enter and
double-click preview the file through its existing file-type preview handler.
Right-click opens the duplicate-result menu. Up/Down select rows; PageUp/PageDown
and the wheel scroll; Home/End move to the ends; Tab/Shift+Tab reach the controls.
F5 refreshes. Escape stops an active scan or closes the panel when idle. Closing
or hiding retains results. Starting with new roots cancels the old worker, joins
it, then adopts the new roots. No deletion or replacement is offered.

## Results and limits

Progress distinguishes enumeration, hashing and byte verification. Read failures,
changed files, scan limits and cancellation never become a complete result.
Partial/stopped results may retain groups already verified. Results describe the
files when checked, not an immutable filesystem snapshot; refresh after changes.

Hard-link aliases are counted separately instead of presented as independent
copies. Windows overlapping roots retain their single-scan behavior. Native
Linux parents run first, and an explicit nested root is omitted only when the
completed parent scan proves that exact directory was entered and finished.
A separately requested root at or below an excluded mount still runs. Repeated
root aliases and already observed physical files are deduplicated; caller root
indices remain unchanged for result actions. Coverage is from the observed scan,
not an atomic filesystem snapshot. Symlinks, Windows reparse entries,
Linux FIFOs/sockets/devices and nested Linux mount boundaries are excluded. Scan
limits are 100,000 total metadata entries across retained roots, 64 MiB of stored
relative paths, and the scanner's existing depth/name bounds. Limits are reported
as incomplete. Windows offline files are not recalled for payload reads.

**Duplicate bytes** sums logical lengths beyond one copy per group. It is not an
estimate of reclaimable disk space: sparse files, compression, hard links and
other storage sharing can change that substantially. File metadata, permissions,
NTFS alternate streams, Linux xattrs and resource forks are not compared. The
feature verifies regular-file data only.

Native Linux scanning and hashing run only for WSL roots with `wsl.helper`
enabled. A persistent, rooted `folderwing-wsl --duplicate-read` channel hashes in Linux;
exact cross-endpoint comparisons stream bounded blocks. Raw Linux names,
including punctuation and invalid UTF-8 bytes, use the existing reversible path
codec. Traversal is descriptor-relative, refuses links/special files, and checks
identity/size and modification/change metadata across hashing and verification.
Windows readers likewise open rooted components and recheck their identity and
metadata. Disabling **Find duplicates** stops its helper and rejects its actions.
Result actions carry the host's root session and the helper's scan generation.

## Focused validation

```powershell
./tests/duplicates.ps1 -BuildDirectory ./build/release
```

This runs the complete duplicate engine/panel executable and real-host acceptance
against owned Windows and Linux fixtures. It covers true duplicates, same-size
different content, forced hash collisions, empty/multi-block files, hard links,
overlapping roots, phase cancellation, mutation and replacement, inaccessible
inputs, path guards, native raw names, cross-endpoint groups, rendered controls,
worker supersession, hidden reuse, preview/reveal and disabled-feature cleanup.
It uses owned windows/messages without physical input or stopping WSL/Docker.
No full application suite or performance audit is claimed for this feature batch.
