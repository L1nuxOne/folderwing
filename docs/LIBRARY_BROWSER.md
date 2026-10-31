# Library browser v0

Folderwing can browse registered Calibre and Zotero catalogues and read their
local PDF attachments. The permanent **Libraries** zone occupies the bottom of
the folder tree, above the action dock when the dock is placed there. Drag its
upper divider to resize it; the zone scrolls independently of the folder tree.
Selecting a library replaces the file panes. Selecting a drive/folder, or the
**Files** button, returns to the existing file panes without replacing their tabs.

## Combined libraries

Tick the checkboxes beside library roots to include them in one catalogue. The
combined view can mix Calibre and Zotero. **Library** is displayed beside Title
and Size, identifies each row's parent library, and can be clicked to sort. Rows
retain their original attachments and covers; duplicate books in different
libraries remain separate rows. Hover a row to see its full source path.

Click a library's name to open that library alone. Uncheck libraries to narrow
the joint view; unchecking the last library returns to Files. Arrow keys move
focus in the library zone without changing membership, Space toggles the focused
checkbox, and Enter opens that library alone. Checked membership is saved in
`libraries.ini` and remains checked while viewing Files. Startup still restores
the normal file-browser session rather than automatically opening libraries.

Search, format filtering and tags apply across the combined list. Calibre series
can span source libraries; Zotero collection paths include their parent library
and keep separate collection identities. Combined views have their own saved
filters, selection, attachment choices and pin preference, keyed by the selected
set rather than checkbox order. The combined reader split is stored separately
from each library's single-view split. Changing the selected set closes the old
reader and opens that set's saved catalogue view; choose Read PDF to read again.

If a source cannot be opened, available sources remain browseable with an
**Incomplete view** banner, named failures in its tooltip and an incomplete count
in the footer. Refresh retries all selected sources. If every source fails,
the view shows an opening error. Empty catalogues contribute zero rows without
being reported as failures. Refresh can add a recovered source without mixing
up selected rows or remembered attachments. Item/attachment identities include
the source catalogue, so overlapping database IDs cannot select another library's
file. All sources remain read-only.

The combined row limit is 100,000, with at most 64 distinct source roots. This
feature does not scan library folders or build another book index.

## Reading

The library helper presents a virtualized catalogue and an optional details pane.
Selecting a book/reference displays metadata, formats or attachments and a cover
when available. **Read PDF** or double-clicking the attachment opens the existing
PDF reader as a third pane. Enter or double-click on a catalogue row also reads
the selected PDF. It retains the reader's navigation rail, personal
bookmarks and position memory. Once opened, the reader follows the selected
available PDF after a brief selection pause. **Pin reader** holds the current PDF
while you browse; unpinning follows the current selection. A reading bar names
the book/reference and actual filename currently in the reader. Selecting an
item without an available PDF keeps the current document and says so in that
bar. **Close reader** leaves the catalogue open, and later selections do not
reopen it until you choose **Read PDF** again. Queued automatic follow requests
cannot reopen a reader that has just been closed.

Clicking the active library root focuses its catalogue without reloading it or
closing its reader. Returning from Files to the same live catalogue resumes the
existing view; **Refresh** explicitly reloads it.

## Browsing

- Search matches title, creators, dates, publication, series and tags. Ctrl+F
  focuses the search field. Searches are literal, case-insensitive metadata
  searches; attachment contents are not scanned.
- Title and selected-format size stay beside each other as the view narrows.
  Author, year, format and added-date columns appear as room allows; full titles
  and creators remain available in row tooltips and Details.
- Format filtering includes books/references with that format. Click **Size**
  to rank largest first. In a PDF-filtered catalogue this is the PDF size, even
  when the same book has a larger EPUB. With **All formats**, Size represents
  the largest available format/attachment. Unknown sizes remain at the bottom.
- Calibre supplies recorded per-format sizes directly. Zotero local-attachment
  sizes are queried on demand when size sorting is requested and cached during
  the current catalogue session. Ranking is labelled incomplete while they load.
  **Refresh** discards those size samples and reloads the catalogue. No folder
  traversal or whole-drive index is needed to open either catalogue. Selected
  attachments are also probed for availability and current size. Catalogue rows,
  Details and the compact chooser use the same resolved size samples.
- **All groups** opens a searchable picker for tags, Calibre series and Zotero
  collection paths. Type any part of a name, then narrow to Tags, Series or
  Collections. It displays up to 200 matching groups and shows the full match
  count; refine the search to find others. Typing and category changes do not
  filter the catalogue until **Use group**, Enter or a double-click applies a
  result. **Clear group** removes only the grouping filter; **Clear filters**
  resets metadata search, format and grouping together.
- Selecting a Zotero parent collection includes its descendants. Collection
  choices are saved by ID, so identical display names stay distinct. Old saved
  label selections are restored when they identify one unambiguous group.
- Zotero catalogue rows represent references and standalone attachments; child
  attachments appear in Details. Notes, annotations and deleted items are omitted.
- Stored Zotero files resolve through their attachment key and `storage:` path.
  Absolute linked local files are supported. Relative linked files whose base
  directory is unavailable, URL-only entries, and missing files retain explicit
  availability labels. This version does not download or relink attachments.
- The attachment chooser and **Read PDF** stay below the catalogue even when
  Details is collapsed. Attachments show their actual filenames, availability
  and sizes. Manual choices are remembered per item (the most recent 256 choices
  per library). An available PDF is preferred over a missing default attachment;
  available remembered choices take precedence within the active format.
