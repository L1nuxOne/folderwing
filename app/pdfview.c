/* The PDF reader: see pdfview.h. */
#include "pdfview.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include <windowsx.h>

#include "app.h"
#include "pdfpos.h"
#include "pdfproto.h"
#include "pdftext.h"
#include "previewlaunch.h"
#include "previewreader.h"
#include "pipewriter.h"
#include "theme.h"

#define WM_PDF_MSG  (WM_APP + 60) /* lParam: a msg from the helper */
#define WM_PDF_GONE (WM_APP + 61) /* the helper's pipe closed */
#define WM_PDF_LAUNCHED (WM_APP + 62) /* generation-tagged launch completion */
#define WM_PDF_WRITE_FAILED (WM_APP + 63)
#define TIMER_EDGE  1             /* selecting past the top or bottom: keep scrolling */
#define TIMER_FIND  2             /* typing in the find box settled: search */
#define FIND_TYPING_MS 150

enum { S_EMPTY, S_PENDING, S_READY };
#define THUMB 0xFFFFu

/* Pages whose text is kept for selecting; text is only asked for once the
 * pointer is over a page or a selection reaches it. */
#define TEXT_CACHE 12
#define CARET_END  INT32_MAX /* the end of the page, whatever its length */

typedef struct ptext {
    uint8_t    state;
    uint32_t   page, n, used;
    pdfp_char *c;
} ptext;

typedef struct fhit {
    uint32_t page, index, count;
    float    l, t, r, b;
} fhit;

/* A point between two characters. idx < 0: not known yet, taken from
 * (x, y) in points once the page's text is in. */
typedef struct caret {
    uint32_t page;
    int32_t  idx;
    float    x, y;
} caret;

typedef struct slot {
    uint8_t  state, dark; /* dark: the helper darkened the page (colors) */
    uint16_t tx, ty; /* THUMB: the page's small render */
    uint32_t page, ticket, used, pass;
    float    scale;
    int      w, h;
} slot;

typedef struct msg {
    pdfp_head head;
    uint8_t   data[];
} msg;

static struct {
    HWND hwnd;
    HDC  bdc;
    HBITMAP bmp, old_bmp;
    int  bw, bh, cw, ch;

    /* helper */
    HANDLE process, job, to, from, section;
    preview_reader *reader;
    pipe_writer *writer;
    const uint8_t *pixels;
    DWORD  pid;
    int    gone; /* it stopped on its own (a crash): the next open starts it again */
    /* creating the process takes tens of milliseconds: a thread does it,
     * fills `launched` and posts WM_PDF_LAUNCHED */
    preview_launch *launcher;
    HANDLE child_in, child_out;
    uint32_t launch_gen;

    /* document */
    wchar_t   *path;
    uint64_t   size;
    int64_t    mtime;
    uint32_t   doc, npages;
    pdfp_page *pages;
    pdfp_opened info;
    int        opened, err;

    /* view, in device pixels */
    float   scale;
    int     fit;     /* the scale follows the width (until the user zooms) */
    double  top, left;
    double *ys;      /* each page's top */
    double  total;   /* height of all pages */
    int     docw;    /* widest page */
    double  wheel;
    int     panning, pan_x, pan_y;
    double  pan_top, pan_left;

    slot     slots[PDFP_SLOTS];
    uint32_t ticket, epoch, pass, clock;
    uint32_t colors;    /* PDFP_COLORS_* */
    int      page_dark; /* while painting: the page being drawn has dark paper */

    /* selection and copy */
    ptext    text[TEXT_CACHE];
    caret    anchor, focus;
    int      sel, selecting;
    pdftext  copy;
    int      copying, copy_wanted; /* copy_wanted: 1 + layout, waiting for a caret's page */
    caret    copy_lo, copy_hi;
    uint32_t copy_next;
    uint64_t copy_request;

    /* find: the box exists only while find is open */
    HWND     find_edit, find_back; /* find_back: where the focus was before Ctrl+F */
    WNDPROC  find_orig;
    HBRUSH   find_brush;
    wchar_t  find_q[256];          /* what was searched */
    uint32_t find_gen, find_pages, find_n, find_cap;
    int      find_done, find_cur;  /* find_cur: the current hit, -1 before one is picked */
    int64_t  find_t0;              /* profiling: when the search was asked for */
    fhit    *hits;                 /* in document order */
} R;

static int head_h(void) { return app_px(30); }
static int gap(void) { return app_px(10); }
static int pad(void) { return app_px(14); }
static int view_h(void) { return R.ch - head_h() > 0 ? R.ch - head_h() : 0; }
static float px_per_pt(void) { return (float)app_px(96) / 72.0f; }
static void pos_restore(void);

/* ---- helper process ---------------------------------------------------------------- */

static void exe_dir(wchar_t *buf, size_t cap)
{
    DWORD n = GetModuleFileNameW(NULL, buf, (DWORD)cap);
    wchar_t *slash = n && n < cap ? wcsrchr(buf, L'\\') : NULL;
    if (slash) slash[1] = 0;
    else buf[0] = 0;
}

int pdfview_available(void)
{
    wchar_t dir[MAX_PATH], p[MAX_PATH + 16];
    exe_dir(dir, MAX_PATH);
    _snwprintf(p, MAX_PATH + 16, L"%lsfolderwing-pdf.exe", dir);
    p[MAX_PATH + 15] = 0;
    if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) return 0;
    _snwprintf(p, MAX_PATH + 16, L"%lspdfium.dll", dir);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

static int read_all(HANDLE h, void *buf, DWORD n)
{
    uint8_t *p = buf;
    while (n) {
        DWORD got = 0;
        if (!ReadFile(h, p, n, &got, NULL) || !got) return -1;
        p += got;
        n -= got;
    }
    return 0;
}

static DWORD WINAPI from_helper(void *arg)
{
    preview_reader *reader = arg;
    HANDLE in = reader->pipe;
    HWND to = reader->window;
    uint32_t generation = reader->generation;
    const size_t pages_max = sizeof(pdfp_opened) + PDFP_MAX_PAGES * sizeof(pdfp_page);
    const size_t text_max = sizeof(pdfp_text) + PDFP_TEXT_MAX * sizeof(pdfp_char);
    const size_t most = pages_max > text_max ? pages_max : text_max;
    for (;;) {
        pdfp_head h;
        if (read_all(in, &h, sizeof h) || h.size > most) break;
        msg *m = malloc(sizeof *m + h.size);
        if (!m || read_all(in, m->data, h.size)) {
            free(m);
            break;
        }
        m->head = h;
        if (InterlockedCompareExchange(&reader->cancelled, 0, 0) ||
            !PostMessageW(to, WM_PDF_MSG, generation, (LPARAM)m)) free(m);
    }
    if (!InterlockedCompareExchange(&reader->cancelled, 0, 0)) PostMessageW(to, WM_PDF_GONE, generation, 0);
    preview_reader_finish(reader);
    return 0;
}

static void send_req(uint32_t type, uint32_t doc, uint32_t ticket, const void *data, uint32_t size)
{
    if (!R.writer) return;
    uint8_t buf[sizeof(pdfp_head) + sizeof(pdfp_render)];
    uint8_t *p = size <= sizeof(pdfp_render) ? buf : malloc(sizeof(pdfp_head) + size);
    if (!p) return;
    pdfp_head h = { type, size, doc, ticket };
    memcpy(p, &h, sizeof h);
    if (size) memcpy(p + sizeof h, data, size);
    if (pipe_writer_send(R.writer, p, sizeof h + size, 0, 0))
        PostMessageW(R.hwnd, WM_PDF_WRITE_FAILED, R.launch_gen, 0);
    if (p != buf) free(p);
}

