# Git integration

Press **Ctrl+Alt+G**, choose **Git status** from a folder/file context menu, or use
the command palette. A selected folder becomes the request folder; otherwise the
current folder is used. The on-demand `folderwing-git.exe` panel discovers the repository
root, including when invoked from a subfolder or linked worktree. **Git panel**
reopens retained results without refreshing them.

The panel shows the branch (including detached/unborn states), changed-path count,
and locally known upstream/ahead/behind information when present. All, Staged,
Unstaged, Untracked and Conflicts filters provide separate lists. A file with
both staged and unstaged changes appears in both categories. Rename/copy records
retain their original path. Ignored files are omitted.

Select a row for **Diff**, **Preview**, **Left**, **Right** or **Copy path**.
Right-click offers the same result actions. **Terminal** opens the existing
terminal helper at the repository root. Enter/double-click requests a diff;
Up/Down select rows, PageUp/PageDown and the wheel scroll, Home/End move to the
ends, Tab/Shift+Tab reach controls, F5 refreshes, and Escape stops or closes.
Buttons wrap when the panel is narrow. Hiding retains the snapshot and process.

File panes can show cached two-character Git status beside names, such as `MM`
(staged and unstaged modification), `.M` (unstaged modification), `R.` (staged
rename), `??` (untracked) and `UU` (conflict). A folder containing reported changes
gets `*`. These are **snapshots**, not live monitors: refresh Git status after
changes. Ordinary navigation/painting launches no Git process. Only the latest
explicitly requested repository is cached. Badges can be disabled in Settings:

```ini
[git.status]
badges = off
```

Disabling the **Git status** function retires its helper and clears its badge
cache. No Git parsing library or command execution is loaded into the explorer;
the core only parses a bounded returned snapshot and performs cached lookups.

## Diffs and execution

The existing `folderwing-diff.exe` viewer renders both Git comparison modes:

- **Staged:** the HEAD blob versus the index blob captured in the status snapshot.
- **Unstaged:** the captured index blob versus the current working file.

Blob IDs freeze the selected staged versions across later index/branch changes.
New/deleted blob sides are represented as empty data when appropriate. A retained
working-file deletion is checked again at comparison time: a recreated regular
file supplies its guarded current bytes; a still-absent path supplies an empty
side. Recreated unsupported types, read/access failures, cancellation and source
limits remain explicit failures. The captured status row does not certify current
emptiness, and staged comparisons keep their captured blob IDs. The viewer
retains its aligned text, linked scrolling, change navigation and binary byte
summary. Text display retains its 2 MiB/100,000-line bounds; Git source reads are
capped at 16 MiB per side. Over-limit/error/cancelled inputs never become an
equality result. These are raw file/blob comparisons; Git textconv, external diff
programs and working-tree filters are not run. Untracked files use Preview;
conflicts use the status list and repository terminal. Submodule/gitlink and
symlink/type-change diffs are explicitly unsupported in this first version.

Windows repositories use an installed `git.exe` (PATH, then the usual
Program Files Git location). WSL repositories use native Git through a private
`folderwing-wsl --git-read` mode; the installed Git in that distro is required. WSL source
handling is gated by `wsl.helper`; Windows repositories start no Linux helper.
Native paths use the existing reversible codec, including reserved punctuation
and invalid UTF-8 bytes. The existing preview handlers retain their own source
limits, so listing a raw Linux filename does not add preview support to a handler
that previously excluded it.

Only fixed inspection operations are exposed: root discovery, structured status,
reading blob objects, and reading regular working files. Processes use argument
arrays/escaping, inherited-handle allowlists, owned kill-on-close jobs, bounded
output and 30-second command deadlines. Inherited `GIT_*` routing overrides are
removed. Optional index locking, fsmonitor hooks, terminal prompts and lazy object
fetching are disabled. No network refresh, stage, commit, checkout, reset, clean,
fetch or push action is provided. Existing normal Git user/repository config is
otherwise respected, including repository trust/access restrictions.

Status capture is capped at 2 MiB, 5,000 changed paths and 1 MiB of decoded names;
exceeding a limit shows an unavailable snapshot rather than a clean repository.
Failures display the error/exit code. Repository results carry a host-issued
request session and helper generation; actions must refer to a path in the
accepted snapshot. New requests cancel and join the old worker before adoption.

## Focused validation

```powershell
./tests/git_integration.ps1 -BuildDirectory ./build/release
```

The complete Git engine/panel and real-host executables use owned Windows and
Linux repositories. Fixtures cover staged/unstaged edits, rename/deletion,
binary content, ignored/untracked paths, conflicts, unborn and linked worktrees,
raw Linux names, inherited routing overrides, cancellation, cached badges,
filters, rendering, retained/superseded sessions, preview/reveal, actual Git diff
routing and repository terminals. The existing diff executable also runs as a
regression lane. Windows and Linux index hashes are checked for unintended
changes. No physical input, full application suite, new performance audit,
WSL shutdown or Docker shutdown is part of this feature gate.