- **Reveal containing folder** in the attachment's **…** menu returns to the attachment's folder in the file browser. **Copy path**
  copies the selected attachment path. File deletion, renaming and catalogue
  editing are not exposed by this view.
- PDF is the embedded reading format for v0. Other formats remain listed with
  their metadata; additional readers and opening actions can follow later.

Search, format/group filters, sort direction, selected item, scroll position,
columns and details visibility/width are saved per library. Calibre state is
also keyed by its catalogue UUID. The outer PDF split is saved per registered
root (and separately for combined views). Narrow library areas automatically collapse Details without changing the
saved visibility preference, and Details returns when there is room. The
**Hide details / Show details** button changes that preference explicitly. Column
preferences survive temporary responsive hiding. Reader pin preference is saved
per catalogue; documents are opened explicitly on the next app launch.
Loading, cancellation, opening errors, an empty source catalogue and a filtered
list with no matches have separate messages in the catalogue area. Loading can
be cancelled; Retry or F5 starts a fresh read. Empty sources offer Refresh;
no-match results offer Clear filters. Details automatically yields its space
while no rows are available, without changing the saved visibility preference.
An existing reader remains available while catalogue data refreshes.

The catalogue, attachment headers, selector contents, group search and menus
share the dark palette and use system colors in high-contrast mode. Native
scrollbars retain Windows styling. Provider names are not repeated when the
registered library name already matches, and metadata omits empty labels.

Keyboard controls while the catalogue helper has focus:

| Key | Action |
| --- | --- |
| Ctrl+F | Focus and select the metadata search |
| Ctrl+G | Open grouping search |
| Ctrl+Shift+F | Clear all catalogue filters |
| Enter in metadata search | Apply search and focus results |
| Enter / double-click on a result | Read the selected available PDF |
| F6 / Shift+F6 | Move between catalogue and attachment chooser |
| Alt+P | Pin or unpin an open reader |
| F5 | Refresh or retry opening |
| Escape | Dismiss an open picker/dropdown first; clear a focused search next; otherwise close an open reader |

Within grouping search, Down moves from the query to results, Up on the first
result returns to the query, Enter applies a result and Escape cancels the draft.
Tab also reaches the visible buttons and metadata/attachment controls.

## Registration and state

`libraries.ini` lives beside Folderwing's session file. Without a session-file
location it defaults to `%LOCALAPPDATA%\Folderwing`. The regular installed
configuration therefore uses `%APPDATA%\Folderwing\libraries.ini`.
`library-views.json` stores view preferences in the same directory. Neither file
is written into the source library. Library registrations remain local and are not compiled into the application or
bundled into a public release.

The v0 registration format is:

```ini
[Libraries]
Count=1
Height=220

[Library1]
Name=Example library
Type=calibre
Path=C:\Books\Example
ReaderPercent=60
Checked=0
```

Types are `calibre` and `zotero`. There can be up to 64 roots. Registration changes
are read at app startup. A guided Add Library/import surface and custom mappings
for other catalogue schemas are deferred. `FOLDERWING_LIBRARY_CONFIG` selects a
different registry for owned tests; normal users do not need that override.

An empty Zotero database is shown as an empty catalogue; the browser does not
reconstruct records from loose storage files or choose a backup database.

## Process and data ownership

The explorer owns the library roots, navigation and PDF reader. One on-demand
`folderwing-library.exe` process owns the catalogue UI and database work. It
reuses the pinned `folderwing-sqlite.dll`; no Calibre or Zotero runtime is loaded.
Queries use a read-only connection and a short read transaction, with normal
SQLite locking/WAL handling, cancellation and resource limits. Adapter queries
use the base tables rather than application-specific SQL functions or views.
Schema columns are checked and unsupported schemas produce an error.

Catalogue changes are detected from the main database and WAL timestamps while
the view is visible. The status asks for Refresh; it does not move rows underneath
a reader. File availability is checked when an item is selected and again before
reading. Recorded Calibre sizes for unprobed attachments can be stale after changes
made outside Calibre.
Reading state and view preferences belong to Folderwing; catalogue data belongs
to Calibre/Zotero and is never edited here.

Host/helper messages are bound to the current root session and owned helper
process. Switching roots clears the old selection and attachment actions before
loading the new catalogue. Background file-pane refreshes cannot replace a
library PDF. Hidden file selections are not exposed as operation targets while
library mode is active. Stored relative paths are confined to their library;
explicit absolute Zotero links may point to another local Windows drive.

The v0 limits are 100,000 catalogue rows, 1,000,000 rows per relation/query,
128 MiB of decoded field input and 32,768 UTF-16 units per field. Covers load on
selection in the helper, with an 8 MiB encoded-file and 20-megapixel limit. A
limit or unreadable database produces an explicit error rather than an
apparently complete truncated library. The adapters were also exercised against existing catalogues during development.

## Focused verification

```powershell
./build.ps1 -OutputDirectory ./build/preview-navigation -Targets folderwing.exe,folderwing-library.exe
./tests/library_catalog_tests.ps1 -BuildDirectory ./build/preview-navigation
./tests/library_ui_tests.ps1 -BuildDirectory ./build/preview-navigation
./tests/library_host_tests.ps1 -BuildDirectory ./build/preview-navigation
```

The catalogue/UI fixtures are synthetic. The native-host check uses the existing
owned `build/pdf-navigation-fixtures/plain.pdf` fixture and the real PDF helper.
All windows are owned and off-screen; no physical desktop input or WSL shutdown
is required. `-InspectLibrary 'calibre|C:\Books\Example'` on the catalogue test is an
optional, explicit read-only check of a local root and is never used by CI.
