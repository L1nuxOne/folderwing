# Archive previews

Implemented 2026-10-22: read-only archive navigation with on-demand miniz and
libarchive helpers. The main application contains the pane/controller code, not
either decoder library.

## Interaction

Select an archive in either file pane. Its contents replace the opposite pane,
preserving that pane's path, selection and tabs. The initial listing does not
extract any entry. Enter or double-click opens a folder; Backspace or the header's
Up button returns to its parent. Names and paths remain relative to the archive.

Selecting a supported entry opens the existing PDF, image or text reader in a
third column after a short selection debounce. Markdown uses the existing text
reader. Tab cycles focus; the content divider is draggable. Closing the content
leaves the archive visible. Closing the archive restores the ordinary pane layout,
including single-pane mode and archives selected from the right pane.

The archive context menu provides Open / preview, Extract file, Copy name, Copy
relative path, Up and Close as applicable. Its separate logical command scope
prevents ordinary Delete, Rename and Paste commands from acting on the outer
archive when focus is inside the archive or its nested reader.

**Extract file…** chooses an existing destination folder and decodes one regular
file to an owned private temporary source. It then stages a normal copy in
Operations. Choose conflict handling and press Start there. The source remains
retained if the archive closes and is released after cancellation or job
completion. Existing copy durability checks remain in force. Extraction currently
preserves bytes and the file name, not the archive's timestamps, ACLs or modes.

Archive access uses Windows provider paths. Engine-escaped Linux identities
that require native access are refused before probing or decoder launch, including
quoted parent components and mapped WSL shares. Ordinary Windows names containing
the same literal private-use character remain supported.

## Engines and bounds

- `folderwing-zip.exe`: miniz 3.1.2 for ordinary ZIP files and ZIP-based extensions.
- `folderwing-archive.exe`: libarchive 3.8.10, including zlib, bzip2 and LZMA/XZ, for other
  supported formats and ZIP methods that miniz cannot handle. TAR, TAR.GZ, 7z and
  BZIP2-compressed ZIP fallback are exercised by the focused acceptance fixtures.
  Format/codec recognition is not a promise that every variant is supported.
- Only built-in filters are registered; the helper does not invoke external
  decompression commands from PATH. Unavailable codecs and encrypted entries
  report an error; password entry is not implemented.
- Helpers launch through the existing restricted low-integrity preview launcher,
  with a kill-on-close job and a 512 MiB process memory cap. Metadata and helper
  state are reused between selections. Helpers close after 30 seconds idle.
- Listings allow at most 100,000 entries, 4,095 UTF-16 units per name and 32 MiB
  of names. Compressed stream formats can require scanning the stream to list.
- Automatic preview decodes only the selected entry, up to 64 MiB. Explicit
  extraction streams at most 16 GiB. Superseded requests cannot publish results.
- Path traversal, absolute paths, symlinks and special entries are excluded from
  extraction. Names that cannot be represented as a Windows file name remain
  viewable where supported but cannot be extracted through this first UI.
- Decoders receive no destination paths. The controller creates exact owned
  temporary files and does not recursively clean arbitrary directories. Normal
  cleanup removes only its owned file/directory; failed cleanup is left for review.
- Ordinary source cleanup remains asynchronous. Normal final shutdown first
  finishes engine use and releases Operations pins, then drains archive workers
  and queued cleanup attempts outside the live window/message path. A stopped
  worker releasing an extracted result late is included in that drain. Cleanup
  scheduling or deletion failures still preserve data for review; the drain does
  not broaden deletion to other files or recursively remove directories.

Archive writing, password dialogs, folder/multi-file extraction and nested archive
browsing are future features. The engine comparison's encoding measurements do
not imply an archive-creation feature is included here.

## Build and focused validation

`tools/archive-dependencies.json` pins source/tool downloads and checksums.
`tools/get-archives.ps1` builds static libraries into the ignored local dependency
cache, verifies cached assets and copies third-party notices into the build.
It does not install system tools or require WSL.

`tests/archive_preview.ps1` verifies the packaged fixture hashes, freezes input
hashes, and runs eight native archive selectors with real helper processes and
owned nonactivated Windows windows. It exercises backend routing, exact decoded
bytes, image pixel delivery, third-pane layout, folder navigation, supersession,
unsafe/corrupt/oversized entries, command scope, pane restoration and Operations
source lifetime. It does not require desktop input or stop WSL/Docker.

Current implementation evidence: [closeout](../build/archive_preview/20261022/RESULTS.md).
This is focused feature acceptance, not a new full hardening/performance campaign.
