# Linux in the main folder tree

The ordinary browsing tree shows a **Linux** root after the Windows drives when
WSL integration is enabled and supported distributions are registered for the
current Windows user. Linux starts expanded, with its installed distribution
roots visible immediately. Each distribution's own folders remain collapsed
until opened or revealed through navigation.

Distribution discovery reads registration metadata only. It does not open UNC
paths, enumerate Linux directories, launch wsl.exe or start a helper. Both WSL1
and WSL2 registrations are included; invalid/duplicate entries are omitted.
A local vector penguin marks Linux and local drive icons mark the distributions,
including when Windows icon mode is selected, without probing stopped distros.

Click a distribution to open its root in the active file pane. Expand it to load
its folders through the existing asynchronous directory provider. Normal tree
selection and ancestor reveal work within that filesystem; legacy wsl$ addresses
map to the same tree nodes and Linux directory names remain case sensitive.

The Linux heading groups the children. Clicking its label or pressing Enter or
Space folds/unfolds it without navigating. It supplies no filesystem target to
context commands, tabs, drag-out, drops or the places list. Distribution roots
retain root-operation protection. Existing folder actions work within them.

Disabling WSL integration removes the Linux branch and retires its pending tree
responses. Re-enabling reconstructs it expanded. An unchanged settings reload
keeps the user's existing collapse state. Installed distributions are refreshed
at startup and when WSL settings are configured. No user distribution registration
or virtual disk is changed.

Focused acceptance is `tests/wsl_tree.ps1`: an owned private registry catalog
exercises discovery and absence/disable cases, actual host windows exercise tree
commands and scope, and marker-guarded Linux folders exercise native navigation.
No physical input or WSL/Docker shutdown is required.
