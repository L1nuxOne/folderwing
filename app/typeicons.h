/* typeicons.h - file icons that say what a file is, not which app claimed it.
 *
 * Every file maps to a category by its extension (or, without one, by a few
 * well-known names like Makefile and LICENSE). Each category has its own
 * shape and color, drawn with plain GDI at any size: no shell, no icon
 * cache, no worker thread, and the same on every machine whatever is
 * installed. Shape carries the meaning and color helps scanning, so the
 * icons still work for color-blind users.
 *
 * Users can move extensions between categories (ti_add); those win over
 * the built-in table. UI thread only. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

typedef enum ti_kind {
    /* files */
    TI_FOLDER, TI_TEXT, TI_DOC, TI_PDF, TI_SHEET, TI_SLIDES, TI_IMAGE, TI_VIDEO, TI_AUDIO, TI_ARCHIVE,
    TI_CODE, TI_SCRIPT, TI_DATA, TI_DATABASE, TI_PROGRAM, TI_SYSTEM, TI_SHORTCUT, TI_FONT, TI_DISC,
    TI_DESIGN, TI_UNKNOWN,
    TI_FILE_KINDS, /* the ones above can be named in settings */
    /* places */
    TI_DRIVE = TI_FILE_KINDS, TI_HOME, TI_DESKTOP, TI_DOCUMENTS, TI_DOWNLOADS, TI_LINUX,
    TI_COUNT
} ti_kind;

/* ti_draw flags */
#define TI_FADED 0x1u /* hidden items */
#define TI_LINK  0x2u /* symbolic links and junctions: a small arrow */

/* name[0..len); ext: offset of the extension after the last '.', len if none
 * (as in fpe_entry). Directories are TI_FOLDER. */
ti_kind     ti_of_name(const wchar_t *name, uint32_t len, uint32_t ext, uint32_t attrs);
ti_kind     ti_of_path(const wchar_t *path, int folder);
const char *ti_name(ti_kind k);   /* "code", "image", ... ("" past TI_FILE_KINDS) */
int         ti_find(const char *name); /* a ti_name back to its kind, -1 if none */
COLORREF    ti_color(ti_kind k);

/* Extensions ("tsx vue", commas and leading dots allowed) that belong to k
 * from now on. Returns how many were rejected (too long, not ASCII). */
int  ti_add(ti_kind k, const char *exts);
void ti_reset(void); /* forget ti_add */
/* Extensions the built-in table lists under two categories (tests). */
int  ti_duplicates(const char **first);

/* Draws k in the size x size box at (x, y). color 0 = the category's own. */
void ti_draw(HDC dc, ti_kind k, int x, int y, int size, uint32_t flags, COLORREF color);
