# File differences

Show both file panes and select one file in each (the cursor file counts when
there is no explicit selection). Press **Ctrl+Alt+D**, choose **Compare files**
from the item menu, or use the command palette. The captured left file stays on
the left even when the command is invoked from the right pane.

The on-demand **File differences** panel runs in `folderwing-diff.exe`. Drag the panel's
left edge to give the comparison more width. The main explorer does not read,
decode or align the files. Nothing is edited or written back to either source.
Selecting other files does not silently change the captured comparison; invoke
Compare files again for a new pair. Opening no comparison starts no diff helper.

## Reading and navigation

Both columns share aligned rows and scroll positions. Blue outlines mark the
selected row; amber marks changed lines, red marks removals and green marks
additions. A gap on one side preserves alignment. Original one-based line
numbers remain visible; a missing final newline is labelled explicitly.

| Control | Action |
|---|---|
| Previous / Next, Shift+F7 / F7 | Go to the previous/next change block, wrapping at either end |
| Wheel, Up/Down, Page Up/Down, Home/End | Scroll both columns together |
| Left/Right, Shift+wheel | Scroll both columns horizontally |
| Tab / Shift+Tab, Enter / Space | Focus and activate toolbar controls |
| Stop or Escape during work | Cancel without declaring incomplete data equal |
| Refresh or F5 | Read the same captured file pair again |
| Escape when idle | Hide the panel |

Hiding retains the current comparison. The **File differences** palette command
reopens it. Disabling `file.compare_files` retires only its owned helper and
captured paths. Host exit ends the helper through the existing panel job.

Disabling WSL integration retires retained native or mixed comparisons and
invalidates their sessions. Windows-only comparisons retain their results and
use the updated policy on Refresh. Both complete ordinary-Diff paths are checked
before either is opened; native-only escaped Linux identities report unsupported
access instead of being interpreted as Windows names.

## Text and byte equality

The text decoder is the existing preview decoder: BOM/heuristic UTF-8,
UTF-16/32 and Windows-1252, with CRLF/CR normalized to LF. Matching is exact after
decoding; whitespace, case and the presence of the final newline matter. The
panel shows detected encodings and flags guessed/replaced decoding.

**Bytes identical/differ** is independent of line alignment. Thus two files can
have zero decoded text changes and still differ in encoding, line endings or
decoding. Binary data receives an equality-and-size summary. The decoder's
existing heuristic determines whether an input is text; there is no manual
encoding override in this first version.

Text view covers complete files up to **2 MiB and 100,000 lines per side**.
Larger inputs use byte comparison with sizes. Equal-size inputs are read until
the first differing chunk or through their complete ends; size inequality also
proves byte inequality. A bounded text prefix is never used to claim full-file
equality. Equal results therefore include all bytes, even beyond the text limit.

Line alignment uses a bounded Myers edit search, with common-prefix/suffix
preservation, at most 1,024 edit levels and 8,000,000 search steps. Large rewrites
that exceed that budget retain the full text and show the remaining middle as
one **coarse alignment** change. Coarse alignment can include equal lines inside
that block; it never declares unmatched data equal. Replacements pair removed
and added lines in their original order. This version highlights whole lines,
without an additional character-level diff.

## File access and limits

The worker holds read-only handles. Windows sharing excludes existing writers
and new writes/deletes while those handles are held. It checks size, write time
and change time after reading, and reports a changed/unreadable source as an
error. This is a comparison of the captured reads, not a simultaneous atomic
snapshot of two mutable filesystems or a permanent equality guarantee.

Regular Windows files and WSL files accessible through the existing Windows
UNC provider are supported, including mixed pairs. WSL access is gated by the
enabled `wsl.helper` feature. This helper uses the provider's normal access;
it does not run Linux commands or elevate to the configured Linux root user.
Links/reparse points, directories, device/offline files and inaccessible inputs
are rejected. Native-only Linux names/access that the provider cannot open
remain unsupported. WSL and Docker are never stopped or restarted.

Stop requests cancellation of synchronous I/O and signals the alignment worker.
At 60 seconds a comparison is stopped automatically. A provider that delays an
I/O cancellation can delay reuse of that helper worker; the main explorer and
the panel input loop continue independently. A new pair cancels the prior job
and publishes only the latest requested pair. Results are held in memory and
are not saved as content history.

Editing, merging, unified patches, folder-comparison result integration and
manual encoding/ignore-whitespace options are follow-ups.

## Focused validation

`tests/file_diff.ps1` creates unique owned Windows and Linux fixtures and runs
`diff_tests.exe` and `diff_host_tests.exe` serially. It captures exact source and
binary hashes before execution, outcomes and timings, plus a rendering from the
actual helper buffer. It uses no physical input and does not change the external
clipboard. Cleanup names only the fixture entries it created.

The checks cover alignment against an independent LCS oracle, newline/encoding
semantics, binary and large-file tail differences, errors, cancellation,
supersession, hidden reuse and actual explorer-to-helper command dispatch.

Delivery evidence: `build/file_diff/20261022/RESULTS.md`.
