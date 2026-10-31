# Reading rails across previews

The slim right-edge rail now accompanies the scrollable reading previews:

| Reader | Rail ticks | Personal position |
| --- | --- | --- |
| PDF | Internal outline, otherwise 5% steps | Page and vertical offset; see [PDF navigation](PDF_NAVIGATION.md) |
| Markdown | Headings | Heading and fraction within its section |
| Word | Document headings | Heading and fraction within its section |
| Jupyter notebooks | Loaded cells, labelled by their heading or cell type | Cell and fraction within it; selecting a saved later cell loads it on demand |
| JSON, JSONL, YAML, TOML, INI | Visible keys and sections | Structural path; jumping can reopen a collapsed branch |
| XML | Visible elements and attributes | XPath; jumping can reopen a collapsed branch |
| CSV, TSV, Excel | 5% of the **loaded rows** | Original source row and fraction within it; Excel marks are per sheet |
| Native text/code and Source | 5% of the loaded snapshot | Character offset; wrapping maps it back to a visible line |
| SQLite Rows | 5% of the current **loaded page** | Row offset within the table/filter/sort query; choosing a saved mark loads its page |
| SQLite Schema | 5% of the schema text | Schema line, separate from row marks |

Ticks extend toward the pointer, hover gives a label, clicking jumps and dragging
scrubs. The blue marker shows the reading position. With no headings, ordinary
reading previews use 0%, 5%, …, 100% steps over their scrollable range. A fitting,
unscrollable view has a single fallback tick. Semantic rails draw up to 2,048
visible sections; dragging remains available beyond the displayed ticks.

Click the ribbon icon or press **Ctrl+D** to toggle a personal bookmark.
**Ctrl+Shift+D**, or right-clicking the icon, opens the personal list. Gold ticks
represent saved marks. In browser previews the list also offers direct removal;
in native text jump to a mark and toggle it to remove it. Right-click the rail
for nearby overlapping sections (SQLite opens its saved-position list).

Structured records/documents, Excel sheets, rendered/source modes, and SQLite
queries each have separate bookmark contexts. Table marks use source row numbers,
so sorting loaded rows does not silently change the referenced CSV/Excel row.
A saved row outside the current loaded CSV/Excel window is kept, with a message
to load the row and clear filters. These rails do not imply a scan or row count
of the entire file. SQLite row offsets follow the selected query, not persistent
record IDs; modifying the database can change which record is at that offset.

All these marks except PDF use `preview-bookmarks.fwb` beside the session file,
or `%LOCALAPPDATA%\Folderwing\preview-bookmarks.fwb` as the default. PDF retains
its existing separate store. The shared format has a checksum, atomic flushed
replacement, an exclusive mutation lock, and limits of 512 marks per document
context, 4,096 total and 8 MiB. Busy or corrupt storage reports an error and keeps
existing data. Browser messages select only positions in the already-open
preview; the host fixes the storage path and document identity. Archive keys
use the archive path plus its case-sensitive member name, independent of
extraction paths. Renaming/moving a document does not migrate its marks.

Marks refer to positions in the current document, not a historical snapshot.
Editing content or deleting a section can change or invalidate a destination;
unavailable marks remain saved. Labels have a 159 UTF-16-unit limit. Images keep
their zoom/pan controls and audio/video keep their playback timeline; neither
receives a document-reading rail in this change.

The browser widget is shared offline JS/CSS, hosted by the existing Markdown,
Word and Excel helper processes. SQLite has a native WinForms rail and Source
uses the native reader. No additional helper process or third-party dependency
is introduced. Only content/scroll/resize/context changes schedule rail work;
there is no idle polling.

Focused checks, using synthetic fixtures and owned off-screen/headless windows:

```powershell
./build.ps1 -OutputDirectory ./build/preview-navigation -Targets folderwing.exe,folderwing-text.exe,preview_navigation_tests.exe,pdf_navigation_tests.exe
./tests/navigation_browser_tests.ps1
./tests/preview_navigation.ps1 -BuildDirectory ./build/preview-navigation
./tests/pdf_navigation.ps1 -BuildDirectory ./build/preview-navigation
```

These checks do not use physical desktop input or stop WSL. The first run on a
new build directory needs the ordinary runtime assets, including the PDF helper,
just as the app does.
