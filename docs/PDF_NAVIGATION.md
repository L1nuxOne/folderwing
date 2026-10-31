# PDF navigation rail and personal bookmarks

The PDF reader reserves a slim rail on its right edge. PDF outline/bookmark
entries become ticks at their actual destinations. Major sections have longer
ticks than subsections. Ticks extend sideways around the pointer without moving
their vertical positions. Hover shows the section title and page number; click
to jump, or drag along the rail to scrub through the document. The blue marker
shows the current reading position.

A PDF without usable internal outline destinations gets 0%, 5%, …, 100% ticks
instead, measured over the scrollable range. Only local page destinations are
used; URL, remote-file and other external bookmark actions are not followed.
Right-click the rail to choose between entries that share a position. Outline
extraction runs inside the existing restricted PDF helper, after opening the
PDF. It has limits of 2,048 destinations, 4,096 visited nodes, 32 levels and
159 UTF-16 code units per title. Unsupported destinations are omitted. A very
large or malformed outline may be incomplete; the rail still supports dragging.

Click the ribbon bookmark icon at the right of the PDF header, or press Ctrl+D,
to toggle a personal mark at the top of the viewport. Its title comes from the
containing outline section when available, otherwise its page number. Personal
marks are gold; the ribbon fills when the current position is bookmarked.
Right-click the icon, or press Ctrl+Shift+D, for the personal-bookmark list. Jump
to a saved position and toggle the icon again to remove it. Custom titles and
renaming marks are deferred.

Personal marks are saved immediately in `pdf-bookmarks.fwb` beside the reader's
position-memory file. If session storage is disabled, the location is
`%LOCALAPPDATA%\Folderwing\pdf-bookmarks.fwb`. These marks are independent of the
recent-PDF limit and persist even when automatic position remembering is off.
Archive PDF previews use a stable archive/member identity instead of temporary
extraction paths. Member names remain case-sensitive. Ordinary marks identify
documents by path; renaming/moving a PDF or archive does not migrate its marks.
Marks keep page/point positions rather than text anchors, so editing a document
can change the content at a saved position. Marks beyond a changed document's
page count are retained in storage but omitted from its current rail and menu.

The versioned local store has a checksum and bounds of 512 marks per document,
4,096 total marks and 8 MiB. Reaching a limit reports an error rather than
removing older marks. Each mutation takes an exclusive store lock, reloads the
latest data and replaces the file atomically after flushing. This preserves
other windows' changes. Busy, unreadable and corrupt stores are reported; no
empty replacement is written over unreadable existing data. Hover over a red
bookmark icon for the error. Separate `.lock` files are expected.

PDF previews include PDFs inside archives. Other reading previews now use the
same interaction; see [Reading rails across previews](PREVIEW_NAVIGATION.md) for
their semantic anchors, row limits and shared bookmark storage.

Focused acceptance, without physical desktop input or stopping WSL:

```sh
python3 tests/fixtures/native/pdf_navigation_fixtures.py --output build/pdf-navigation-fixtures
```

```powershell
./build.ps1 -OutputDirectory ./build/pdf-navigation -Targets folderwing.exe,folderwing-pdf.exe,pdf_navigation_tests.exe
./tests/pdf_navigation.ps1 -BuildDirectory ./build/pdf-navigation
```
