# Folderwing visual identity

Folderwing is the product name for the Windows file manager previously called
OpenPilot. The folder-and-wing emblem represents navigation and moving between
files, panes and tools. Use **Folderwing**, with a capital F and lowercase wing.
The descriptor is **File manager for Windows and WSL**.

The first identity pass includes:

- A vector app-icon master and PNG export, with native Windows ICO sizes from
  16 through 256 pixels. Cream framing keeps the coral/plum mark visible on
  light and dark desktops.
- Embedded executable icon/version information, caption and Alt-Tab icons that
  follow DPI, and the explicit `Folderwing.FileManager` taskbar identity.
- Folderwing window titles, context-menu/function descriptions, generated file
  headers and an **About Folderwing** command in the command palette.
- A Folderwing shortcut with the same icon and taskbar identity, preserving an
  existing matching OpenPilot shortcut's arguments and working directory.
- The horizontal wordmark and a small, reusable colour palette.

| Colour | Hex | Use |
|---|---|---|
| Coral | `#FF6655` | Folder surface and wing fold |
| Plum | `#30213C` | Wing and light-background wordmark |
| Cream | `#FFF7EF` | App-icon tile |

Assets are in [branding](branding): `folderwing-icon.svg` is the editable,
reproducible icon master; `folderwing-icon.png` and `folderwing.ico` are exports.
`folderwing-wordmark.png` is the generated transparent wordmark on a light ground.
The selected concept and generation prompts are retained alongside the assets.
The UI's file-type colours retain their existing semantic meanings.

Re-export the icon with `tools/build-branding.py` using CairoSVG and Pillow.
Ordinary Windows builds consume the checked-in ICO and compile
`app/folderwing.rc` with Zig RC; those Python packages are only asset-authoring
requirements. No image runtime, network call or logo file is needed at app launch.

The shipped app is `folderwing.exe`, the CLI is `folderwing-cli.exe`, the
engine DLL is `folderwing-engine.dll`, Windows helpers use `folderwing-<role>.exe`
and the Linux helper is `folderwing-wsl`. Source filenames, exported engine APIs,
window classes and helper protocols retain their stable internal identities.
Third-party DLL names and copyright attribution retain their upstream names.

Default settings now use `folderwing.ini` beside the executable when portable,
otherwise `%APPDATA%\Folderwing\folderwing.ini`. An existing portable
`openpilot.ini` is copied without replacement; if that fails, it remains in use.
An AppData migration copies the known settings, session, named-workspace,
folder-view and PDF-position files, preserving originals and existing destination
files. Settings are copied last and a completion marker prevents reimporting
removed session data. A failed migration retains the legacy base path and retries
on the next launch. An existing new settings file establishes the current data
set as authoritative. Explicit `--settings=FILE` and `--session=FILE`, including
empty opt-outs, remain supported.

Older already-running windows continue using their original helper cohort. The
local update retains that cohort for them; new builds and shortcuts use the new
filenames. Local NTFS/search cache locations, format markers and historical audit
evidence remain compatible; this migration does not move or rebuild caches.

`tools/folderwing-shortcut.ps1` creates a shortcut in a specified folder (Desktop
by default) after verifying its exact target and backing up matching originals.
It refuses an existing Folderwing shortcut targeting another app/build. It does
not pin items or reset Explorer's icon cache.
