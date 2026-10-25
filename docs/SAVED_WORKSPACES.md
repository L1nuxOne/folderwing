# Saved workspaces

Press **Ctrl+Alt+W** or choose **Saved workspaces** in the command palette.
The native manager is part of the core/session code and opens only on demand.

| Action | Behavior |
|---|---|
| Save as new | Capture current folder tabs and layout under the entered name |
| Switch | Restore the selected saved setup, replacing the current tabs |
| Update selected | Explicitly replace the selected saved setup with the current tabs/layout |
| Rename selected | Change the selected saved label to the entered name |
| Delete… | Request confirmation; Delete again removes the selected saved entry |
| Reload list | Read changes saved by another app window or retry a load |

Switching and ordinary navigation **never update a saved workspace**. Save as
does not overwrite an existing name; use Update selected for that. Update and
Delete act on the selected row, regardless of text typed into the name field.
Deleting a saved entry keeps the currently open tabs. The manager identifies the
last workspace switched from that manager, without implying the current layout
still matches it. Double-clicking a saved row switches to it. Native controls
provide Tab/Shift+Tab, list arrows and normal edit shortcuts; Escape closes it.

## What is saved

- Tab order and exact folder paths in both panes, including the hidden pane.
- The selected tab in each pane and the active pane.
- Per-tab sort key and descending order.
- One/two-pane layout, split position, tree visibility and width, hidden-file
  visibility.

Split persistence uses the existing session format's three decimal places.

Pins, recent-folder history, window placement/maximization, navigation histories,
file selections/filters, thumbnail preferences, previews and helper sessions are
independent. Switching closes the file preview but does not stop Operations or
other retained panels. Explicit current folder preferences continue to apply to
later navigation; the workspace's saved sort wins for the initial restoration.

Saving waits for pending folder/address navigation to finish. Every tab must
refer to a real folder path; virtual place lists do not serialize as workspaces.
A directory that has become unavailable can still be retained by its exact path.

## Missing folders and WSL

The two selected tabs load asynchronously using the existing directory engine.
Inactive tabs hold only their saved path/options and are checked on first use.
An unavailable tab keeps its target address and shows an error in the pane; tab
strips mark failed tabs in red with `!`. The pane never shows an unrelated
previous folder's files beneath a restored tab. Refresh retries the same target
and clears the error after a successful load.

Exact UTF-16 path transport is reused from session persistence, including Linux
reserved-name mapping and raw-byte surrogate units. WSL-disabled restoration
retains the address with a visible error and starts no Linux directory load.
Enable WSL integration and Refresh to retry. Enabled WSL uses the ordinary
directory engine and existing installed helper; its first cold load may use an
unambiguous Windows-provider listing while the native helper starts. No subsystem is stopped or
restarted, and missing/offline endpoints are not replaced with a different folder.

## Storage and concurrency

The catalog lives at **`<session-file>.workspaces`**. For the default configuration
this is `%APPDATA%\OpenPilot\session.ini.workspaces`; settings placed elsewhere
and explicit `--session=FILE` use that session file's location instead.
`--session=` disables named workspace persistence and the manager reports that.
Named snapshots remain separate from the normal automatic exit session.

The first version permits **32 workspaces**, **80 UTF-16 name units**, and the
existing **32 tabs per pane**. Each serialized snapshot is bounded to 1 MiB and
the complete catalog to 16 MiB. Names are case-insensitively unique, cannot
contain control/unpaired-surrogate characters, and cannot start/end with spaces.
Names are labels, not filenames. Catalogs contain folder paths, not file contents
or credentials.

All catalog I/O runs on a worker. The versioned envelope includes lengths and an
accidental-corruption checksum; session bodies must parse and reproduce their
canonical form. Unknown, malformed or truncated catalogs fail visibly and are
retained. They are never treated as an empty store that a save can overwrite.

A per-catalog lock serializes cooperating app windows. A mutation checks the
revision loaded by the manager before changing anything; a stale manager must
Reload rather than overwrite another window's changes. Saves write a unique
owned temporary file, flush it, then atomically replace the prior catalog. Errors
preserve the prior saved catalog and loaded snapshots. The confirmation applies
only to deleting that saved entry, with current folders left open.

Closing/disabling the manager requests cancellation of pending I/O. An operation
already committed can finish; reopen or Reload to see the current catalog.
Cancellation and ordinary write failures remove only that worker's owned temp
file. A process/OS interruption may leave an uncommitted temp file beside the
catalog; it is not loaded as a saved workspace and is retained for review.
This follows normal Windows replace/flush behavior, without claiming a new
power-loss durability guarantee for directory metadata.

## Validation

`tests/workspaces.ps1` runs the storage and actual host/manager acceptance modules
serially against a private catalog, owned Windows folders and an isolated Linux
folder fixture. It does not use physical input, change the external clipboard,
touch the user's catalog or stop WSL/Docker. Coverage includes canonical/exact
path persistence, corrupt stores, revision conflicts, failed replacement,
explicit actions, active sorting/layout restoration, missing-folder recovery,
stale callbacks and the disabled/enabled WSL path.

Evidence and current delivery scope: `build/workspaces/20261022/RESULTS.md`.

Workspace replacement clears each pane's previous folder-watch identity before
starting restored-folder I/O. The same clear applies when activating a lazy
restored tab. Already queued or racing notifications from the retired folder
cannot refresh the replacement tab. Native watch teardown remains on the
background reaper; missing or disabled WSL targets install no replacement watch.
