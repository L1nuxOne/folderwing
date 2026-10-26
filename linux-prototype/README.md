# Folderwing native Linux browser prototype

A GTK4/C frontend for the existing native Linux listing, inotify
watching and native filesystem-job helpers. Runs directly on Linux; WSL is not a runtime requirement. The
first desktop checks use WSLg and an isolated X11 display without stopping WSL.
This is an experimental frontend, not a Linux v0.1.0 release or a replacement for
the Windows app.

## Build and launch

Requires a C compiler, pkg-config, GTK4 >= 4.8 development files, and SQLite3
headers/library. On Debian/Ubuntu:

```sh
sudo apt install build-essential pkg-config libgtk-4-dev libsqlite3-dev
./linux-prototype/build.sh
GSK_RENDERER=cairo ./build/linux-prototype/folderwing-linux --left "$HOME" --right /
```

The app finds `folderwing-linux-helper` and `folderwing.svg` beside itself.
`--helper /absolute/path` overrides the helper explicitly. Both folder arguments
are absolute native paths, passed as argv rather than shell commands. Build
outputs remain in ignored `build/linux-prototype/`.
The CPU renderer above matches the checked profile and avoids software-GPU
fallback overhead on the current WSLg host; it is a launch option, not a global
GTK setting.

The Linux helper is rebuilt from the same full source list as the WSL target in
`build.ps1`, using the existing wire protocol. This prototype
uses the distribution's SQLite/GTK shared libraries; no public binary package
or cross-distribution dependency/license closure is claimed yet.

## First useful scope

- Two independent panes with a draggable divider, virtualized Name/Size/Modified
  columns, folder-first natural-name sorting and clickable sort headers.
- Per-pane path input, Home/Up, Back/Forward history, and Refresh.
- Hidden-file toggle; metadata and literal symlink targets in row tooltips.
- Live create/rename/delete/content-size updates from the existing helper.
- Failed or interrupted listings preserve the previous snapshot and show an
  error. A previous successful snapshot is not relabeled as a new location.
- Navigation cancels and replaces only that pane's owned helper. Watch-before-
  list ordering and debounced refresh avoid an unobserved scan/watch handoff.
- Selection is restored by exact filename after a same-folder refresh.

Keyboard: Ctrl+L address, Alt+Left/Right history, Alt+Up parent, F5 refresh,
Ctrl+H hidden files, F6 other pane, Ctrl+Q close. Enter/double-click opens folders
and opens regular files with the default Linux application.
Ctrl/Shift selection supports multiple files and folders. Copy/Move target the
other pane: use the buttons, Shift+F5 for Copy, or Shift+F6 for Move. Delete opens
a Trash review only while a file list has focus; it keeps normal text editing
behavior in the address bar. Every action requires an explicit review/Start,
with exact sources/destination captured before launch. Keep-both is the default;
skip/merge, replace/merge and fail-on-conflict are explicit alternatives.

Operations run in a dedicated `--jobs` helper; navigation and live listings stay
independent. The operation bar shows progress, cancellation and bounded event
details (16 KiB plus complete error/skip counts). Closing during a job requests
cancellation and waits for the helper result. Completed items are kept after
cancellation; operations are not whole-batch rollback transactions. A disconnect
or invalid result after mutation starts is **completion unconfirmed**, never
success. There is no permanent-delete action or fallback.

Previews and desktop/mount integration remain future steps. Mutation of mounted Windows/9P endpoints is refused: those need
the Windows application's stream inventory and durability acknowledgements.

The address bar uses a reversible display: `\\` represents a literal backslash,
`\xFF` an undecodable byte, and `\x0A` a newline. Ordinary Unicode remains
readable. Row activation always uses original filename bytes. Relative input
is joined to the pane's current path; `~`/`~/...` uses the current user's home.
Paths retain logical components, including symlinks and `..`; the Linux kernel
resolves them. Up follows the displayed logical path, not a physical realpath.

## Boundaries and responsiveness

The GTK frontend never enumerates a folder itself. Each visible pane owns one
same-user subprocess with asynchronous GIO pipe reads/writes. Superseded replies
cannot update a new connection. Helpers do no periodic work while idle; a
100 ms refresh debounce is armed only after changes. There is no root elevation,
WSL control or call to a shell in the adapter.

