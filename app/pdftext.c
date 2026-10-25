/* Selected PDF characters to clipboard text: see pdftext.h. */
#include "pdftext.h"

#include <stdlib.h>
#include <string.h>
#include <wctype.h>

void pdftext_init(pdftext *t, int layout)
{
    memset(t, 0, sizeof *t);
    t->layout = layout;
}

void pdftext_free(pdftext *t)
{
    free(t->buf);
    free(t->line);
    memset(t, 0, sizeof *t);
}

static int grow(pdftext *t, wchar_t **p, size_t *cap, size_t need)
{
    if (need <= *cap) return 1;
    size_t c = *cap ? *cap : 256;
    while (c < need) c *= 2;
    wchar_t *q = realloc(*p, c * sizeof *q);
    if (!q) {
        t->oom = 1;
        return 0;
    }
    *p = q;
    *cap = c;
    return 1;
}

static void put(pdftext *t, wchar_t **p, size_t *len, size_t *cap, const wchar_t *s, size_t n)
{
    if (t->oom || !grow(t, p, cap, *len + n + 1)) return;
    memcpy(*p + *len, s, n * sizeof *s);
    *len += n;
}

static void line_put(pdftext *t, const wchar_t *s, size_t n) { put(t, &t->line, &t->llen, &t->lcap, s, n); }
static void out_put(pdftext *t, const wchar_t *s, size_t n) { put(t, &t->buf, &t->len, &t->cap, s, n); }

static int is_bullet(wchar_t c)
{
    return c == 0x2022 || c == 0x25E6 || c == 0x25AA || c == 0x25CF || c == 0x2023 || c == 0x2043 || c == 0x00B7 ||
           c == 0x25A0 || c == 0x25A1 || c == 0x2013 || c == 0x2014;
}

static int is_hyphen(uint32_t c) { return c == '-' || c == 0x2010 || c == 0x00AD; }

static int overlap_x(const pdftext_line *x, const pdftext_line *y)
{
    float lo = x->l > y->l ? x->l : y->l, hi = x->r < y->r ? x->r : y->r;
    return x->ok && y->ok && x->page == y->page && hi > lo;
}

/* Whether a new paragraph starts between a (written) and b (collected), p
 * being the line before a. */
static int para_break(const pdftext *t, const pdftext_line *b)
{
    const pdftext_line *a = &t->a, *p = &t->p;
    if (t->llen && is_bullet(t->line[0])) return 1;
    if (!a->ok || !b->ok) return 0;
    float h = a->b - a->t;
    if (h <= 0) return 0;
    int same_col = overlap_x(a, b) || (b->page != a->page && b->l < a->r && b->r > a->l);
    /* extra space before b, in the same column */
    if (same_col && b->page == a->page && b->t >= a->t && b->t - a->b > h * 0.5f) return 1;
    /* a ends short of its column's right edge, as known from its neighbours there */
    float edge = a->r;
    if (overlap_x(p, a) && p->r > edge) edge = p->r;
    if (same_col && b->r > edge) edge = b->r;
    return a->r < edge - h * 2;
}

/* A bare page number in the top or bottom tenth of the page ("12", "- 12 -", "xiv"),
 * or rotated text: its cells stack up instead of running along. */
static int furniture(const pdftext *t, size_t skip)
{
    const pdftext_line *c = &t->cur;
    if (!c->ok) return 0;
    if (t->llen - skip >= 3 && c->b - c->t > c->cell * 3) return 1;
    if (c->page_h <= 0 || (c->b > c->page_h * 0.1f && c->t < c->page_h * 0.9f)) return 0;
    /* digits, or roman numerals of i, v, x only (so a word like "mid" or "civil" stays) */
    int digits = 0, roman = 0;
    for (size_t i = skip; i < t->llen; i++) {
        wchar_t ch = t->line[i];
        if (iswdigit(ch)) digits++;
        else if (ch && wcschr(L"ivxIVX", ch)) roman++;
        else if (!ch || !wcschr(L" -.\x2013\x2014", ch)) return 0;
    }
    return digits ? digits + roman <= 6 && !roman : roman > 0 && roman <= 5;
}

