# Pane footer filters

Each file pane has a filter row above its existing item count, selection/status
and directory-read timing. Filters inspect the loaded listing only; they do not
search subfolders or read file contents.

The **All types** button opens a checkable menu: Images, Video, Audio, Documents,
Archives, Text/code, Data and Other. Selected categories combine as alternatives;
selecting All types clears category restrictions. Classification reuses the
built-in file-type icon registry, including its configured extension overrides.
Documents includes PDFs, word-processing files, spreadsheets and presentations.
Text/code includes plain text, Markdown, source code and scripts. Data includes
structured data and databases. Other covers the remaining categories.

**Keep folders visible** is checked by default. With it enabled, file categories
leave folders available for navigation; the text filter still applies to folder
names. Unchecking it hides folders entirely until it is checked again or the
filters are cleared. Hidden/system visibility remains controlled by the existing
application setting.

The text field updates as you type and matches case-insensitively:

| Input | Meaning |
| --- | --- |
| `invoice` | Filename contains invoice |
| `.pdf` | Filename ends in .pdf |
| `*.pdf` | Whole-name wildcard matching .pdf files |
| `report*.csv` | Starts with report and ends in .csv |
| `*.jpg;*.png` | Either wildcard pattern |

`*` matches zero or more characters; `?` matches one UTF-16 code unit.
Semicolon-separated alternatives are ORed, and spaces around alternatives are
ignored. Plain text otherwise retains internal spaces. The text filter ANDs
with the selected file categories. Linux names are matched after decoding the
helper's escaped-name transport; no Linux helper is started just to filter.

Typing while the file list is focused continues the existing type-to-filter
behavior and mirrors into the field. Ctrl+F from the file list focuses the
field, while Ctrl+F inside a preview retains that preview's Find command.
Native text editing supports paste, selection, undo and IME input. Enter returns
to the list. Tab/Shift+Tab traverse the three footer controls and return to the
list. Enter and Space activate footer buttons. Escape from the list or footer
clears both name and category filters. The × button clears only the text.

Active filters show `visible of total items`; total respects hidden/system
visibility and includes folders. Each tab keeps its own filters while the
application runs. Filters survive refresh and sorting, and clear when navigation
to another folder completes. They are not saved in restart sessions or saved
workspaces. Rows hidden by filtering are removed from the selection, so later
file actions cannot silently act on hidden selected files. Clearing filters does
not reselect those files.

Build/run the focused acceptance without desktop input:

```powershell
./build.ps1 -OutputDirectory ./build/footer-filters -Targets folderwing.exe,folderwing-engine.dll,filter_footer_tests.exe
./tests/footer_filters.ps1 -BuildDirectory ./build/footer-filters
```

The harness uses an owned private desktop and temporary fixture folder. WSL and
Docker stay running; no performance baseline or full-suite result is implied.