Snapshots are capped at 200,000 entries, wire frames at the existing 1 MiB bound,
and paths at 16 KiB. A request has a 15-second failure timeout. Partial listings
are discarded. GTK filtering/sorting is incremental and rows stay disabled
while a listing or sort is incomplete. The displayed listing milliseconds cover
helper/protocol/model work, not total rendering time or a performance promise.
Network filesystems, mount loss and full power-failure recovery are outside this
prototype's acceptance claims.

## Focused acceptance

```sh
./linux-prototype/build.sh --tests
G_DEBUG=fatal-criticals FW_CLIPBOARD_TEST_ISOLATED=1 GTK_A11Y=none GSK_RENDERER=cairo GDK_BACKEND=x11 \
  dbus-run-session -- xvfb-run -a \
  build/linux-prototype/folderwing-linux-browser-tests \
  "$PWD/build/linux-prototype/folderwing-linux-helper" \
  "$PWD/build/linux-prototype/folderwing.svg"
```

Requires Xvfb and dbus-run-session for both checked profiles. Clipboard tests
use two explicit GDK connections to that private X11 server, so real clipboard
transport is exercised without reading or replacing the user's clipboard.
The test refuses to run these cases without `FW_CLIPBOARD_TEST_ISOLATED=1` and
an explicit X11 display other than WSLg's `:0`.

Set `FW_LINUX_SCREENSHOT` to an absolute PNG path to capture the rendered test
window. For Wayland widgets use the receipt runner below: it sets
`GDK_BACKEND=wayland,x11`, asserts that the widget display is Wayland, and uses
its owned Xvfb only for the clipboard. Wayland clipboard transport itself is
not covered by that profile. No user input automation or distro shutdown is
needed; GTK environment configuration stays local to the launched process.

Thirty-four selectors cover byte-exact paths and metadata, independent/same-folder
watches, failed/rapid navigation, malformed/truncated/disconnected peers,
close-during-request cleanup, real two-pane widgets/history/sorting/filtering,
a 10,000-file load, multi-selection review/cancel/refresh, copy conflict policies,
same/cross-filesystem moves, trash recovery records, injected persistence errors,
cancellation before publication and close-during-job draining. Desktop checks
exercise default/chosen application launches, file types and symlink/byte-exact
paths, context-menu hit testing/selection/actions, clipboard content generation,
and asynchronous close/cancel. Application registration uses owned XDG fixtures. Clipboard checks cover
GNOME/Nautilus and KDE offers, bounded URI decoding/streaming, real native
Copy/Move through Paste review, cut styling/refresh, cancellation, clipboard
replacement, partial/failed completion and text-field shortcut guards. Tests
neither launch user applications nor overwrite the user clipboard. Tab/session
checks cover independent history/selection, reordering and bounds, unavailable
folders, pending Paste/transfer/rename state, byte-exact session round trips,
writer ownership, corrupt-file preservation and asynchronous close/save failure.
All session fixtures are owned temporary paths; tests leave the user session alone.
The test creates its own temporary root and removes
only those fixtures, using lstat so fixture symlinks are never followed.
Selectors and helper/source dependencies are in `tests/native-impact.json`.

For a fresh build plus the standing audit, source-impact selection and a
content-addressed acceptance receipt, run:

```sh
python3 linux-prototype/check.py --backend x11
python3 linux-prototype/check.py --backend wayland
```

These are separate, serialized resource profiles. Evidence goes to
`build/linux-prototype/check-<backend>/`. The standing repository audit also
expects its pinned libvterm source inventory in `third_party/libvterm`, as in the
Windows source-check environment; the prototype itself does not use libvterm.
Windows behavior is not covered by a Linux receipt. No full Windows,
transfer-durability, media, VM or performance suite is substituted by this lane.

## Durability scope

The frontend sends only native path records and the normal running/continue
handshake. Filesystem mutation, metadata preservation, staging, source checks,
ancestry fences and flushes remain in the existing helper. Final success requires
a valid terminal result, no errors, and a clean helper exit. Skips are reported.
The progress percentage is not a commit acknowledgement.