static void helper_stop(void)
{
    ++R.launch_gen;
    preview_launch_cancel(&R.launcher);
    pipe_writer_cancel(&R.writer);
    preview_reader_cancel(&R.reader);
    if (R.to) {
        CloseHandle(R.to); /* EOF: the helper ends even if QUIT is lost */
        R.to = NULL;
    }
    if (R.process) {
        if (R.job) TerminateJobObject(R.job, 1);
        else TerminateProcess(R.process, 1);
        CloseHandle(R.process);
        R.process = NULL;
    }
    if (R.job) CloseHandle(R.job); /* kills whatever is left */
    R.job = NULL;
    if (R.child_in) CloseHandle(R.child_in);
    if (R.child_out) CloseHandle(R.child_out); /* the reader thread's pipe ends */
    R.child_in = R.child_out = NULL;
    if (R.from) CloseHandle(R.from);
    R.from = NULL;
    if (R.pixels) UnmapViewOfFile(R.pixels);
    R.pixels = NULL;
    if (R.section) CloseHandle(R.section);
    R.section = NULL;
    R.pid = 0;
    memset(R.slots, 0, sizeof R.slots);
    if (R.hwnd) {
        MSG m; /* replies already posted by the reader thread */
        while (PeekMessageW(&m, R.hwnd, WM_PDF_MSG, WM_PDF_MSG, PM_REMOVE)) free((void *)m.lParam);
        while (PeekMessageW(&m, R.hwnd, WM_PDF_LAUNCHED, WM_PDF_LAUNCHED, PM_REMOVE)) {}
    }
}

/* Pipes and pixels now, the process on a thread: requests written before
 * it runs wait in the pipe. */
static int helper_start(void)
{
    if (R.process || R.launcher) return 0;
    int64_t t = fpe_prof_begin();
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    int ok = 0;
    R.section = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE | SEC_COMMIT, 0,
                                   (DWORD)PDFP_SLOTS * PDFP_SLOT_BYTES, NULL);
    R.pixels = R.section ? MapViewOfFile(R.section, FILE_MAP_READ, 0, 0, 0) : NULL;
    if (!R.pixels || !CreatePipe(&R.child_in, &R.to, &sa, 1 << 16) || !CreatePipe(&R.from, &R.child_out, &sa, 1 << 16))
        goto out;
    SetHandleInformation(R.to, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(R.from, HANDLE_FLAG_INHERIT, 0);
    R.writer = pipe_writer_begin(R.to, 1 << 20, R.hwnd, WM_PDF_WRITE_FAILED, R.launch_gen + 1);
    R.reader = preview_reader_begin(R.from, R.hwnd, R.launch_gen + 1, from_helper);
    if (!R.writer || !R.reader) goto out;
    CloseHandle(R.from); R.from = NULL;
    R.launcher = preview_launch_begin(L"folderwing-pdf.exe", R.child_in, R.child_out, R.section,
                                      (SIZE_T)1 << 30, R.hwnd, WM_PDF_LAUNCHED, ++R.launch_gen);
    ok = R.launcher != NULL;
    if (ok) {
        CloseHandle(R.child_in); CloseHandle(R.child_out);
        R.child_in = R.child_out = NULL;
    }
out:
    if (!ok) helper_stop();
    R.gone = 0;
    FPE_PROF_END("pdf.helper_start", t);
    return ok ? 0 : -1;
}

/* ---- layout --------------------------------------------------------------------------- */

static int page_w(uint32_t i) { return (int)ceilf(R.pages[i].w * R.scale); }
static int page_h(uint32_t i) { return (int)ceilf(R.pages[i].h * R.scale); }

static float min_scale(void)
{
    float widest = 1;
    for (uint32_t i = 0; i < R.npages; i++)
        if (R.pages[i].w > widest) widest = R.pages[i].w;
    float s = (float)R.cw * 0.25f / widest;
    return s > 0.02f ? s : 0.02f;
}

static float max_scale(void) { return px_per_pt() * 8; }

static void relayout(void)
{
    double y = 0;
    R.docw = 0;
    for (uint32_t i = 0; i < R.npages; i++) {
        R.ys[i] = y;
        y += page_h(i) + gap();
        if (page_w(i) > R.docw) R.docw = page_w(i);
    }
    R.total = R.npages ? y - gap() : 0;
}

static void clamp_view(void)
{
    double max_top = R.total - view_h();
    if (R.top > max_top) R.top = max_top;
    if (R.top < 0) R.top = 0;
    if (R.docw <= R.cw) {
        R.left = -(R.cw - R.docw) / 2.0;
    } else {
        double lo = -pad(), hi = R.docw - R.cw + pad();
        if (R.left < lo) R.left = lo;
        if (R.left > hi) R.left = hi;
    }
}

/* The body text's width fills the pane, with a little air on both sides. */
static void apply_fit(void)
{
    float tw = R.info.fit_x1 - R.info.fit_x0;
    if (tw < 1 || R.cw < 1) return;
    float s = (float)(R.cw - 2 * pad()) / tw;
    if (s > max_scale()) s = max_scale();
    if (s < min_scale()) s = min_scale();
    R.scale = s;
    relayout();
    R.left = R.info.fit_x0 * s - pad();
    clamp_view();
}

static uint32_t page_at_y(double y)
{
    uint32_t lo = 0, hi = R.npages;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (R.ys[mid] <= y) lo = mid;
        else hi = mid;
    }
    return lo;
}

/* Zoom to `s`, keeping the document point under (vx, vy) where it is. */
static void zoom_at(float s, int vx, int vy)
{
    if (!R.opened || !R.npages) return;
    if (s < min_scale()) s = min_scale();
    if (s > max_scale()) s = max_scale();
    double dy = R.top + vy, dx = R.left + vx;
    uint32_t i = page_at_y(dy);
    double py = (dy - R.ys[i]) / R.scale, px = dx / R.scale;
    R.scale = s;
    R.fit = 0;
    relayout();
    R.top = R.ys[i] + py * s - vy;
    R.left = px * s - vx;
    clamp_view();
}

/* ---- tiles ------------------------------------------------------------------------------ */

static slot *find_slot(uint32_t page, uint16_t tx, uint16_t ty, float scale)
{
    for (int i = 0; i < PDFP_SLOTS; i++) {
        slot *s = &R.slots[i];
        if (s->state != S_EMPTY && s->page == page && s->tx == tx && s->ty == ty && s->scale == scale) return s;
    }
    return NULL;
}

static slot *victim(void)
{
    slot *best = NULL;
    for (int i = 0; i < PDFP_SLOTS; i++) {
        slot *s = &R.slots[i];
        if (s->state == S_EMPTY) return s;
        if (s->pass == R.pass) continue; /* needed for this view */
        if (!best || s->used < best->used) best = s;
    }
    return best;
}

static void send_render(const slot *s)
{
    pdfp_render r = { s->page, (uint32_t)(s - R.slots), R.epoch, s->scale, 0, 0, (uint32_t)s->w, (uint32_t)s->h,
                      R.colors };
    if (s->tx != THUMB) {
        r.x0 = s->tx * PDFP_TILE;
        r.y0 = s->ty * PDFP_TILE;
    }
    send_req(PDFP_RENDER, R.doc, s->ticket, &r, sizeof r);
}

static float thumb_scale(uint32_t i)
{
    float a = 256.0f / R.pages[i].w, b = (float)PDFP_TILE / R.pages[i].h;
    return a < b ? a : b;
}

static void need(uint32_t page, uint16_t tx, uint16_t ty)
{
    float scale = tx == THUMB ? thumb_scale(page) : R.scale;
    slot *s = find_slot(page, tx, ty, scale);
    if (s) {
        s->pass = R.pass;
        s->used = ++R.clock;
        if (s->state == S_PENDING) send_render(s); /* again, in the current epoch */
        return;
    }
    if (!(s = victim())) return;
    s->state = S_PENDING;
    s->page = page;
    s->tx = tx;
    s->ty = ty;
    s->scale = scale;
    s->pass = R.pass;
    s->used = ++R.clock;
    s->ticket = ++R.ticket;
    if (tx == THUMB) {
        s->w = (int)ceilf(R.pages[page].w * scale);
        s->h = (int)ceilf(R.pages[page].h * scale);
    } else {
        int pw = page_w(page), ph = page_h(page);
        s->w = pw - tx * PDFP_TILE < PDFP_TILE ? pw - tx * PDFP_TILE : PDFP_TILE;
        s->h = ph - ty * PDFP_TILE < PDFP_TILE ? ph - ty * PDFP_TILE : PDFP_TILE;
    }
    if (s->w < 1) s->w = 1;
    if (s->h < 1) s->h = 1;
    if (s->w > PDFP_TILE) s->w = PDFP_TILE;
    if (s->h > PDFP_TILE) s->h = PDFP_TILE;
    send_render(s);
}

