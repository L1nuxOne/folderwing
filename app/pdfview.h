/* pdfview.h - the PDF reader: one child window that shows one document as
 * a continuous column of pages, fitted to the width of its text.
 *
 * Reading only: wheel scrolls, Ctrl+wheel zooms around the pointer,
 * double-click goes back to fitting the text, middle-drag pans. Pages are
 * drawn by the helper process (folderwing-pdf.exe, see pdfproto.h): a small render
 * of each page shows first, sharp tiles replace it as they arrive.
 *
 * Nothing exists before pdfview_start, and pdfview_stop gives everything
 * back (window, helper process, shared memory). */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

#include "pdfproto.h"

int  pdfview_available(void);     /* folderwing-pdf.exe and pdfium.dll are next to the exe */
int  pdfview_start(HWND parent);  /* the reader window, hidden; the helper starts on the first open */
void pdfview_stop(void);
HWND pdfview_window(void);
void pdfview_open(const wchar_t *path, uint64_t size, int64_t mtime); /* unchanged file does nothing */
void pdfview_clear(void);               /* closes the document, keeps the helper */
const wchar_t *pdfview_path(void);      /* NULL when none */
/* Find: Ctrl+F opens the box in the header (or selects what it holds) and
 * moves the keyboard there; F3 / Shift+F3 step through the hits, or open
 * the box when there is nothing to step through. Esc closes it. */
/* Remember where each document was left (pdfpos.h) in file, at most max
 * documents; NULL or 0: don't. Set after each pdfview_start. */
void pdfview_set_memory(const wchar_t *file, uint32_t max);
void pdfview_find(void);
/* PDFP_COLORS_ORIGINAL / MIXED / DARK (pdfproto.h); pages are drawn again. */
void     pdfview_set_colors(uint32_t colors);
uint32_t pdfview_colors(void);
void pdfview_find_step(int dir); /* 1 next, -1 previous */
int  pdfview_helper_pid(void);          /* 0 when it isn't running (tests, reports) */
