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
`build.ps1`, without modifying that helper or its wire protocol. This prototype
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
Ctrl+H hidden files, F6 other pane, Ctrl+Q close. Enter/double-click opens folders.
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

Launching files, previews, tabs, terminal and desktop/mount integration remain
future steps. Mutation of mounted Windows/9P endpoints is refused: those need
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
G_DEBUG=fatal-criticals GTK_A11Y=none GSK_RENDERER=cairo GDK_BACKEND=x11 \
  dbus-run-session -- xvfb-run -a \
  build/linux-prototype/folderwing-linux-browser-tests \
  "$PWD/build/linux-prototype/folderwing-linux-helper" \
  "$PWD/build/linux-prototype/folderwing.svg"
```

Requires Xvfb and dbus-run-session for the isolated lane. Set
`FW_LINUX_SCREENSHOT` to an absolute PNG path to capture the real rendered test
window. `GDK_BACKEND=wayland` can run the same desktop acceptance against an
available Wayland display; it opens only owned test windows and needs no user
input. GTK environment configuration remains local to the launched process.

Thirteen selectors cover byte-exact paths and metadata, independent/same-folder
watches, failed/rapid navigation, malformed/truncated/disconnected peers,
close-during-request cleanup, real two-pane widgets/history/sorting/filtering,
a 10,000-file load, multi-selection review/cancel/refresh, copy conflict policies,
same/cross-filesystem moves, trash recovery records, injected persistence errors,
cancellation before publication and close-during-job draining. The test creates its own temporary root and removes
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