/* Tiles of the pages in [y0, y1) (document pixels), nearest the view first. */
static void need_tiles(double y0, double y1, int thumbs_only)
{
    if (y1 <= y0 || !R.npages) return;
    uint32_t first = page_at_y(y0);
    for (uint32_t i = first; i < R.npages && R.ys[i] < y1; i++) {
        if (thumbs_only) {
            need(i, THUMB, 0);
            continue;
        }
        int pw = page_w(i), ph = page_h(i);
        double a = y0 - R.ys[i], b = y1 - R.ys[i];
        int ty0 = a < 0 ? 0 : (int)(a / PDFP_TILE), ty1 = (int)((b < ph ? b : ph - 1) / PDFP_TILE);
        int tx0 = R.left < 0 ? 0 : (int)(R.left / PDFP_TILE);
        int tx1 = (int)(((R.left + R.cw) < pw ? R.left + R.cw : pw - 1) / PDFP_TILE);
        for (int ty = ty0; ty <= ty1; ty++)
            for (int tx = tx0; tx <= tx1; tx++) need(i, (uint16_t)tx, (uint16_t)ty);
    }
}

static void update_requests(void)
{
    if (!R.opened || !R.npages || !R.to) return;
    R.epoch++;
    R.pass++;
    double vh = view_h();
    need_tiles(R.top - vh * 0.25, R.top + vh * 1.25, 1); /* small renders first: something to show at once */
    need_tiles(R.top, R.top + vh, 0);
    need_tiles(R.top + vh, R.top + vh * 1.5, 0); /* the next half screen */
}

static void view_changed(void)
{
    clamp_view();
    update_requests();
    InvalidateRect(R.hwnd, NULL, FALSE);
}

/* ---- text and selection ------------------------------------------------------------------ */

static void text_drop_all(void)
{
    if (R.hwnd) app_clipboard_cancel(R.hwnd);
    for (int i = 0; i < TEXT_CACHE; i++) free(R.text[i].c);
    memset(R.text, 0, sizeof R.text);
    if (R.copying) pdftext_free(&R.copy);
    R.copying = R.copy_wanted = 0;
    R.sel = R.selecting = 0;
}

static ptext *text_find(uint32_t page)
{
    for (int i = 0; i < TEXT_CACHE; i++)
        if (R.text[i].state != S_EMPTY && R.text[i].page == page) return &R.text[i];
    return NULL;
}

/* Kept while something depends on it: a selection's ends, the next pages to copy. */
static int text_pinned(uint32_t page)
{
    if (R.sel && (page == R.anchor.page || page == R.focus.page)) return 1;
    return R.copying && page >= R.copy_next && page < R.copy_next + 4;
}

static ptext *text_slot(void)
{
    ptext *best = NULL;
    for (int i = 0; i < TEXT_CACHE; i++) {
        ptext *t = &R.text[i];
        if (t->state == S_EMPTY) return t;
        if (t->state == S_PENDING || text_pinned(t->page)) continue;
        if (!best || t->used < best->used) best = t;
    }
    if (best) {
        free(best->c);
        memset(best, 0, sizeof *best);
    }
    return best;
}

/* The page's text if it is here; otherwise asks for it and returns NULL. */
static ptext *text_want(uint32_t page)
{
    if (!R.opened || page >= R.npages || !R.to) return NULL;
    ptext *t = text_find(page);
    if (t) {
        t->used = ++R.clock;
        return t->state == S_READY ? t : NULL;
    }
    if (!(t = text_slot())) return NULL;
    t->state = S_PENDING;
    t->page = page;
    t->used = ++R.clock;
    send_req(PDFP_TEXT, R.doc, 0, &page, sizeof page);
    return NULL;
}

static int has_box(const pdfp_char *c) { return c->r > c->l && c->b > c->t; }

/* The caret nearest (x, y): before or after the nearest character, by which
 * half of it the point is in. Distance counts vertical misses four times,
 * so a point beside a line stays on that line. */
static int32_t caret_at(const ptext *t, float x, float y)
{
    int32_t best = -1;
    float best_d = 0;
    for (uint32_t i = 0; i < t->n; i++) {
        const pdfp_char *c = &t->c[i];
        if (!has_box(c)) continue;
        float dx = x < c->l ? c->l - x : x > c->r ? x - c->r : 0;
        float dy = y < c->t ? c->t - y : y > c->b ? y - c->b : 0;
        float d = dx + dy * 4;
        if (best < 0 || d < best_d) {
            best = (int32_t)i;
            best_d = d;
            if (d == 0) break;
        }
    }
    if (best < 0) return 0;
    const pdfp_char *c = &t->c[best];
    return best + (x > (c->l + c->r) / 2);
}

/* The character under (x, y), -1 if none. */
static int32_t char_under(const ptext *t, float x, float y)
{
    for (uint32_t i = 0; i < t->n; i++) {
        const pdfp_char *c = &t->c[i];
        if (has_box(c) && x >= c->l && x <= c->r && y >= c->t && y <= c->b) return (int32_t)i;
    }
    return -1;
}

static int resolve(caret *k)
{
    if (k->idx >= 0) return 1;
    ptext *t = text_want(k->page);
    if (!t) return 0;
    k->idx = caret_at(t, k->x, k->y);
    return 1;
}

static int32_t clamp_idx(const caret *k, const ptext *t) { return k->idx > (int32_t)t->n ? (int32_t)t->n : k->idx; }

static int caret_less(const caret *a, const caret *b)
{
    return a->page < b->page || (a->page == b->page && a->idx < b->idx);
}

/* The selection's ends in order: 1 = some text, 0 = none, -1 = not known
 * yet (a page's text is on its way). */
static int sel_state(caret *lo, caret *hi)
{
    if (!R.sel) return 0;
    int a = resolve(&R.anchor), f = resolve(&R.focus);
    if (!a || !f) return -1;
    int fwd = !caret_less(&R.focus, &R.anchor);
    *lo = fwd ? R.anchor : R.focus;
    *hi = fwd ? R.focus : R.anchor;
    if (lo->page == hi->page) {
        ptext *t = text_find(lo->page);
        if (!t || t->state != S_READY) return lo->idx < hi->idx ? 1 : 0;
        if (clamp_idx(lo, t) >= clamp_idx(hi, t)) return 0;
    }
    return 1;
}

static int sel_bounds(caret *lo, caret *hi) { return sel_state(lo, hi) == 1; }

/* A point in the view (client pixels) as a caret: the page it is on (or
 * the one above, between pages), in points from the page's top-left. */
static int point_caret(int cx, int cy, caret *k)
{
    if (!R.opened || !R.npages) return 0;
    double dy = R.top + (cy - head_h()), dx = R.left + cx;
    uint32_t i = page_at_y(dy);
    k->page = i;
    k->idx = -1;
    k->x = (float)(dx / R.scale);
    k->y = (float)((dy - R.ys[i]) / R.scale);
    resolve(k);
    return 1;
}

static int word_char(uint32_t cp)
{
    cp &= PDFP_CH_CODE;
    return cp > 0xFFFF || iswalnum((wint_t)cp) || cp == '_' || (cp >= 0xFB00 && cp <= 0xFB06);
}

/* Double-click: the word under the point (or the one character, if it isn't part of a word). */
static int select_word(int cx, int cy)
{
    caret k;
    if (!point_caret(cx, cy, &k)) return 0;
    ptext *t = text_want(k.page);
    int32_t i = t ? char_under(t, k.x, k.y) : -1;
    if (i < 0) return 0;
    int32_t a = i, b = i + 1;
    if (word_char(t->c[i].cp)) {
        while (a > 0 && !(t->c[a - 1].cp & PDFP_CH_GEN) && word_char(t->c[a - 1].cp)) a--;
        while (b < (int32_t)t->n && !(t->c[b].cp & PDFP_CH_GEN) && word_char(t->c[b].cp)) b++;
    }
    R.anchor = R.focus = k;
    R.anchor.idx = a;
    R.focus.idx = b;
    R.sel = 1;
    InvalidateRect(R.hwnd, NULL, FALSE);
    return 1;
}

static void select_all(void)
{
    if (!R.opened || !R.npages) return;
    memset(&R.anchor, 0, sizeof R.anchor);
    R.focus = R.anchor;
    R.focus.page = R.npages - 1;
    R.focus.idx = CARET_END;
    R.sel = 1;
    InvalidateRect(R.hwnd, NULL, FALSE);
}

