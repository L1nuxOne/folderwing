# Action dock

The action dock exposes the existing file-manager tools through native buttons.
The default position is **Tree bottom**: two compact rows beneath the folder tree.
Right-click the dock, or click **More**, to switch between:

- **Left edge**: a vertical strip outside the tree and file panes.
- **Tree bottom**: at most two rows, using only the tree's width.
- **Top**: one horizontal row above the tree, file panes and preview surfaces.
- **Hidden**: releases the dock controls and returns their space to the layout.

When the tree is hidden, Tree bottom temporarily uses the left strip. Showing
the tree restores the selected position. A short strip, narrow tree or labelled
top row uses More for tools that do not fit. More always remains available, even
when all optional tool buttons have been hidden.

The first set includes Find files, Search contents, Compare, Size, Duplicates,
Git, Workspaces, Terminal, Operations and Direct agent. Compare offers file/folder comparisons;
Size offers the space analyzer and folder-size calculation. The context menu
also toggles labels and chooses which buttons appear. Hidden buttons remain
available through the existing command palette/menus.

Use **Action dock options** in the command palette or Settings to restore a
hidden dock. Its function and options are:

```ini
[functions]
view.action_dock = on

[view.action_dock]
placement = tree-bottom
labels = off
items = find content compare size duplicates git workspaces terminal operations direct
```

Right-click/menu changes are saved through the existing asynchronous settings
writer, preserving other settings and checking concurrent edits. With
`--settings=` they apply only to the current session. Placement is an application
setting; switching a saved workspace does not overwrite it.

## Interaction and scope

Hover for the action name, shortcut and availability hint. Icons keep descriptive
native button names when labels are hidden. Unavailable/disabled commands are
shown disabled; buttons for the currently visible helper panel are highlighted.
The dock runs the registered commands rather than maintaining alternative tool
implementations. It starts no helper merely by being displayed, moved or drawn.

Clicks retain the active file pane and its selection. The target is captured
before button focus moves or a group/overflow menu opens, then rechecked before
execution. A changed pane, selection, listing or command revision cancels the
invocation rather than silently retargeting it. Compare additionally checks the
other pane. Archive-preview context retains the existing archive command rules.
For Size and Duplicates, a file cursor with no explicit selection targets the
current folder; selected folders retain their ordinary command behavior.
Duplicates accepts up to 16 selected folders. Query and captured invocation
count each folder once; mixed file/folder selections and excess roots remain
unavailable. Button and overflow routes submit the same exact selected roots.

Tab reaches the dock as part of the normal pane/tree/preview focus cycle. Arrow
keys move among visible enabled buttons; Enter or Space activates; Tab/Shift+Tab
return to the host focus cycle; Escape returns to the active file pane. The Menu
key or Shift+F10 opens dock options. Existing registered shortcuts also work
while a dock button has focus.

Icons are native DPI-scaled vector strokes, with no image pack or new helper
process. Existing splitters, side panels, previews and archive third panes use
the resulting content rectangle. Tooltips use the Windows common-controls DLL,
loaded on first use and cached until process exit so a menu can safely destroy
its invoking button while a native subclass callback is returning.

## Focused validation

```powershell
./tests/action_dock.ps1 -BuildDirectory ./build/release
```

The focused native control and real-host executables exercise placement/DPI
bounds, two-row/overflow rules, hidden-tree fallback, native names/tooltips,
keyboard routing, disabled actions, splitter offsets, stale-menu context,
helper launches, saved options and feature-disable cleanup. Host menus use owned
selection oracles and windows/messages; no physical input or user windows are
used. Offscreen screenshots retain the initial placement variants. No full
application suite or performance audit is claimed, and WSL/Docker remain running.

The Direct agent button opens the optional native `direct-client.exe` beside
OpenPilot. Its runtime has an independent lifetime; opening/closing the panel
does not submit a turn or stop runtime work. The button follows `direct.show`
and panel enablement, and is highlighted while its panel is visible. Disabling
Direct retires its presentation helper. See [Direct setup](DIRECT_AGENT.md).
