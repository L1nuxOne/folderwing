# Search inside files

**Search inside files** (`file.find_content`, default **Ctrl+Alt+F**) opens a
separate **Find in files** panel for a folder. It captures the selected folder,
or the current folder when the target is a file/background. Navigating either
pane does not change that captured root. **Find in files** (`content.show`) in the
command palette reopens the retained panel/results.

Disabling WSL integration through Settings retires retained native searches,
including hidden panels, and invalidates their result actions. Windows-only
searches retain their results; later Start uses the current WSL policy. A fresh
native search can be opened after WSL integration is re-enabled.

## Interaction

Enter a literal, single-line query in **Find** and press Enter or **Start**.
**Files** accepts filename patterns, including semicolon lists such as
`*.c;*.h;*.py`. Empty or `*` searches all regular filenames. Patterns match leaf
names, case-insensitively. **Case** controls content case sensitivity;
**Whole word** requires letter/digit/underscore boundaries. **Subfolders** controls
recursion. Hidden files are included; ignore files such as `.gitignore` are not
interpreted in this first version.

Each result shows its relative path, one-based line/column and a matching-line
excerpt, with the actual occurrence highlighted. Multiple occurrences on the
same line have separate rows. Excerpts of long lines are bounded around the
match. Search results retain exact UTF-16 positions and the case-preserved matched
text, not just the query.

Click a result then press Enter or **Preview match**, or double-click it, to reveal
the file in the active pane and select the match in the existing text reader.
The reader scrolls that match into view. Files with otherwise unrecognized text
extensions can use the plain-text reader through this explicit action.
Right-click/Shift+F10 offers Preview at match, Reveal in left pane, Reveal in right
pane and Copy path. The **Reveal file** button uses the left pane.

Tab moves between fields, options, Start/Stop, results and action buttons. Fields
support typing, arrows/Home/End, Backspace/Delete, Ctrl+A and Ctrl+V. Up/Down,
Home/End and Page Up/Down navigate the results. Ctrl+C on results copies the path
through OpenPilot's existing clipboard controller. Escape stops a running search
or hides the idle panel.

**Stop** retains incomplete results. Changing a query, pattern or option stops
obsolete work and disables old result actions until **Start** runs the new search.
Starting again replaces the previous rows. Hiding the panel preserves its worker
and results; reopening reuses them. Closing OpenPilot or disabling this function
retires its helper. Opening the panel does not start a content scan until Start.
Ordinary browsing launches no content-search helper.

## Windows and WSL

`folderwing-content.exe` owns the panel, one background search worker and a bounded result
model. Windows traverses directories through held parent handles and reads files
without following reparse points. WSL roots launch `folderwing-wsl --content-search` only
when requested and native WSL integration is enabled. Linux traversal is native,
descriptor-relative, and excludes symlinks, special files and nested mount
boundaries, including same-device bind mounts. The explicitly selected root may
itself be on a mounted filesystem. The configured WSL user is captured; there is
no automatic elevation or WSL/Docker shutdown.

Paths/options travel through bounded binary pipes, not interpolated shell
commands. Content search and text preview share `textutil.c`: UTF-8, BOM-marked
UTF-16/32, the existing BOM-less UTF-16 detection and explicitly guessed
Windows-1252 fallback. CRLF/CR normalize exactly as in the preview. Line numbers
are one-based; columns count normalized UTF-16 units, not byte offsets or expanded
tab display columns. Case-insensitive matching uses simple code-unit case folding,
without Unicode normalization or full multi-character case expansion.

File metadata is checked before and after reading/decoding; observed changes are
reported instead of publishing those matches. This remains a best-effort scan of
live files, not an atomic snapshot. A later preview checks the actual matched text
at its line/column. If the file/encoding changed or the match is outside a limited
preview prefix, it reports the mismatch instead of selecting unrelated text.
Existing preview access limitations for native-only Linux names/permissions still
apply; native searching itself preserves their exact result paths.

## Bounds and exclusions

The first version searches literal text; regular expressions, replacement,
archive traversal and a persistent content index are deferred.

- Query/pattern fields: at most 127 UTF-16 units each; one-line literal queries.
- Regular files up to 2 MiB are decoded in full using the preview decoder.
  Larger files are counted as skipped and make the result incomplete.
- Binary data is excluded, with a separate count. Filtered filenames, links,
  special files and recursion/mount exclusions are counted separately.
- At most 100,000 directory entries, 1 GiB of content read, 10,000 matching
  occurrences, 32 MiB of retained path names and 128 traversal levels per search.
- Stop, unreadable/changed entries and reached limits retain an explicit
  stopped/partial status. Zero matches can be a complete successful result.

The scanner reads no archive payloads, writes no source files, performs no
replacement, and stores no user query/result history on disk. Helpers terminate
through the existing owned panel/child-process lifecycle.

## Validation

`tests/content_search.ps1` runs the real matcher/decoder, Windows and native Linux
workers, panel and explorer/preview integration against owned fixtures. It uses
owned nonactivated Windows windows and messages, without physical input or
external clipboard changes. The host lane clicks real helper results, verifies an
exact selected match at Windows line 80 and Linux line 2, and checks stale
root/query actions, changed source, hidden-session reuse and feature deactivation.

[Implementation evidence](../build/content_search/20261022/RESULTS.md).