static int sel_clear(void)
{
    caret lo, hi;
    int had = sel_bounds(&lo, &hi);
    R.sel = 0;
    InvalidateRect(R.hwnd, NULL, FALSE);
    return had;
}

static int clip_put(const wchar_t *s, size_t len, int layout)
{
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (len + 1) * sizeof *s);
    wchar_t *p = g ? GlobalLock(g) : NULL;
    if (!p) {
        if (g) GlobalFree(g);
        return 0;
    }
    memcpy(p, s, (len + 1) * sizeof *s);
    GlobalUnlock(g);
    wchar_t success[160];
    _snwprintf(success, 160, layout ? L"Copied %u characters, lines as on the page" : L"Copied %u characters", (unsigned)len);
    app_clipboard_submit(R.copy_request, CF_UNICODETEXT, g, 0, NULL, success);
    return 1;
}

/* Feeds the pages to copy in order, as their text arrives. */
static void copy_pump(void)
{
    while (R.copying) {
        uint32_t p = R.copy_next;
        if (p > R.copy_hi.page) {
            size_t len = 0;
            int layout = R.copy.layout;
            wchar_t *s = pdftext_finish(&R.copy, &len);
            R.copying = 0;
            if (!s || !clip_put(s, len, layout)) {
                app_status(NULL, 1, L"Not enough memory to copy that much text");
            }
            free(s);
            InvalidateRect(R.hwnd, NULL, FALSE);
            return;
        }
        for (uint32_t k = 1; k < 4 && p + k <= R.copy_hi.page; k++) text_want(p + k); /* the next ones meanwhile */
        ptext *t = text_want(p);
        if (!t) return; /* on its way */
        uint32_t from = p == R.copy_lo.page ? (uint32_t)clamp_idx(&R.copy_lo, t) : 0;
        uint32_t to = p == R.copy_hi.page ? (uint32_t)clamp_idx(&R.copy_hi, t) : t->n;
        if (from < to) pdftext_add(&R.copy, p, R.pages[p].h, t->c, from, to);
        R.copy_next++;
    }
}

static void copy_start(int layout)
{
    caret lo, hi;
    int st = sel_state(&lo, &hi);
    if (st != 1) {
        R.copy_wanted = st < 0 ? 1 + layout : 0; /* -1: an end's page text is on its way */
        return;
    }
    R.copy_wanted = 0;
    if (R.copying) pdftext_free(&R.copy);
    pdftext_init(&R.copy, layout);
    R.copying = 1;
    R.copy_lo = lo;
    R.copy_hi = hi;
    R.copy_next = lo.page;
    copy_pump();
    if (R.copying) InvalidateRect(R.hwnd, NULL, FALSE); /* the header shows progress */
}

static void text_arrived(const msg *mm)
{
    if (mm->head.size < sizeof(pdfp_text)) return;
    pdfp_text th;
    memcpy(&th, mm->data, sizeof th);
    if (th.n > PDFP_TEXT_MAX || mm->head.size < sizeof th + (size_t)th.n * sizeof(pdfp_char) || th.page >= R.npages)
        return;
    ptext *t = text_find(th.page);
    if (!t && !(t = text_slot())) return;
    free(t->c);
    t->c = th.n ? malloc(th.n * sizeof *t->c) : NULL;
    t->n = t->c ? th.n : 0;
    if (t->c) memcpy(t->c, mm->data + sizeof th, th.n * sizeof *t->c);
    t->page = th.page;
    t->state = S_READY;
    t->used = ++R.clock;
    if (R.sel) {
        resolve(&R.anchor);
        resolve(&R.focus);
    }
    if (R.copy_wanted) copy_start(R.copy_wanted - 1);
    copy_pump();
    InvalidateRect(R.hwnd, NULL, FALSE);
}

/* A highlight band that keeps the text readable. On light paper the page is
 * multiplied by the color (dark text stays dark); on dark paper that would
 * vanish, so the color is laid over it at part strength instead. */
static void band(HDC dc, int x, int y, int w, int h, COLORREF color)
{
    if (w <= 0 || h <= 0) return;
    if (!R.page_dark) {
        HGDIOBJ old = SelectObject(dc, GetStockObject(DC_BRUSH));
        SetDCBrushColor(dc, color);
        PatBlt(dc, x, y, w, h, 0x00A000C9); /* DPa */
        SelectObject(dc, old);
        return;
    }
    static HDC one;
    static uint32_t *pixel;
    if (!one) {
        BITMAPINFO bi = { { sizeof bi.bmiHeader, 1, 1, 1, 32, BI_RGB } };
        void *bits = NULL;
        HBITMAP bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!bmp) return;
        one = CreateCompatibleDC(NULL);
        if (!one) {
            DeleteObject(bmp);
            return;
        }
        SelectObject(one, bmp); /* kept for the process: one pixel */
        pixel = bits;
    }
    *pixel = (uint32_t)GetBValue(color) | (uint32_t)GetGValue(color) << 8 | (uint32_t)GetRValue(color) << 16;
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 120, 0 };
    GdiAlphaBlend(dc, x, y, w, h, one, 0, 0, 1, 1, bf);
}

/* Characters [from, to) of a page as highlight bands, one per line. */
static void paint_runs(HDC dc, const ptext *t, uint32_t from, uint32_t to, int x, int y, COLORREF color)
{
    if (to > t->n) to = t->n;
    float s = R.scale, l = 0, tp = 0, r = 0, b = 0;
    int open = 0;
    for (uint32_t k = from; k <= to; k++) {
        const pdfp_char *c = k < to ? &t->c[k] : NULL;
        if (c && !has_box(c)) continue;
        /* the same line: extend the run (it covers the spaces between words) */
        if (c && open && (c->t + c->b) / 2 >= tp && (c->t + c->b) / 2 <= b && c->l >= l - (b - tp)) {
            if (c->l < l) l = c->l;
            if (c->r > r) r = c->r;
            if (c->t < tp) tp = c->t;
            if (c->b > b) b = c->b;
            continue;
        }
        if (open) {
            int x0 = x + (int)floorf(l * s), y0 = y + (int)floorf(tp * s);
            band(dc, x0, y0, x + (int)ceilf(r * s) - x0, y + (int)ceilf(b * s) - y0, color);
        }
        open = c != NULL;
        if (c) l = c->l, tp = c->t, r = c->r, b = c->b;
    }
}

static void paint_selection(HDC dc, uint32_t i, int x, int y, const caret *lo, const caret *hi)
{
    if (i < lo->page || i > hi->page) return;
    ptext *t = text_want(i);
    if (!t) return;
    uint32_t from = i == lo->page ? (uint32_t)clamp_idx(lo, t) : 0;
    uint32_t to = i == hi->page ? (uint32_t)clamp_idx(hi, t) : t->n;
    paint_runs(dc, t, from, to, x, y, RGB(164, 200, 255));
}

/* ---- find ------------------------------------------------------------------------------------- */

#define C_HIT     RGB(255, 226, 100)
#define C_HIT_CUR RGB(255, 150, 40)

static int find_open(void) { return R.find_edit != NULL; }

