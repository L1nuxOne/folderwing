/* pdfproto.h - what the app and the PDF helper (folderwing-pdf.exe) say to each other.
 *
 * The helper hosts PDFium in its own process, with a restricted token at
 * low integrity, inside a job that caps its memory and kills it with the
 * app. A malformed PDF can only crash the helper; stopping the helper gives
 * all of its memory back.
 *
 * Messages are a pdfp_head plus `size` payload bytes, over the helper's
 * stdin (requests) and stdout (replies). Rendered pixels never go through
 * the pipe: the app creates one shared section of PDFP_SLOTS tiles, and a
 * RENDER request names the slot to draw into (32-bit BGRx, top-down, rows
 * of w pixels). The app draws a slot only once its RENDERED reply arrived.
 *
 * Requests carry an epoch. When the view moves, the app raises the epoch
 * and asks again for the tiles it still needs; the helper skips renders
 * from older epochs that are still queued, so scrolling never waits behind
 * tiles that went off screen. */
#pragma once
#include <stdint.h>

#define PDFP_TILE       512
#define PDFP_SLOT_BYTES (PDFP_TILE * PDFP_TILE * 4)
#define PDFP_SLOTS      64 /* 64 MB reserved; pages are only touched as tiles are drawn */
#define PDFP_PATH_MAX   32768
#define PDFP_MAX_PAGES  100000
#define PDFP_TEXT_MAX   100000 /* characters of one page; more are dropped */

enum {
    /* app -> helper */
    PDFP_OPEN = 1, /* payload: UTF-16 path, NUL-terminated; replaces the open document */
    PDFP_CLOSE,    /* closes the document */
    PDFP_RENDER,   /* payload: pdfp_render */
    PDFP_QUIT,
    PDFP_TEXT,     /* payload: uint32_t page; never skipped, answered in order */
    PDFP_FIND,     /* payload: pdfp_find, then the UTF-16 query, NUL-terminated; empty: stop */
    PDFP_THUMB_OPEN, /* OPEN for first-page thumbnail: reports only page 0, skips fit analysis */
    PDFP_OUTLINE, /* bounded document outline; request has no payload */
    /* helper -> app */
    PDFP_OPENED = 101, /* payload: pdfp_opened, then npages pdfp_page */
    PDFP_RENDERED,     /* payload: pdfp_rendered */
    PDFP_TEXTED,       /* payload: pdfp_text, then n pdfp_char */
    PDFP_FOUND,        /* payload: pdfp_found, then n pdfp_hit: one page searched */
    PDFP_FIND_DONE,    /* payload: pdfp_found (page: pages searched, n: hits in all) */
    PDFP_OUTLINED,     /* payload: pdfp_outline, then count pdfp_outline_entry */
};

typedef struct pdfp_head {
    uint32_t type, size; /* size: payload bytes that follow */
    uint32_t doc;        /* the app's number for the document */
    uint32_t ticket;     /* RENDER: echoed in RENDERED */
} pdfp_head;

enum { PDFP_FIT_PAGE, PDFP_FIT_TEXT, PDFP_FIT_INK };

typedef struct pdfp_opened {
    int32_t  err;     /* 0, or FPDF_ERR_* (4 = password) */
    uint32_t npages;
    uint32_t fit_kind; /* PDFP_FIT_*: what the box below was measured from */
    uint32_t fit_page; /* the page it was measured on */
    float    fit_x0, fit_y0, fit_x1, fit_y1; /* points, from the page's top-left */
    uint64_t size;                           /* the file's length (remembered positions are by path and size) */
} pdfp_opened;

typedef struct pdfp_page {
    float w, h; /* points */
} pdfp_page;

/* Colors (pdfp_render.colors). Dark pages invert lightness and keep hue,
 * so white paper turns dark gray, black text light gray, a red curve stays
 * red; then the range is narrowed to PDFP_DARK_LO..PDFP_DARK_HI.
 *   ORIGINAL  as the PDF draws it
 *   MIXED     decided per page: pages that are mostly pictures (scans,
 *             slides with a photo behind the text) stay as they are; on
 *             other pages everything turns dark except the pictures
 *   DARK      every page dark, pictures too */
enum { PDFP_COLORS_ORIGINAL, PDFP_COLORS_MIXED, PDFP_COLORS_DARK };
#define PDFP_DARK_LO 38  /* where white paper ends up */
#define PDFP_DARK_HI 222 /* where black text ends up */

typedef struct pdfp_render {
    uint32_t page, slot, epoch;
    float    scale;  /* device pixels per point */
    int32_t  x0, y0; /* the tile's top-left, in device pixels from the page's top-left */
    uint32_t w, h;   /* at most PDFP_TILE each */
    uint32_t colors; /* PDFP_COLORS_* */
} pdfp_render;

typedef struct pdfp_rendered {
    uint32_t slot;
    int32_t  err;  /* 0 = drawn; otherwise the slot holds nothing usable */
    uint32_t dark; /* the page's paper came out dark */
} pdfp_rendered;

/* A page's text in PDFium's reading order. PDFium adds characters of its
 * own between words and lines (PDFP_CH_GEN: a space, or "\r\n" at a line
 * break); they have no box. */
typedef struct pdfp_text {
    uint32_t page, n;
} pdfp_text;

#define PDFP_CH_CODE   0x001FFFFFu /* the code point */
#define PDFP_CH_GEN    0x01000000u /* added by PDFium, not in the page */
#define PDFP_CH_HYPHEN 0x02000000u /* a hyphen that splits a word at the line's end */

typedef struct pdfp_char {
    uint32_t cp;         /* code point | PDFP_CH_* */
    float    l, t, r, b; /* the glyph's cell (font height, not ink), points from the page's top-left */
} pdfp_char;

/* Find searches one page at a time, starting at `from` and wrapping
 * around, and reads new requests between pages: renders are never kept
 * waiting, and a new FIND (or OPEN, CLOSE) replaces the search. Replies
 * carry the request's gen, so the app drops those of an older search. */
#define PDFP_FIND_CASE  0x1u
#define PDFP_FIND_WORD  0x2u
#define PDFP_FIND_PAGE_MAX 1000   /* hits reported per page */
#define PDFP_FIND_MAX   100000    /* hits in all; the search stops there */

typedef struct pdfp_find {
    uint32_t gen, flags, from;
} pdfp_find;

typedef struct pdfp_found {
    uint32_t gen, page, n;
} pdfp_found;

typedef struct pdfp_hit {
    uint32_t index, count; /* characters, as numbered in PDFP_TEXT */
    float    l, t, r, b;   /* their cells' union, points from the page's top-left */
} pdfp_hit;

/* Titles are fixed, bounded UTF-16; destinations refer only to this document. */
#define PDFP_OUTLINE_MAX 2048
#define PDFP_OUTLINE_TITLE 160
typedef struct pdfp_outline { uint32_t count, truncated; } pdfp_outline;
typedef struct pdfp_outline_entry {
    uint32_t page, depth;
    float y; /* points from the displayed page's top edge */
    uint16_t title[PDFP_OUTLINE_TITLE];
} pdfp_outline_entry;