Copied data and cross-filesystem moves retain the existing checked file and
namespace barriers before source removal. Same-filesystem moves and trash use
namespace durability for renames; they do not recursively flush pre-existing
file contents. Trash now persists the files/info directories and their ancestry,
the recovery record file and its directory before moving the source name, then
the trash destination and source-parent namespaces. A late flush failure retains
the trash object and recovery record and reports an error; the original name
may already be gone. No power-loss/VM interruption run is claimed for this update.


## Rename and New Folder

- F2 or the pane's Rename icon edits one selected item in place using a small
  pane-attached name editor. Multiple selection does not start a rename.
- Ctrl+Shift+N or the New Folder icon opens the same editor with an available
  default name. The folder is created only when you press Enter/Create.
- Enter applies; Escape/Cancel before applying changes nothing. A conflict
  keeps the editor open for correction and never replaces an existing node.
- Name text uses the path bar's reversible byte escaping. Slash, NUL, empty
  names, `.`/`..` and names over 255 bytes are refused. Tilde, spaces, dollar
  signs and other Linux filename bytes remain literal.
- The editor captures its pane's original folder/source. Navigation discards
  an unapplied edit. A successful operation refreshes and selects the result;
  newly hidden names follow the current hidden-file setting.

Rename reuses the helper's `--rename` preview/apply protocol, including fresh
source identity and no-replacement checks. New Folder uses `--jobs` mkdir.
Their helper paths now fence ancestry and directory namespace changes before
reporting success. A late rename barrier error is completion-unconfirmed; a
created folder is retained if its final barrier fails. These are namespace
barriers, not recursive payload flushing or a new power-loss test.


## File opening and context menus

Enter/double-click on a regular file asks its default **Linux** application to
open it. Folder activation stays inside the pane. File metadata and application
discovery run off the GTK thread; launches are asynchronous. Symlinks follow their
target, including the kernel semantics of logical `symlink/../name` paths. Missing
files, broken links, special nodes, missing associations and launch failures show
an error in a separate opening-status line. An accepted launch is reported as an
open request, not proof that another application's document has rendered.

Right-click any cell in a row for Open, Open With, Copy/Move to Other Pane,
Rename, Move to Trash and Copy Path. Existing multi-selection is preserved;
right-clicking another item selects that item alone. Open and Rename require one
item. Copy/Move/Trash use the existing review dialogs and helpers. Shift+F10 or
the Menu key opens the selection's menu while the file list has focus.

Right-click empty space for New Folder, Copy Path, Open Terminal Here and Refresh.
A selected folder also offers Open Terminal Here for that folder. Menus close
when selection, location or listing changes, so an old menu cannot redirect an
action onto a new snapshot. Dismissing a menu never starts an operation.

Open With lists installed visible Linux applications, with compatible apps first
and a search field. It applies only to that launch and never changes the default
association. The chooser supports Cancel/Escape; closing Folderwing cancels
pending discovery and closes its chooser. No filename becomes shell code.

Copy Path puts the reversible displayed path(s) in text and byte-preserving
`file:` URIs in `text/uri-list`. This retains raw Linux filenames even where a
plain-text consumer cannot represent them. The background action copies the
pane's current folder.