/* The first hit on page `page` or after it. */
static uint32_t hit_lower(uint32_t page, uint32_t index)
{
    uint32_t lo = 0, hi = R.find_n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        const fhit *h = &R.hits[mid];
        if (h->page < page || (h->page == page && h->index < index)) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static void hits_clear(void)
{
    free(R.hits);
    R.hits = NULL;
    R.find_n = R.find_cap = R.find_pages = 0;
    R.find_cur = -1;
    R.find_done = 0;
}

/* Scrolls just enough to show hit i, with some room above it. */
static void find_show(int i)
{
    if (i < 0 || (uint32_t)i >= R.find_n || !R.opened) return;
    const fhit *h = &R.hits[i];
    if (h->page >= R.npages) return;
    double s = R.scale, y0 = R.ys[h->page] + h->t * s, y1 = R.ys[h->page] + h->b * s;
    double x0 = h->l * s, x1 = h->r * s;
    if (y0 < R.top || y1 > R.top + view_h()) R.top = y0 - view_h() / 3.0;
    if (x0 < R.left || x1 > R.left + R.cw) R.left = (x0 + x1) / 2 - R.cw / 2.0;
    view_changed();
}

/* Searches for what the box holds, from the page in view; empty stops. */
static void find_restart(void)
{
    KillTimer(R.hwnd, TIMER_FIND);
    wchar_t q[256] = L"";
    if (R.find_edit) GetWindowTextW(R.find_edit, q, 256);
    wcscpy(R.find_q, q);
    hits_clear();
    R.find_gen++;
    if (!R.opened || !R.to) {
        InvalidateRect(R.hwnd, NULL, FALSE);
        return;
    }
    size_t n = wcslen(q);
    uint8_t buf[sizeof(pdfp_find) + sizeof q];
    pdfp_find f = { R.find_gen, 0, R.npages ? page_at_y(R.top) : 0 };
    memcpy(buf, &f, sizeof f);
    memcpy(buf + sizeof f, q, (n + 1) * sizeof *q);
    send_req(PDFP_FIND, R.doc, 0, buf, (uint32_t)(sizeof f + (n + 1) * sizeof *q));
    R.find_done = n == 0;
    R.find_t0 = fpe_prof_begin();
    InvalidateRect(R.hwnd, NULL, FALSE);
}

/* One page's hits: merged in document order; the first hit found becomes the current one. */
static void find_found(const msg *mm)
{
    pdfp_found f;
    if (mm->head.size < sizeof f) return;
    memcpy(&f, mm->data, sizeof f);
    if (f.gen != R.find_gen || f.n > PDFP_FIND_PAGE_MAX || mm->head.size < sizeof f + f.n * sizeof(pdfp_hit) ||
        f.page >= R.npages)
        return;
    R.find_pages++;
    if (f.n) {
        if (R.find_n + f.n > R.find_cap) {
            uint32_t cap = R.find_cap ? R.find_cap : 64;
            while (cap < R.find_n + f.n) cap *= 2;
            fhit *h = realloc(R.hits, cap * sizeof *h);
            if (!h) return;
            R.hits = h;
            R.find_cap = cap;
        }
        uint32_t at = hit_lower(f.page, 0);
        memmove(R.hits + at + f.n, R.hits + at, (R.find_n - at) * sizeof *R.hits);
        for (uint32_t k = 0; k < f.n; k++) {
            pdfp_hit ph;
            memcpy(&ph, mm->data + sizeof f + k * sizeof ph, sizeof ph);
            fhit *h = &R.hits[at + k];
            h->page = f.page;
            h->index = ph.index;
            h->count = ph.count;
            h->l = ph.l, h->t = ph.t, h->r = ph.r, h->b = ph.b;
        }
        R.find_n += f.n;
        if (R.find_cur >= (int)at) {
            R.find_cur += (int)f.n;
        } else if (R.find_cur < 0) {
            R.find_cur = (int)at;
            find_show(R.find_cur);
        }
    }
    InvalidateRect(R.hwnd, NULL, FALSE);
}

/* Next (dir 1) or previous (-1) hit, around the end. */
static void find_step(int dir)
{
    wchar_t q[256] = L"";
    if (R.find_edit) GetWindowTextW(R.find_edit, q, 256);
    if (R.find_edit && wcscmp(q, R.find_q)) { /* typed, not searched yet: search now */
        find_restart();
        return;
    }
    if (!R.find_n) return;
    if (R.find_cur < 0) R.find_cur = dir > 0 ? 0 : (int)R.find_n - 1;
    else R.find_cur = (int)((R.find_cur + dir + (int)R.find_n) % (int)R.find_n);
    find_show(R.find_cur);
    InvalidateRect(R.hwnd, NULL, FALSE);
}

static void paint_hits(HDC dc, uint32_t i, int x, int y)
{
    uint32_t k = hit_lower(i, 0);
    if (k >= R.find_n || R.hits[k].page != i) return;
    ptext *t = text_want(i);
    for (; k < R.find_n && R.hits[k].page == i; k++) {
        const fhit *h = &R.hits[k];
        COLORREF c = (int)k == R.find_cur ? C_HIT_CUR : C_HIT;
        if (t) {
            paint_runs(dc, t, h->index, h->index + h->count, x, y, c);
        } else if (h->r > h->l) { /* the page's text is on its way: the hit's box meanwhile */
            int x0 = x + (int)floorf(h->l * R.scale), y0 = y + (int)floorf(h->t * R.scale);
            band(dc, x0, y0, x + (int)ceilf(h->r * R.scale) - x0, y + (int)ceilf(h->b * R.scale) - y0, c);
        }
    }
}

static void find_place(void)
{
    if (!R.find_edit) return;
    int w = app_px(260) < R.cw / 2 ? app_px(260) : R.cw / 2, h = app_px(22);
    MoveWindow(R.find_edit, pad(), (head_h() - h) / 2, w > 0 ? w : 0, h, TRUE);
}

static void find_close(void)
{
    if (!R.find_edit) return;
    HWND back = R.find_back;
    int had_focus = GetFocus() == R.find_edit;
    DestroyWindow(R.find_edit);
    R.find_edit = NULL;
    if (R.find_brush) DeleteObject(R.find_brush);
    R.find_brush = NULL;
    if (R.to && R.opened && R.find_q[0] && !R.find_done) {
        pdfp_find stop = { ++R.find_gen, 0, 0 };
        uint8_t buf[sizeof stop + 2] = { 0 };
        memcpy(buf, &stop, sizeof stop);
        send_req(PDFP_FIND, R.doc, 0, buf, sizeof buf);
    }
    R.find_q[0] = 0;
    hits_clear();
    if (had_focus) app_focus_window(back && IsWindowVisible(back) ? back : R.hwnd);
    InvalidateRect(R.hwnd, NULL, FALSE);
}

static LRESULT CALLBACK find_edit_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    int shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
    switch (m) {
    case WM_KEYDOWN:
        switch (w) {
        case VK_RETURN:
        case VK_F3: find_step(shift ? -1 : 1); return 0;
        case VK_DOWN: find_step(1); return 0;
        case VK_UP: find_step(-1); return 0;
        case VK_ESCAPE: find_close(); return 0;
        case VK_TAB: app_focus_window(R.hwnd); return 0;
        default: break;
        }
        if (ctrl && (w == 'A' || w == 'F')) {
            SendMessageW(h, EM_SETSEL, 0, -1);
            return 0;
        }
        break;
    case WM_CHAR:
        if (w == '\r' || w == 0x1B || w == '\t' || w == 0x7F || w == 0x01 || w == 0x06) return 0; /* no beeps */
        break;
    }
    return CallWindowProcW(R.find_orig, h, m, w, l);
}

/* ---- painting ------------------------------------------------------------------------------ */

static void fill(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    RECT r = { x, y, x + w, y + h };
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, GetStockObject(DC_BRUSH));
}

