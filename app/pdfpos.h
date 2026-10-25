/* pdfpos.h - where each PDF was left, across runs.
 *
 * One small text file (UTF-8), newest first, at most `max` documents:
 *   size <TAB> mtime <TAB> page <TAB> y <TAB> zoom <TAB> x <TAB> path
 * size and mtime are the listing's file version: a changed PDF starts at
 * the top again. Older rows without mtime are read but not restored for a
 * versioned file. y and x are points from the page's top-left (the view's top
 * edge, its left edge when zoomed), zoom is a factor of 100% (0: fitted to
 * the text), so a position survives other window sizes and DPIs.
 *
 * Nothing is read before the first lookup. Saving merges with the file as
 * it is on disk, so two windows remembering different documents keep both;
 * the file is replaced in one step (written beside it, then renamed).
 * Lines that don't parse are dropped. UI thread only. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

typedef struct pdfpos {
    wchar_t *path;
    uint64_t size;
    int64_t mtime;
    uint32_t page;
    float    y, zoom, x;
    uint8_t  touched; /* set in this run: wins over the file when saving */
} pdfpos;

void pdfpos_open(const wchar_t *file, uint32_t max); /* max 0: remember nothing */
const pdfpos *pdfpos_get(const wchar_t *doc, uint64_t size, int64_t mtime);
int  pdfpos_has(const wchar_t *doc); /* any entry, whatever its size */
void pdfpos_set(const wchar_t *doc, uint64_t size, int64_t mtime, uint32_t page, float y, float zoom, float x);
int  pdfpos_save(void);  /* 0 = written or nothing to write */
void pdfpos_close(void); /* saves, then forgets everything */
uint32_t pdfpos_count(void);
