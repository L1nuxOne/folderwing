#pragma once
#include <windows.h>
#include "thumbutil.h"
/* All calls are UI-thread owned. submit replaces queued work; workers read only
 * private paths, and results are copied out of shared banks before publication. */
typedef struct thumb_want {
    wchar_t *path;
    uint64_t size;
    int64_t mtime;
    uint32_t edge;
    int pdf, visible; /* decoder kind: 0 image, 1 PDF, 2 muted video poster */
} thumb_want;
void thumbnails_configure(uint32_t cache_mb);
int thumbnails_start(HWND owner, UINT changed);
void thumbnails_stop(void);
void thumbnails_submit(const thumb_want *items, unsigned n);
thumb_entry *thumbnails_get(const wchar_t *path, uint64_t size, int64_t mtime, uint32_t edge);
void thumbnails_stats(unsigned *ready, unsigned *failed, size_t *bytes, uint64_t *decoded);