static void text(HDC dc, const wchar_t *s, int x, int y, int w, int h, COLORREF c, UINT fmt)
{
    if (w <= 0) return;
    RECT r = { x, y, x + w, y + h };
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, fmt | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

static void blit(HDC dc, const slot *s, int dx, int dy, int dw, int dh)
{
    BITMAPINFO bi = { { sizeof bi.bmiHeader, s->w, -s->h, 1, 32, BI_RGB } };
    const uint8_t *px = R.pixels + (size_t)(s - R.slots) * PDFP_SLOT_BYTES;
    if (dw == s->w && dh == s->h)
        SetDIBitsToDevice(dc, dx, dy, (DWORD)s->w, (DWORD)s->h, 0, 0, 0, (UINT)s->h, px, &bi, DIB_RGB_COLORS);
    else
        StretchDIBits(dc, dx, dy, dw, dh, 0, 0, s->w, s->h, px, &bi, DIB_RGB_COLORS, SRCCOPY);
}

/* Draws what there is of page i; sets R.page_dark to how its paper came out. */
static void paint_page(HDC dc, uint32_t i, int x, int y)
{
    int pw = page_w(i), ph = page_h(i);
    int dark = R.colors != PDFP_COLORS_ORIGINAL; /* a guess until a tile says */
    for (int k = 0; k < PDFP_SLOTS; k++)
        if (R.slots[k].state == S_READY && R.slots[k].page == i) {
            dark = R.slots[k].dark;
            break;
        }
    R.page_dark = dark;
    fill(dc, x, y, pw, ph, dark ? RGB(PDFP_DARK_LO, PDFP_DARK_LO, PDFP_DARK_LO) : RGB(236, 236, 236));
    /* what's there already, scaled: the small render, then tiles from another zoom */
    for (int pass = 0; pass < 3; pass++)
        for (int k = 0; k < PDFP_SLOTS; k++) {
            const slot *s = &R.slots[k];
            if (s->state != S_READY || s->page != i) continue;
            int thumb = s->tx == THUMB, current = !thumb && s->scale == R.scale;
            if ((pass == 0) != thumb || (pass == 2) != current) continue;
            double f = R.scale / s->scale;
            double ox = thumb ? 0 : s->tx * PDFP_TILE, oy = thumb ? 0 : s->ty * PDFP_TILE;
            int dx = x + (int)floor(ox * f), dy = y + (int)floor(oy * f);
            int dw = current ? s->w : (int)ceil((ox + s->w) * f) - (int)floor(ox * f);
            int dh = current ? s->h : (int)ceil((oy + s->h) * f) - (int)floor(oy * f);
            if (thumb) {
                dw = pw;
                dh = ph;
            }
            blit(dc, s, dx, dy, dw, dh);
        }
}

static void paint(HDC dc)
{
    int hh = head_h();
    HGDIOBJ old_font = SelectObject(dc, app_font());
    SetStretchBltMode(dc, HALFTONE);
    fill(dc, 0, 0, R.cw, R.ch, C_BG);

    if (R.opened && R.npages) {
        HRGN clip = CreateRectRgn(0, hh, R.cw, R.ch);
        SelectClipRgn(dc, clip);
        caret lo, hi;
        int sel = sel_bounds(&lo, &hi);
        uint32_t first = page_at_y(R.top);
        for (uint32_t i = first; i < R.npages && R.ys[i] < R.top + view_h(); i++) {
            int x = (int)floor(-R.left), y = hh + (int)floor(R.ys[i] - R.top);
            int64_t t = fpe_prof_begin();
            paint_page(dc, i, x, y);
            FPE_PROF_END("pdf.paint/page", t);
            if (R.find_n) {
                t = fpe_prof_begin();
                paint_hits(dc, i, x, y);
                FPE_PROF_END("pdf.paint/hits", t);
            }
            if (sel) {
                t = fpe_prof_begin();
                paint_selection(dc, i, x, y, &lo, &hi);
                FPE_PROF_END("pdf.paint/selection", t);
            }
        }
        SelectClipRgn(dc, NULL);
        DeleteObject(clip);
    } else if (R.path) {
        const wchar_t *msg = R.err == -1 ? L"The PDF helper could not start (folderwing-pdf.exe)"
                           : R.gone ? L"The PDF helper stopped: this file may be damaged"
                           : R.err == 4 ? L"This PDF is protected by a password"
                           : R.err ? L"This PDF could not be read"
                                   : L"";
        text(dc, msg, pad(), hh, R.cw - 2 * pad(), app_px(60), R.err || R.gone ? C_ERROR : C_DIM, DT_CENTER);
    }

    /* header: the name, where you are, how big */
    fill(dc, 0, 0, R.cw, hh, C_HEADER);
    wchar_t right[64] = L"";
    if (R.opened && R.npages && R.copying) {
        _snwprintf(right, 64, L"Copying page %u of %u...", R.copy_next - R.copy_lo.page + 1,
                   R.copy_hi.page - R.copy_lo.page + 1);
    } else if (R.opened && R.npages) {
        uint32_t cur = page_at_y(R.top + view_h() / 2.0) + 1;
        _snwprintf(right, 64, L"%u / %u     %.0f%%", cur, R.npages, R.scale / px_per_pt() * 100);
    }
    SIZE sz = { 0 };
    GetTextExtentPoint32W(dc, right, (int)wcslen(right), &sz);
    if (find_open()) {
        /* the find box, then how it went: "3 of 27", still searching, or nothing */
        RECT er;
        GetWindowRect(R.find_edit, &er);
        MapWindowPoints(NULL, R.hwnd, (POINT *)&er, 2);
        fill(dc, er.left - app_px(6), er.top - app_px(2), er.right - er.left + app_px(12), er.bottom - er.top + app_px(4),
             C_EDIT);
        wchar_t count[64] = L"";
        COLORREF cc = C_DIM;
        if (!R.find_q[0]) {
            wcscpy(count, L"Enter: next   Shift+Enter: previous");
        } else if (!R.find_n) {
            if (R.find_done) wcscpy(count, L"No matches"), cc = C_ERROR;
            else _snwprintf(count, 64, L"Searching... page %u of %u", R.find_pages, R.npages);
        } else {
            wchar_t cur[16] = L"-";
            if (R.find_cur >= 0) _snwprintf(cur, 16, L"%d", R.find_cur + 1);
            _snwprintf(count, 64, L"%ls of %u%ls", cur, R.find_n, R.find_done ? L"" : L"...");
            cc = C_TEXT;
        }
        count[63] = 0;
        int cx = er.right + app_px(14);
        text(dc, count, cx, 0, R.cw - 2 * pad() - sz.cx - cx, hh, cc, DT_LEFT);
    } else {
        const wchar_t *name = R.path ? wcsrchr(R.path, L'\\') : NULL;
        name = name ? name + 1 : R.path ? R.path : L"";
        text(dc, name, pad(), 0, R.cw - 3 * pad() - sz.cx, hh, C_TEXT, DT_LEFT);
    }
    text(dc, right, R.cw - pad() - sz.cx, 0, sz.cx, hh, C_DIM, DT_LEFT);
    SelectObject(dc, old_font);
}

/* ---- input ---------------------------------------------------------------------------------- */

static void scroll_by(double dy, double dx)
{
    R.top += dy;
    R.left += dx;
    view_changed();
}

static void back_to_fit(void)
{
    if (!R.opened) return;
    R.fit = 1;
    double frac = R.total > 0 ? R.top / R.total : 0;
    apply_fit();
    R.top = frac * R.total;
    view_changed();
}

static void key(WPARAM vk, int sys)
{
    int ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    double line = app_px(60), page = view_h() - app_px(40);
    if (!sys) {
        switch (vk) {
        case VK_UP: scroll_by(-line, 0); return;
        case VK_DOWN: scroll_by(line, 0); return;
        case VK_LEFT: scroll_by(0, -line); return;
        case VK_RIGHT: scroll_by(0, line); return;
        case VK_PRIOR: scroll_by(-page, 0); return;
        case VK_NEXT: scroll_by(page, 0); return;
        case VK_SPACE: scroll_by(shift ? -page : page, 0); return;
        case VK_HOME: if (ctrl || !shift) { R.top = 0; view_changed(); } return;
        case VK_END: if (ctrl || !shift) { R.top = R.total; view_changed(); } return;
        case VK_ESCAPE: /* the selection, then find, then the preview */
            if (sel_clear()) return;
            if (find_open()) find_close();
            else app_preview_close();
            return;
        case VK_F3:
            if (find_open()) {
                find_step(shift ? -1 : 1);
                return;
            }
            break;
        case VK_TAB: app_focus_cycle(shift ? -1 : 1); return;
        default: break;
        }
        /* with text selected, Ctrl+C copies it (Shift: lines as on the page); without, the keys are the app's */
        caret lo, hi;
        if (ctrl && (vk == 'C' || vk == VK_INSERT) && sel_state(&lo, &hi) != 0) {
            R.copy_request = app_clipboard_begin(R.hwnd);
            copy_start(shift);
            return;
        }
        if (ctrl && !shift && vk == 'A' && R.opened) {
            select_all();
            return;
        }
        if (ctrl && (vk == '0' || vk == VK_NUMPAD0)) {
            back_to_fit();
            return;
        }
        if (ctrl && (vk == VK_OEM_PLUS || vk == VK_ADD || vk == VK_OEM_MINUS || vk == VK_SUBTRACT)) {
            float f = vk == VK_OEM_PLUS || vk == VK_ADD ? 1.25f : 0.8f;
            zoom_at(R.scale * f, R.cw / 2, view_h() / 2);
            view_changed();
            return;
        }
    }
    /* anything else: the app's keys, acting on the pane the preview belongs to */
    fn_ctx c;
    app_preview_ctx(&c, R.hwnd, FN_FROM_KEY);
    uint8_t mods = (uint8_t)((ctrl ? FN_CTRL : 0) | (shift ? FN_SHIFT : 0) | (sys || GetKeyState(VK_MENU) < 0 ? FN_ALT : 0));
    fn_key((uint8_t)vk, mods, &c);
    app_ctx_free(&c);
}

static LRESULT CALLBACK reader_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_SIZE:
        R.cw = LOWORD(l);
        R.ch = HIWORD(l);
        find_place();
        if (R.opened && R.npages) {
            if (R.fit) {
                double frac = R.total > 0 ? R.top / R.total : 0;
                apply_fit();
                R.top = frac * R.total;
            }
            view_changed();
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        int64_t t = fpe_prof_begin();
        if (!R.bdc || R.bw != R.cw || R.bh != R.ch) {
            if (!R.bdc) R.bdc = CreateCompatibleDC(dc);
            if (R.bmp) {
                SelectObject(R.bdc, R.old_bmp);
                DeleteObject(R.bmp);
            }
            R.bmp = CreateCompatibleBitmap(dc, R.cw > 0 ? R.cw : 1, R.ch > 0 ? R.ch : 1);
            R.old_bmp = SelectObject(R.bdc, R.bmp);
            SetBkMode(R.bdc, TRANSPARENT);
            R.bw = R.cw;
            R.bh = R.ch;
        }
        paint(R.bdc);
        BitBlt(dc, 0, 0, R.cw, R.ch, R.bdc, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        FPE_PROF_END("pdf.paint", t);
        return 0;
    }

    case WM_PDF_MSG: {
        msg *mm = (msg *)l;
        if ((uint32_t)w != R.launch_gen) { free(mm); return 0; }
        int64_t t = fpe_prof_begin();
        if (mm->head.type == PDFP_OPENED && mm->head.doc == R.doc && mm->head.size >= sizeof(pdfp_opened)) {
            memcpy(&R.info, mm->data, sizeof R.info);
            uint32_t n = R.info.npages;
            if (n > PDFP_MAX_PAGES || mm->head.size < sizeof(pdfp_opened) + n * sizeof(pdfp_page)) n = 0;
            R.err = R.info.err;
            free(R.pages);
            free(R.ys);
            R.pages = malloc((n ? n : 1) * sizeof *R.pages);
            R.ys = malloc((n ? n : 1) * sizeof *R.ys);
            if (R.pages && R.ys && !R.err) {
                memcpy(R.pages, mm->data + sizeof(pdfp_opened), n * sizeof *R.pages);
                R.npages = n;
                R.opened = 1;
                R.fit = 1;
                apply_fit();
                R.top = 0;
                int64_t t0 = fpe_prof_begin();
                pos_restore(); /* the first lookup reads the file */
                FPE_PROF_END("pdf.positions_restore", t0);
                view_changed();
                if (find_open()) find_restart(); /* the same words, in the next PDF */
            }
            InvalidateRect(h, NULL, FALSE);
        } else if (mm->head.type == PDFP_TEXTED && mm->head.doc == R.doc && R.opened) {
            text_arrived(mm);
        } else if (mm->head.type == PDFP_FOUND && mm->head.doc == R.doc && R.opened) {
            find_found(mm);
        } else if (mm->head.type == PDFP_FIND_DONE && mm->head.doc == R.doc && mm->head.size >= sizeof(pdfp_found)) {
            pdfp_found f;
            memcpy(&f, mm->data, sizeof f);
            if (f.gen == R.find_gen) {
                R.find_done = 1;
                FPE_PROF_END("pdf.find_all", R.find_t0); /* asked -> every page searched (not UI time) */
                InvalidateRect(h, NULL, FALSE);
            }
        } else if (mm->head.type == PDFP_RENDERED && mm->head.doc == R.doc && mm->head.size >= sizeof(pdfp_rendered)) {
            const pdfp_rendered *r = (const pdfp_rendered *)mm->data;
            slot *s = r->slot < PDFP_SLOTS ? &R.slots[r->slot] : NULL;
            if (s && s->state == S_PENDING && s->ticket == mm->head.ticket) {
                s->state = r->err ? S_EMPTY : S_READY;
                s->dark = r->dark != 0;
                InvalidateRect(h, NULL, FALSE);
            }
        }
        free(mm);
        FPE_PROF_END("pdf.reply", t);
        return 0;
    }

    case WM_COMMAND:
        if ((HWND)l == R.find_edit && R.find_edit && HIWORD(w) == EN_CHANGE) SetTimer(h, TIMER_FIND, FIND_TYPING_MS, NULL);
        return 0;

    case WM_CTLCOLOREDIT:
        if ((HWND)l == R.find_edit) {
            if (!R.find_brush) R.find_brush = CreateSolidBrush(C_EDIT);
            SetTextColor((HDC)w, C_TEXT);
            SetBkColor((HDC)w, C_EDIT);
            return (LRESULT)R.find_brush;
        }
        break;

    case WM_PDF_LAUNCHED:
        if ((uint32_t)w == R.launch_gen && R.launcher) {
            preview_launch_take(&R.launcher, &R.process, &R.job, &R.pid);
            if (!R.process) { /* it could not start */
                helper_stop();
                text_drop_all();
                R.err = -1;
                R.gone = 1;
                R.opened = 0;
                InvalidateRect(h, NULL, FALSE);
            } else if (WaitForSingleObject(R.process, 0) == WAIT_OBJECT_0) {
                PostMessageW(h, WM_PDF_GONE, R.launch_gen, 0); /* it died before we knew it ran */
            }
        }
        return 0;

    case WM_PDF_WRITE_FAILED:
        if ((uint32_t)w == R.launch_gen) {
            helper_stop();
            text_drop_all();
            R.err = HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE);
            R.gone = 1;
            R.opened = 0;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;

    case WM_PDF_GONE:
        if ((uint32_t)w != R.launch_gen) return 0;
        /* Pipe EOF makes this transport unusable even if the child is still alive. */
        helper_stop();
        text_drop_all();
        R.gone = 1;
        R.opened = 0;
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_MOUSEWHEEL: {
        int ctrl = GET_KEYSTATE_WPARAM(w) & MK_CONTROL, shift = GET_KEYSTATE_WPARAM(w) & MK_SHIFT;
        double notches = GET_WHEEL_DELTA_WPARAM(w) / (double)WHEEL_DELTA;
        if (ctrl) {
            POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            ScreenToClient(h, &pt);
            zoom_at(R.scale * (float)pow(1.15, notches), pt.x, pt.y - head_h());
            view_changed();
        } else if (shift) {
            scroll_by(0, -notches * app_px(80));
        } else {
            scroll_by(-notches * app_px(80), 0);
        }
        return 0;
    }

    case WM_MOUSEHWHEEL:
        scroll_by(0, GET_WHEEL_DELTA_WPARAM(w) / (double)WHEEL_DELTA * app_px(80));
        return 0;

    case WM_LBUTTONDOWN: {
        int64_t t0 = fpe_prof_begin();
        app_focus_window(h);
        FPE_PROF_END("pdf.press/focus", t0);
        caret k;
        t0 = fpe_prof_begin();
        int on_page = GET_Y_LPARAM(l) >= head_h() && point_caret(GET_X_LPARAM(l), GET_Y_LPARAM(l), &k);
        FPE_PROF_END("pdf.press/hit", t0);
        if (!on_page) return 0;
        if (!(w & MK_SHIFT) || !R.sel) R.anchor = k; /* Shift+click extends */
        R.focus = k;
        R.sel = R.selecting = 1;
        SetCapture(h);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_RBUTTONDOWN:
        app_focus_window(h);
        return 0;

    case WM_LBUTTONDBLCLK:
        if (!select_word(GET_X_LPARAM(l), GET_Y_LPARAM(l))) back_to_fit(); /* off the text: fit again */
        return 0;

    case WM_LBUTTONUP:
        if (R.selecting) {
            R.selecting = 0;
            KillTimer(h, TIMER_EDGE);
            ReleaseCapture();
        }
        return 0;

    case WM_TIMER:
        if (w == TIMER_FIND) {
            find_restart();
            return 0;
        }
        if (w == TIMER_EDGE && R.selecting) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            int top = head_h(), d = pt.y < top ? pt.y - top : pt.y > R.ch ? pt.y - R.ch : 0;
            if (!d) {
                KillTimer(h, TIMER_EDGE);
                return 0;
            }
            scroll_by(d * 0.6, 0);
            point_caret(pt.x, pt.y, &R.focus);
        }
        return 0;

    case WM_MBUTTONDOWN:
        app_focus_window(h);
        SetCapture(h);
        R.panning = 1;
        R.pan_x = GET_X_LPARAM(l);
        R.pan_y = GET_Y_LPARAM(l);
        R.pan_top = R.top;
        R.pan_left = R.left;
        return 0;

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        if (R.panning) {
            R.top = R.pan_top - (y - R.pan_y);
            R.left = R.pan_left - (x - R.pan_x);
            view_changed();
        } else if (R.selecting) {
            point_caret(x, y, &R.focus);
            if (y < head_h() || y > R.ch) SetTimer(h, TIMER_EDGE, 30, NULL);
            InvalidateRect(h, NULL, FALSE);
        } else {
            caret k; /* the page under the pointer: its text, for the I-beam */
            if (y >= head_h()) point_caret(x, y, &k);
        }
        return 0;
    }

    case WM_MBUTTONUP:
    case WM_CAPTURECHANGED:
        if (R.panning) {
            R.panning = 0;
            if (m == WM_MBUTTONUP) ReleaseCapture();
        }
        if (m == WM_CAPTURECHANGED && R.selecting) {
            R.selecting = 0;
            KillTimer(h, TIMER_EDGE);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            LPCWSTR cur = IDC_ARROW;
            POINT pt;
            caret k;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            if (R.panning) {
                cur = IDC_SIZEALL;
            } else if (R.selecting) {
                cur = IDC_IBEAM;
            } else if (pt.y >= head_h() && point_caret(pt.x, pt.y, &k)) {
                ptext *t = text_find(k.page);
                if (t && t->state == S_READY && char_under(t, k.x, k.y) >= 0) cur = IDC_IBEAM;
            }
            SetCursor(LoadCursorW(NULL, cur));
            return TRUE;
        }
        break;

    case WM_KEYDOWN:
        key(w, 0);
        return 0;

    case WM_SYSKEYDOWN:
        if (w == VK_F4) break; /* Alt+F4 closes the window */
        key(w, 1);
        return 0;

    case WM_CHAR:
    case WM_SYSCHAR:
        return 0;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ---- public ------------------------------------------------------------------------------------ */

int pdfview_start(HWND parent)
{
    if (R.hwnd) return 0;
    static int registered;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSEXW wc = { sizeof wc };
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = reader_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = L"OpenPilotPdf";
        registered = RegisterClassExW(&wc) != 0;
    }
    R.hwnd = CreateWindowExW(0, L"OpenPilotPdf", L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 0, 0, parent, NULL, inst,
                             NULL);
    return R.hwnd ? 0 : -1;
}