static void end_line(pdftext *t)
{
    while (t->llen && t->line[t->llen - 1] == L' ') t->llen--;
    size_t skip = 0;
    while (skip < t->llen && t->line[skip] == L' ') skip++;
    if (skip == t->llen || (!t->layout && t->len && furniture(t, skip))) {
        t->llen = 0;
        t->cur.ok = 0;
        t->cur_hyphen = 0;
        return;
    }
    if (t->len) {
        if (t->layout || para_break(t, &t->cur)) {
            out_put(t, L"\r\n", 2);
        } else if (t->a_hyphen) {
            /* before a lowercase letter it only split the word; otherwise it is part of it (COVID-19) */
            if (t->buf[t->len - 1] == 0x00AD || iswlower(t->line[skip])) t->len--;
        } else {
            out_put(t, L" ", 1);
        }
    }
    out_put(t, t->line + skip, t->llen - skip);
    t->p = t->a;
    t->a = t->cur;
    t->a_hyphen = t->cur_hyphen;
    t->llen = 0;
    t->cur.ok = 0;
    t->cur_hyphen = 0;
}

void pdftext_add(pdftext *t, uint32_t page, float page_h, const pdfp_char *c, uint32_t from, uint32_t to)
{
    if (t->llen || t->cur.ok) end_line(t); /* a page ends a line */
    t->cur.page = page;
    t->cur.page_h = page_h;
    for (uint32_t i = from; i < to && !t->oom; i++) {
        uint32_t cp = c[i].cp & PDFP_CH_CODE;
        int gen = (c[i].cp & PDFP_CH_GEN) != 0;
        if (cp == '\r') continue;
        if (cp == '\n') {
            end_line(t);
            t->cur.page = page;
            t->cur.page_h = page_h;
            continue;
        }
        if (gen || cp == ' ' || cp == '\t' || cp == 0xA0) {
            /* one space between words when rejoining; as found in layout */
            if (t->llen && (t->layout || t->line[t->llen - 1] != L' ')) line_put(t, L" ", 1);
            continue;
        }
        if (cp == 2) cp = '-'; /* some TeX fonts map their hyphen there */
        if (cp < 0x20 || cp == 0xFFFE || cp == 0xFFFF || (cp >= 0xD800 && cp < 0xE000)) continue;
        static const wchar_t *const lig[] = { L"ff", L"fi", L"fl", L"ffi", L"ffl", L"st", L"st" };
        if (cp >= 0xFB00 && cp <= 0xFB06) {
            line_put(t, lig[cp - 0xFB00], wcslen(lig[cp - 0xFB00]));
        } else if (cp > 0xFFFF) {
            wchar_t pair[2] = { (wchar_t)(0xD800 + ((cp - 0x10000) >> 10)), (wchar_t)(0xDC00 + ((cp - 0x10000) & 0x3FF)) };
            line_put(t, pair, 2);
        } else {
            wchar_t w = (wchar_t)cp;
            line_put(t, &w, 1);
        }
        t->cur_hyphen = is_hyphen(cp) && t->llen >= 2 && iswalpha(t->line[t->llen - 2]);
        if (c[i].r > c[i].l && c[i].b > c[i].t) {
            if (!t->cur.ok) {
                t->cur.l = c[i].l, t->cur.t = c[i].t, t->cur.r = c[i].r, t->cur.b = c[i].b;
                t->cur.cell = 0;
                t->cur.ok = 1;
            } else {
                if (c[i].l < t->cur.l) t->cur.l = c[i].l;
                if (c[i].t < t->cur.t) t->cur.t = c[i].t;
                if (c[i].r > t->cur.r) t->cur.r = c[i].r;
                if (c[i].b > t->cur.b) t->cur.b = c[i].b;
            }
            if (c[i].b - c[i].t > t->cur.cell) t->cur.cell = c[i].b - c[i].t;
        }
        /* PDFium joins a word split at a line's end without a line break, and marks the hyphen */
        if ((c[i].cp & PDFP_CH_HYPHEN) && t->cur_hyphen) {
            end_line(t);
            t->cur.page = page;
            t->cur.page_h = page_h;
        }
    }
    end_line(t);
}

wchar_t *pdftext_finish(pdftext *t, size_t *len)
{
    end_line(t);
    wchar_t *s = NULL;
    if (!t->oom && grow(t, &t->buf, &t->cap, t->len + 1)) {
        t->buf[t->len] = 0;
        s = t->buf;
        if (len) *len = t->len;
        t->buf = NULL;
    }
    pdftext_free(t);
    return s;
}
