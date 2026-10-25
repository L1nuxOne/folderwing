# Feature roadmap

User-approved priority order, recovered 2026-10-22. Hardening/performance follow-ups
remain separate in [HARDENING_STATUS.md](HARDENING_STATUS.md).

| Priority | Feature | First useful version | Location / status |
|---|---|---|---|
| 1 | Archive browsing and extraction | Browse, preview entries, extract selected items/everything; create archives afterward | On-demand archive helpers. Browsing, nested content previews and single-file extraction shipped; bulk/folder extraction and creation remain follow-ups. |
| 2 | Disk-space analyzer | Expandable size tree, largest files, breakdown by type, reveal results in either pane | Separate Space panel; Windows and native Linux scanning. See [design and usage](DISK_SPACE_ANALYZER.md). |
| 3 | Search inside files | Search text/code within a folder, show matching lines, open preview at a match | On-demand Find in files panel and native Linux helper. Literal search, matching excerpts and exact preview navigation shipped; see [usage](CONTENT_SEARCH.md). |
| 4 | File differences | Two text files, synchronized scrolling and changed lines; binary summary | On-demand diff helper shipped from one file in each pane; read-only aligned rows, change navigation and byte summary. [Usage and limits](FILE_DIFFERENCES.md). Folder-comparison result integration remains a follow-up. |
| 5 | Saved workspaces | Named sets of tabs, layout and folders | Core/session implementation shipped: explicit Save as/Update, Switch, Rename and confirmed Delete; asynchronous restoration and separate revision-safe catalog. [Usage](SAVED_WORKSPACES.md). |
| 6 | Audio/video previews | Playback, seeking, volume, duration, metadata and video thumbnails | Media helper implemented: MF default, lazy libmpv fallback, playback controls, poster thumbnails and archive third pane. Windows audio and Windows/WSL video acceptance pass. [Usage and status](MEDIA_PREVIEWS.md). |
| 7 | Duplicate finder | Group by size, verify contents, review duplicates, reveal locations | On-demand read-only duplicate panel implemented: size/SHA-256/byte verification, Windows/native WSL and mixed roots, preview/reveal, progress/cancel and retained results. [Usage and limits](DUPLICATE_FINDER.md). |
| 8 | Git integration | On-demand changed/untracked indicators, status, diff and repository terminal | On-demand read-only Git helper implemented: staged/unstaged/untracked/conflict lists, cached badges, blob-based diffs, preview/reveal and repository terminal; Windows/native WSL. [Usage and limits](GIT_INTEGRATION.md). Stage/commit/history/remote actions remain follow-ups. |

The separately developed native Direct client is also available through the
optional **Direct agent** dock/palette entry; deployment/runtime usage is in
[DIRECT_AGENT.md](DIRECT_AGENT.md). This integration is separate from the original
eight-tool feature pack and its nine-addition audit snapshot.