/* Where the open document was left. A document only glanced at (fitted,
 * at the top) isn't added: browsing a folder of PDFs would push out the
 * ones being read. One already remembered is updated, also back to the top. */
static void pos_record(void)
{
    if (!R.opened || !R.npages || !R.path || !(R.scale > 0)) return;
    uint32_t page = page_at_y(R.top);
    double y = (R.top - R.ys[page]) / R.scale;
    if (y < 0) y = 0;
    int glance = page == 0 && y < 1 && R.fit;
    if (glance && !pdfpos_has(R.path)) return;
    pdfpos_set(R.path, R.info.size, R.mtime, page, (float)y, R.fit ? 0 : R.scale / px_per_pt(),
               R.fit ? 0 : (float)(R.left / R.scale));
}

static void pos_restore(void)
{
    const pdfpos *e = pdfpos_get(R.path, R.info.size, R.mtime);
    if (!e || e->page >= R.npages) return;
    if (e->zoom > 0) {
        float s = e->zoom * px_per_pt();
        R.scale = s < min_scale() ? min_scale() : s > max_scale() ? max_scale() : s;
        R.fit = 0;
        relayout();
        R.left = e->x * R.scale;
    }
    R.top = R.ys[e->page] + e->y * R.scale;
}

void pdfview_set_memory(const wchar_t *file, uint32_t max) { pdfpos_open(file, max); }