Open Terminal Here asks GIO to find a terminal emulator and runs the existing
helper's `--shell` entry point, using a hex-encoded native path as argv. It enters
the folder and starts the user's Bash configuration. A Linux terminal emulator
must be installed; a missing launcher is reported without changing settings.
This uses the [GIO application launch APIs](https://docs.gtk.org/gio/method.AppInfo.launch_uris_async.html)
and [terminal launch flag](https://docs.gtk.org/gio/flags.AppInfoCreateFlags.html).


## Clipboard Copy, Cut and Paste

With a file list focused, **Ctrl+C** copies the selection, **Ctrl+X** cuts it,
and **Ctrl+V** pastes into that pane's current folder. The row menu has Copy
and Cut; both row and background menus offer **Paste into This Pane…**.
Copy/Move to Other Pane remain available separately. Shortcuts in the address,
rename and new-folder fields keep their normal text-editing behavior.

Copy/Cut put file references on the clipboard and do not modify source files.
Cut dims matching rows in both panes and survives live refresh. Paste reads the
clipboard asynchronously, captures the destination, then opens the existing
Copy/Move review with Keep Both selected. Cancel before Start changes no files
and keeps the clipboard. Navigation or a busy destination prevents a delayed
read from opening a review for another folder. Clipboard replacement invalidates
an existing Paste review before Start.

A cut clipboard is cleared only after a fully confirmed move with zero errors
and zero skips, provided the clipboard offer still matches the reviewed one.
Cancelled, failed, skipped or unconfirmed moves retain it for review; completed
items are not rolled back. A newer clipboard is preserved when an older move
finishes. Replacing the clipboard also removes Folderwing's old cut marks.

The provider offers `text/uri-list`, `x-special/gnome-copied-files`,
`x-special/nautilus-clipboard`, and `application/x-kde-cutselection`, plus text.
It accepts local `file:` URIs, including `localhost`, preserves raw filename
bytes through percent encoding, and refuses remote schemes/hosts, conflicting
cut markers, malformed lists, duplicate sources and oversized input. Plain text
alone never starts file operations. Limits match the coordinator: 4096 sources,
16 KiB per path and a 16 MiB clipboard read. A stalled read is cancelled after
15 seconds. Mounted Windows/9P mutation restrictions still apply.

The exchange formats follow the upstream
[GNOME file clipboard format](https://github.com/GNOME/nautilus/blob/master/src/nautilus-clipboard.c)
and [KDE cut marker](https://github.com/KDE/kio/blob/master/src/widgets/paste.cpp).
Acceptance uses protocol fixtures and two GDK connections; it does not claim
an interactive Nautilus/Dolphin or Wayland-transport interoperability run.


## Tabs and session restoration

Each pane has its own tab strip, a **+** button and close buttons. **Ctrl+T**
duplicates the active folder into a new tab; **Ctrl+W** closes the current tab
while the file list has focus. **Ctrl+PageUp/PageDown** and **Ctrl+Tab** /
**Ctrl+Shift+Tab** cycle within the active pane. Ctrl+W retains its text-editing
behavior in the address/name fields. Drag tab headers to reorder them within a
pane. A folder's context menu offers **Open in New Tab**, and the background
menu offers **New Tab**. Each pane keeps at least one tab, up to 32.

Navigation history and up to 4096 selected names are retained separately for
each tab during the run. Sorting remains per pane. Switching tabs starts a fresh
listing through the existing helper and closes that pane's previous listing
connection. Only the two visible tabs have listing/watch helpers; inactive tabs
keep lightweight folder/navigation records. An unavailable tab retains its path
and error, with no other tab's old snapshot presented as that location. F5 retries
it when a disk or mount returns.

Switching or closing a tab discards unapplied name edits and cancels a pending
clipboard read. Already-started jobs retain their captured source/destination
paths and continue in the operation bar. No running job, clipboard cut state or
unapplied dialog is saved or restarted as a session operation.

Normal startup restores tab paths/order, selected tabs, active pane, hidden-file
visibility, pane split and window size/maximized state. Folder history and file
selection start fresh after a process restart. The state file is
`$XDG_STATE_HOME/folderwing/linux-prototype/session.ini`, or
`~/.local/state/folderwing/linux-prototype/session.ini` when XDG_STATE_HOME is not
absolute. File names are base64-encoded native bytes, so non-UTF-8 names and
logical symlink paths survive unchanged.

Session I/O runs off the UI thread. Changes are coalesced for 500 ms and written
with atomic replacement and GLib's durable-write flags. Closing drains any older
save and writes the latest layout before exit. A final save failure keeps the
window open with an error; closing it again exits without saving. The first
window holds the writer lock for that session. Other windows can load it and
browse independently, with a notice that they will not overwrite the saved
layout. Process exit releases the lock automatically.

Malformed/unsupported regular session files are preserved under a unique
`.invalid-…` name before starting a new session. Unreadable files and non-regular
session paths are preserved with saving disabled. Limits are 32 tabs per pane,
16 KiB per path and a 2 MiB state file; decoding validates all layout fields.

Use `--left PATH` or `--right PATH` to replace that pane's restored tabs with the
specified folder. `--session-file /absolute/path/session.ini` gives a separate
persistent session. `--no-session` starts without loading or saving state; it
can be combined with `--left` and `--right` for disposable runs.
