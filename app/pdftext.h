/* pdftext.h - selected PDF characters to clipboard text.
 *
 * Two shapes: layout keeps the page's line breaks; rejoined (the default
 * copy) puts each paragraph on one line, so text pasted elsewhere reflows.
 * Rejoining joins a line to the next with a space, removes a hyphen that
 * only split a word across the break, and starts a new paragraph when
 *   - there is extra space between the lines (more than half a line),
 *   - the line ends well short of its column's right edge, or
 *   - the next line starts with a bullet.
 * Page furniture is left out of rejoined text once something else was
 * copied: bare page numbers near a page's top or bottom, and rotated text
 * (a margin stamp), so a paragraph runs on across pages.
 * Ligatures (fi, fl, ...) are spelled out in both shapes.
 *
 * Pages are fed in order, one range of characters each; nothing is kept of
 * a page once it is added, so a long selection can stream through. No I/O,
 * no PDFium. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include "pdfproto.h"

typedef struct pdftext_line {
    float    l, t, r, b; /* union of the characters' cells; ok = 0 when none had one */
    float    cell;       /* the tallest cell */
    int      ok;
    uint32_t page;
    float    page_h;
} pdftext_line;

typedef struct pdftext {
    int      layout;
    int      oom;
    wchar_t *buf;  /* the text so far */
    size_t   len, cap;
    wchar_t *line; /* the line being collected */
    size_t   llen, lcap;
    pdftext_line cur, a, p; /* collecting; the last line written; the one before it */
    int      cur_hyphen;    /* cur ends in a hyphen that may only split a word */
    int      a_hyphen;      /* the same for a; it is the last character in buf */
} pdftext;

void     pdftext_init(pdftext *t, int layout);
/* Characters [from, to) of a page's text (pdfp_char, in reading order);
 * page_h: the page's height in points. */
void     pdftext_add(pdftext *t, uint32_t page, float page_h, const pdfp_char *c, uint32_t from, uint32_t to);
/* The text, NUL-terminated, lines ended by "\r\n"; the caller frees it.
 * NULL when memory ran out. t needs no further cleanup. */
wchar_t *pdftext_finish(pdftext *t, size_t *len);
void     pdftext_free(pdftext *t); /* drop it unfinished */