void pdfview_clear(void)
{
    pos_record();
    text_drop_all();
    hits_clear(); /* the find box stays; its words are searched again in the next document */
    R.find_gen++;
    R.find_q[0] = 0;
    if (R.to && R.opened) send_req(PDFP_CLOSE, R.doc, 0, NULL, 0);
    R.doc++; /* replies for the old document are ignored */
    free(R.path);
    free(R.pages);
    free(R.ys);
    R.path = NULL;
    R.size = 0;
    R.mtime = 0;
    R.pages = NULL;
    R.ys = NULL;
    R.npages = 0;
    R.opened = 0;
    R.err = 0;
    R.gone = 0;
    for (int i = 0; i < PDFP_SLOTS; i++) R.slots[i].state = S_EMPTY;
    if (R.hwnd) InvalidateRect(R.hwnd, NULL, FALSE);
}

void pdfview_stop(void)
{
    pdfview_clear();
    int64_t t = fpe_prof_begin();
    pdfpos_close(); /* written once, as the reader goes */
    FPE_PROF_END("pdf.positions_save", t);
    helper_stop();
    if (R.hwnd) DestroyWindow(R.hwnd); /* and the find box with it */
    if (R.find_brush) DeleteObject(R.find_brush);
    if (R.bdc) {
        SelectObject(R.bdc, R.old_bmp);
        DeleteObject(R.bmp);
        DeleteDC(R.bdc);
    }
    memset(&R, 0, sizeof R);
}

void pdfview_set_colors(uint32_t colors)
{
    if (colors > PDFP_COLORS_DARK || colors == R.colors) return;
    R.colors = colors;
    /* every tile again, in the new colors; replies to the old requests no longer match a pending slot */
    for (int i = 0; i < PDFP_SLOTS; i++) R.slots[i].state = S_EMPTY;
    if (R.hwnd) view_changed();
}

uint32_t pdfview_colors(void) { return R.colors; }

void pdfview_find(void)
{
    if (!R.hwnd || !IsWindowVisible(R.hwnd)) return;
    if (!R.find_edit) {
        HWND f = GetFocus();
        R.find_back = f && f != R.hwnd && !IsChild(R.hwnd, f) ? f : NULL;
        R.find_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, R.hwnd, NULL,
                                      GetModuleHandleW(NULL), NULL);
        if (!R.find_edit) return;
        SendMessageW(R.find_edit, WM_SETFONT, (WPARAM)app_font(), FALSE);
        SendMessageW(R.find_edit, EM_SETLIMITTEXT, 255, 0);
        R.find_orig = (WNDPROC)SetWindowLongPtrW(R.find_edit, GWLP_WNDPROC, (LONG_PTR)find_edit_proc);
        find_place();
        hits_clear();
        R.find_q[0] = 0;
    }
    app_focus_window(R.find_edit);
    SendMessageW(R.find_edit, EM_SETSEL, 0, -1);
    InvalidateRect(R.hwnd, NULL, FALSE);
}

void pdfview_find_step(int dir)
{
    if (!R.hwnd || !IsWindowVisible(R.hwnd)) return;
    if (!find_open() || !R.find_q[0]) pdfview_find();
    else find_step(dir);
}

HWND pdfview_window(void) { return R.hwnd; }
const wchar_t *pdfview_path(void) { return R.path; }
int pdfview_helper_pid(void) { return R.process ? (int)R.pid : 0; }

void pdfview_open(const wchar_t *path, uint64_t size, int64_t mtime)
{
    if (!R.hwnd || !path) return;
    if (R.path && fpe_path_equal(R.path, path) && R.size == size && R.mtime == mtime && !R.gone) return;
    FPE_PROF_COUNT("pdf.document_open");
    pdfview_clear();
    R.path = _wcsdup(path);
    if (!R.path) return;
    R.size = size;
    R.mtime = mtime;
    if(fpe_wsl_requires_native(path)){R.err=HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);InvalidateRect(R.hwnd,NULL,FALSE);return;}
    if (helper_start()) {
        R.err = -1;
        InvalidateRect(R.hwnd, NULL, FALSE);
        return;
    }
    send_req(PDFP_OPEN, R.doc, 0, path, (uint32_t)((wcslen(path) + 1) * sizeof(wchar_t)));
}
