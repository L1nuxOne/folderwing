/* fppdf - the PDF helper: PDFium in its own process (see pdfproto.h).
 *
 * Started by the app as `folderwing-pdf.exe <section-handle>` with pipes for stdin
 * and stdout; it never shows a window. One thread reads requests into a
 * queue, the main thread does all PDFium work (PDFium is single-threaded).
 * Renders from an older epoch than the newest one queued are skipped.
 *
 * The document is read through a handle opened with full sharing, so a
 * previewed file can still be renamed, moved or replaced. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "fpdf_edit.h"
#include "fpdf_text.h"
#include "fpdfview.h"
#include "pdfproto.h"

typedef struct msg {
    struct msg *next;
    pdfp_head   head;
    uint8_t     data[];
} msg;

static struct {
    HANDLE           in, out, wake;
    uint8_t         *slots;
    CRITICAL_SECTION lock;
    msg             *first, *last;
    volatile LONG    latest_epoch;
    volatile LONG    eof;
} Q;

static struct {
    FPDF_DOCUMENT doc;
    HANDLE        file;
    uint64_t      size;
    uint32_t      id, npages;
    FPDF_FILEACCESS access;
    struct {
        FPDF_PAGE page;
        uint32_t  index, used;
    } pages[4];
    uint32_t clock;
} D;

/* ---- pipe -------------------------------------------------------------------- */

static int read_all(void *buf, DWORD n)
{
    uint8_t *p = buf;
    while (n) {
        DWORD got = 0;
        if (!ReadFile(Q.in, p, n, &got, NULL) || !got) return -1;
        p += got;
        n -= got;
    }
    return 0;
}

static void send_msg(uint32_t type, uint32_t doc, uint32_t ticket, const void *a, uint32_t na, const void *b,
                     uint32_t nb)
{
    pdfp_head h = { type, na + nb, doc, ticket };
    uint8_t *buf = malloc(sizeof h + na + nb);
    if (!buf) return;
    memcpy(buf, &h, sizeof h);
    if (na) memcpy(buf + sizeof h, a, na);
    if (nb) memcpy(buf + sizeof h + na, b, nb);
    DWORD w;
    WriteFile(Q.out, buf, (DWORD)(sizeof h + na + nb), &w, NULL);
    free(buf);
}

static void push(msg *m)
{
    EnterCriticalSection(&Q.lock);
    if (Q.last) Q.last->next = m;
    else Q.first = m;
    Q.last = m;
    LeaveCriticalSection(&Q.lock);
    SetEvent(Q.wake);
}

/* The next request. Page text goes ahead of queued renders: someone is
 * selecting or copying and waits for it, while a screenful of tiles can
 * take a few hundred milliseconds. Nothing passes an OPEN, CLOSE, FIND or
 * QUIT, so text is never answered for the wrong document. */
static msg *pop(void)
{
    EnterCriticalSection(&Q.lock);
    msg *m = Q.first, *prev = NULL;
    for (msg *k = Q.first, *kp = NULL; k; kp = k, k = k->next) {
        if (k->head.type == PDFP_TEXT) {
            m = k, prev = kp;
            break;
        }
        if (k->head.type != PDFP_RENDER) break;
    }
    if (m) {
        if (prev) prev->next = m->next;
        else Q.first = m->next;
        if (Q.last == m) Q.last = prev;
        m->next = NULL;
    }
    LeaveCriticalSection(&Q.lock);
    return m;
}

static DWORD WINAPI reader(void *arg)
{
    for (;;) {
        pdfp_head h;
        if (read_all(&h, sizeof h) || h.size > PDFP_PATH_MAX * 2 + 64) break;
        msg *m = calloc(1, sizeof *m + h.size + 2);
        if (!m || read_all(m->data, h.size)) {
            free(m);
            break;
        }
        m->head = h;
#ifdef F15_FAULT_TEST
        /* The isolated transport test uses real PDFium for healthy opens, but
         * holds the process alive after EOF on these two fixture filenames. */
        if (h.type == PDFP_OPEN && h.size >= sizeof(wchar_t) && !(h.size % sizeof(wchar_t))) {
            const wchar_t *path = (const wchar_t *)m->data;
            const wchar_t *name = wcsrchr(path, L'\\');
            name = name ? name + 1 : path;
            int partial = !_wcsicmp(name, L"f15-partial.pdf");
            if (partial || !_wcsicmp(name, L"f15-eof.pdf")) {
                Sleep(800); /* lets the fixture observe this still-live helper */
                if (partial) {
                    pdfp_head broken = {PDFP_OPENED, sizeof(pdfp_opened), h.doc, 0};
                    DWORD wrote = 0;
                    WriteFile(Q.out, &broken, sizeof broken / 2, &wrote, NULL);
                }
                CloseHandle(Q.out);
                Q.out = NULL;
                Sleep(2500); /* EOF precedes process exit by a controlled interval */
                ExitProcess(0);
            }
        }
#endif
        if (h.type == PDFP_RENDER && h.size >= sizeof(pdfp_render)) {
            LONG e = (LONG)((pdfp_render *)m->data)->epoch;
            if (e > Q.latest_epoch) InterlockedExchange(&Q.latest_epoch, e);
        }
        push(m);
    }
    InterlockedExchange(&Q.eof, 1); /* the app went away */
    SetEvent(Q.wake);
    return 0;
}

/* ---- document ------------------------------------------------------------------- */

static int get_block(void *param, unsigned long pos, unsigned char *buf, unsigned long size)
{
    OVERLAPPED ov = { 0 };
    ov.Offset = (DWORD)pos;
    DWORD got = 0;
    return ReadFile(D.file, buf, (DWORD)size, &got, &ov) && got == size;
}

/* What dark mode needs to know about a page: where its pictures are (in
 * points from the displayed page's top-left) and whether it is mostly
 * pictures. Kept for a few pages; tiles of one page ask many times. */
#define LOOK_RECTS 32
typedef struct look {
    int      valid, pictures; /* pictures: mostly a picture (a scan): mixed keeps it as it is */
    uint32_t page, used, n;
    float    r[LOOK_RECTS][4]; /* l, t, r, b */
} look;
static look L[8];
static uint32_t look_clock;

/* The search in progress (see PDFP_FIND): one page per step, between requests. */
static struct {
    int            active;
    uint32_t       gen, doc, flags, from, done, total;
    unsigned short query[1024]; /* UTF-16, NUL-terminated (FPDF_WIDESTRING) */
} S;

static void close_doc(void)
{
    S.active = 0;
    memset(L, 0, sizeof L);
    for (int i = 0; i < 4; i++) {
        if (D.pages[i].page) FPDF_ClosePage(D.pages[i].page);
        D.pages[i].page = NULL;
    }
    if (D.doc) FPDF_CloseDocument(D.doc);
    if (D.file && D.file != INVALID_HANDLE_VALUE) CloseHandle(D.file);
    D.doc = NULL;
    D.file = NULL;
    D.npages = 0;
}

static FPDF_PAGE page_at(uint32_t index)
{
    int free_at = 0;
    for (int i = 0; i < 4; i++) {
        if (D.pages[i].page && D.pages[i].index == index) {
            D.pages[i].used = ++D.clock;
            return D.pages[i].page;
        }
        if (D.pages[i].used < D.pages[free_at].used || !D.pages[i].page) free_at = i;
    }
    if (D.pages[free_at].page) FPDF_ClosePage(D.pages[free_at].page);
    D.pages[free_at].page = FPDF_LoadPage(D.doc, (int)index);
    D.pages[free_at].index = index;
    D.pages[free_at].used = ++D.clock;
    return D.pages[free_at].page;
}

/* Page space (PDF units, y up, any origin, any /Rotate) to the space tiles
 * are drawn in: points from the displayed page's top-left. Taken from
 * PDFium's own display matrix, so boxes line up with renders. */
typedef struct xform {
    double a, b, c, d, e, f; /* X = a x + c y + e, Y = b x + d y + f */
} xform;

static xform page_xform(FPDF_PAGE page)
{
    enum { K = 64, U = 1000 }; /* device units per point; a long unit vector, for precision */
    int sx = (int)(FPDF_GetPageWidthF(page) * K + 0.5f), sy = (int)(FPDF_GetPageHeightF(page) * K + 0.5f);
    int ox = 0, oy = 0, xx = 0, xy = 0, yx = 0, yy = 0;
    FPDF_PageToDevice(page, 0, 0, sx, sy, 0, 0, 0, &ox, &oy);
    FPDF_PageToDevice(page, 0, 0, sx, sy, 0, U, 0, &xx, &xy);
    FPDF_PageToDevice(page, 0, 0, sx, sy, 0, 0, U, &yx, &yy);
    const double s = 1.0 / ((double)U * K);
    xform m = { (xx - ox) * s, (xy - oy) * s, (yx - ox) * s, (yy - oy) * s, (double)ox / K, (double)oy / K };
    return m;
}

/* A page-space box as top-left, top, right, bottom (pages turn in quarter turns: two corners do). */
static void map_box(const xform *m, double l, double b, double r, double t, float out[4])
{
    double x0 = m->a * l + m->c * b + m->e, y0 = m->b * l + m->d * b + m->f;
    double x1 = m->a * r + m->c * t + m->e, y1 = m->b * r + m->d * t + m->f;
    out[0] = (float)(x0 < x1 ? x0 : x1);
    out[1] = (float)(y0 < y1 ? y0 : y1);
    out[2] = (float)(x0 < x1 ? x1 : x0);
    out[3] = (float)(y0 < y1 ? y1 : y0);
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static float pct(float *v, uint32_t n, int p) /* p-th percentile of v (sorts it) */
{
    qsort(v, n, sizeof *v, cmp_float);
    uint32_t i = (uint32_t)((uint64_t)(n - 1) * (uint32_t)p / 100);
    return v[i];
}

#define FIT_SAMPLES 4000
#define FIT_LINES   2000
#define FIT_PAGES   5

/* Where the body text sits, from up to FIT_PAGES pages spread over the
 * document (the title page is no guide; a run of middle pages may be an
 * appendix of code or tables). Sideways it is measured by lines, not
 * characters, and the outermost line ends count: text must never be cut
 * off, and ragged text (code) has few long lines. Only lines lying wholly
 * beside the body - a margin note, a page number out in the margin - are
 * left out. */
static int fit_text(pdfp_opened *o, const pdfp_page *pages)
{
    static float y0[FIT_SAMPLES], y1[FIT_SAMPLES], ls[FIT_LINES], le[FIT_LINES], tmp[FIT_LINES];
    uint32_t n = 0, nl = 0, used = 0, first = 0;
    uint32_t np = D.npages, skip = np > 1; /* page 0 only when it is all there is */
    uint32_t span = np - skip, want = span < FIT_PAGES ? span : FIT_PAGES;
    for (uint32_t k = 0; k < want && nl < FIT_LINES; k++) {
        uint32_t pi = skip + (uint32_t)(((uint64_t)span * (2 * k + 1)) / (2 * want)); /* the middle of each slice */
        FPDF_PAGE page = page_at(pi);
        FPDF_TEXTPAGE tp = page ? FPDFText_LoadPage(page) : NULL;
        if (!tp) continue;
        int count = FPDFText_CountChars(tp), chars = 0;
        uint32_t start = n, lstart = nl;
        xform xf = page_xform(page);
        /* the line being read: its extent, vertical band (top-left space: lt < lb) and length */
        double lx0 = 0, lx1 = 0, lb = 0, lt = 0, pr = 0;
        int lchars = 0;
        for (int i = 0; i <= count; i++) {
            /* loose boxes: the font's full height, the same for every glyph on a line */
            double l = 0, r = 0, b = 0, t = 0;
            int ok = 0;
            if (i < count) {
                unsigned int ch = FPDFText_GetUnicode(tp, i);
                FS_RECTF box;
                ok = ch > ' ' && FPDFText_IsGenerated(tp, i) != 1 &&
                     FPDFText_GetLooseCharBox(tp, i, &box) && box.right > box.left && box.top > box.bottom;
                if (!ok) continue;
                float q[4];
                map_box(&xf, box.left, box.bottom, box.right, box.top, q);
                l = q[0], t = q[1], r = q[2], b = q[3];
            }
            /* a new line: the end of the page, or a character outside the
             * line's band, or one that starts well back to the left */
            double cy = (b + t) / 2, h = lb - lt;
            if (lchars && (!ok || cy < lt || cy > lb || l < pr - h * 2)) {
                if (lchars >= 4 && nl < FIT_LINES) {
                    ls[nl] = (float)lx0;
                    le[nl] = (float)lx1;
                    nl++;
                }
                lchars = 0;
            }
            if (!ok) break;
            if (!lchars) {
                lx0 = l, lx1 = r, lb = b, lt = t;
            } else {
                if (l < lx0) lx0 = l;
                if (r > lx1) lx1 = r;
            }
            pr = r;
            lchars++;
            chars++;
            if (n < FIT_SAMPLES) {
                y0[n] = (float)t;
                y1[n] = (float)b;
                n++;
            }
        }
        FPDFText_ClosePage(tp);
        if (chars < 100 || nl - lstart < 5) {
            n = start; /* too little text to say where the body is */
            nl = lstart;
            continue;
        }
        if (!used++) first = pi;
    }
    if (!used) return -1;
    /* the body's core, then everything that overlaps it */
    memcpy(tmp, ls, nl * sizeof *tmp);
    float core0 = pct(tmp, nl, 5);
    memcpy(tmp, le, nl * sizeof *tmp);
    float core1 = pct(tmp, nl, 95);
    float slack = (core1 - core0) * 0.02f, a = core0, b = core1;
    for (uint32_t i = 0; i < nl; i++) {
        if (ls[i] >= core1 - slack || le[i] <= core0 + slack) continue; /* beside the body */
        if (ls[i] < a) a = ls[i];
        if (le[i] > b) b = le[i];
    }
    float c = pct(y0, n, 1), d = pct(y1, n, 99);
    if (b - a < pages[first].w * 0.25f) return -1;
    o->fit_kind = PDFP_FIT_TEXT;
    o->fit_page = first;
    o->fit_x0 = a;
    o->fit_x1 = b;
    o->fit_y0 = c;
    o->fit_y1 = d;
    return 0;
}

/* Scans and other image-only pages: where the ink is, on a small render. */
static int fit_ink(pdfp_opened *o, const pdfp_page *pages)
{
    uint32_t pi = D.npages / 2;
    FPDF_PAGE page = page_at(pi);
    if (!page || pages[pi].w < 1) return -1;
    float s = 300.0f / pages[pi].w;
    int w = 300, h = (int)(pages[pi].h * s);
    if (h < 1 || h > 3000) return -1;
    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px) return -1;
    FPDF_BITMAP bmp = FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRx, px, w * 4);
    FPDFBitmap_FillRect(bmp, 0, 0, w, h, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bmp, page, 0, 0, w, h, 0, 0);
    FPDFBitmap_Destroy(bmp);
    static float xs[FIT_SAMPLES], ys[FIT_SAMPLES];
    uint32_t n = 0, dark = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = px[y * w + x];
            int lum = ((c >> 16 & 255) * 3 + (c >> 8 & 255) * 6 + (c & 255)) / 10;
            if (lum >= 160) continue;
            dark++;
            if (n < FIT_SAMPLES && (dark % 4 == 0 || n < 64)) {
                xs[n] = (float)x;
                ys[n] = (float)y;
                n++;
            }
        }
    free(px);
    if (n < 64 || dark > (uint32_t)(w * h) / 2) return -1; /* blank, or a photo */
    float a = pct(xs, n, 1) / s, b = (pct(xs, n, 99) + 1) / s, c = pct(ys, n, 1) / s, d = (pct(ys, n, 99) + 1) / s;
    if (b - a < pages[pi].w * 0.25f) return -1;
    o->fit_kind = PDFP_FIT_INK;
    o->fit_page = pi;
    o->fit_x0 = a;
    o->fit_x1 = b;
    o->fit_y0 = c;
    o->fit_y1 = d;
    return 0;
}

static void open_doc(const msg *m)
{
    close_doc();
    D.id = m->head.doc;
    pdfp_opened o = { 0 };
    const wchar_t *path = (const wchar_t *)m->data;
    D.file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
    LARGE_INTEGER size;
    if (D.file == INVALID_HANDLE_VALUE || !GetFileSizeEx(D.file, &size) || size.QuadPart > 0x7FFFFFFF) {
        o.err = 2; /* FPDF_ERR_FILE */
        send_msg(PDFP_OPENED, m->head.doc, 0, &o, sizeof o, NULL, 0);
        close_doc();
        return;
    }
    D.access.m_FileLen = (unsigned long)size.QuadPart;
    o.size = (uint64_t)size.QuadPart;
    D.access.m_GetBlock = get_block;
    D.access.m_Param = NULL;
    D.doc = FPDF_LoadCustomDocument(&D.access, NULL);
    if (!D.doc) {
        o.err = (int32_t)FPDF_GetLastError();
        if (!o.err) o.err = 3;
        send_msg(PDFP_OPENED, m->head.doc, 0, &o, sizeof o, NULL, 0);
        close_doc();
        return;
    }
    int count = FPDF_GetPageCount(D.doc);
    D.npages = count < 0 ? 0 : count > PDFP_MAX_PAGES ? PDFP_MAX_PAGES : (uint32_t)count;
    if (m->head.type == PDFP_THUMB_OPEN && D.npages) D.npages = 1;
    pdfp_page *pages = calloc(D.npages ? D.npages : 1, sizeof *pages);
    if (!pages) {
        o.err = 1;
        send_msg(PDFP_OPENED, m->head.doc, 0, &o, sizeof o, NULL, 0);
        close_doc();
        return;
    }
    for (uint32_t i = 0; i < D.npages; i++) {
        FS_SIZEF sz = { 612, 792 };
        FPDF_GetPageSizeByIndexF(D.doc, (int)i, &sz);
        pages[i].w = sz.width > 1 ? sz.width : 612;
        pages[i].h = sz.height > 1 ? sz.height : 792;
    }
    o.npages = D.npages;
    if (D.npages && (m->head.type == PDFP_THUMB_OPEN || (fit_text(&o, pages) && fit_ink(&o, pages)))) {
        o.fit_kind = PDFP_FIT_PAGE;
        o.fit_x1 = pages[0].w;
        o.fit_y1 = pages[0].h;
    }
    send_msg(PDFP_OPENED, m->head.doc, 0, &o, sizeof o, pages, D.npages * (uint32_t)sizeof *pages);
    free(pages);
}

/* ---- dark mode ------------------------------------------------------------------- */

/* Collects the pictures among objs (a page's, or a form's under the matrix
 * m to page space), as page-space bounds. Forms nest: scanners often wrap
 * the page image in one. */
static void collect_pictures(FPDF_PAGEOBJECT form, FPDF_PAGE page, const FS_MATRIX *m, int depth, look *lk,
                             const xform *xf, double *area)
{
    int n = form ? FPDFFormObj_CountObjects(form) : FPDFPage_CountObjects(page);
    for (int i = 0; i < n; i++) {
        FPDF_PAGEOBJECT o = form ? FPDFFormObj_GetObject(form, (unsigned long)i) : FPDFPage_GetObject(page, i);
        int type = o ? FPDFPageObj_GetType(o) : 0;
        if (type == FPDF_PAGEOBJ_FORM && depth < 4) {
            FS_MATRIX f, c;
            if (!FPDFPageObj_GetMatrix(o, &f)) continue;
            /* child -> page: first the form's own matrix, then m */
            c.a = f.a * m->a + f.b * m->c, c.b = f.a * m->b + f.b * m->d;
            c.c = f.c * m->a + f.d * m->c, c.d = f.c * m->b + f.d * m->d;
            c.e = f.e * m->a + f.f * m->c + m->e, c.f = f.e * m->b + f.f * m->d + m->f;
            collect_pictures(o, page, &c, depth + 1, lk, xf, area);
            continue;
        }
        if (type != FPDF_PAGEOBJ_IMAGE && type != FPDF_PAGEOBJ_SHADING) continue;
        float l, b, r, t;
        if (!FPDFPageObj_GetBounds(o, &l, &b, &r, &t) || r <= l || t <= b) continue;
        /* the corners to page space, then to the display's points */
        float x[4] = { l, r, l, r }, y[4] = { b, b, t, t }, q[4] = { 1e9f, 1e9f, -1e9f, -1e9f };
        for (int k = 0; k < 4; k++) {
            double px = m->a * x[k] + m->c * y[k] + m->e, py = m->b * x[k] + m->d * y[k] + m->f;
            float d[4];
            map_box(xf, px, py, px, py, d);
            if (d[0] < q[0]) q[0] = d[0];
            if (d[1] < q[1]) q[1] = d[1];
            if (d[2] > q[2]) q[2] = d[2];
            if (d[3] > q[3]) q[3] = d[3];
        }
        *area += (double)(q[2] - q[0]) * (q[3] - q[1]);
        if (lk->n < LOOK_RECTS) memcpy(lk->r[lk->n++], q, sizeof q);
    }
}

static const look *look_at(uint32_t index, FPDF_PAGE page)
{
    look *lk = &L[0];
    for (int i = 0; i < 8; i++) {
        if (L[i].valid && L[i].page == index) {
            L[i].used = ++look_clock;
            return &L[i];
        }
        if (!L[i].valid || L[i].used < lk->used) lk = &L[i];
    }
    memset(lk, 0, sizeof *lk);
    lk->page = index;
    lk->used = ++look_clock;
    lk->valid = 1;
    xform xf = page_xform(page);
    FS_MATRIX id = { 1, 0, 0, 1, 0, 0 };
    double area = 0, page_area = (double)FPDF_GetPageWidthF(page) * FPDF_GetPageHeightF(page);
    collect_pictures(NULL, page, &id, 0, lk, &xf, &area);
    lk->pictures = page_area > 0 && area >= page_area * 0.5;
    if (lk->n == LOOK_RECTS) lk->pictures = 1; /* too many to keep apart: treat it as a picture page */
    return lk;
}

/* Lightness inverted with hue kept, then narrowed to LO..HI. In HSL terms
 * L -> 1 - L at the same hue and saturation, which is c + 255 - max - min
 * for each channel. */
static void darken_row(uint32_t *px, int n)
{
    static uint8_t map[256];
    if (!map[255]) {
        for (int i = 0; i < 256; i++) map[i] = (uint8_t)(PDFP_DARK_LO + i * (PDFP_DARK_HI - PDFP_DARK_LO) / 255);
    }
    for (int i = 0; i < n; i++) {
        uint32_t c = px[i];
        int b = c & 255, g = c >> 8 & 255, r = c >> 16 & 255;
        int mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        int s = 255 - mx - mn;
        px[i] = (uint32_t)map[b + s] | (uint32_t)map[g + s] << 8 | (uint32_t)map[r + s] << 16;
    }
}

/* Darkens a rendered tile, leaving the picture rects (mixed) as they are. */
static void darken_tile(uint32_t *px, const pdfp_render *r, const look *keep)
{
    uint8_t mask[PDFP_TILE];
    int w = (int)r->w;
    for (int y = 0; y < (int)r->h; y++) {
        uint32_t *row = px + (size_t)y * w;
        int any = 0;
        if (keep) {
            double py = (r->y0 + y + 0.5) / r->scale;
            memset(mask, 0, (size_t)w);
            for (uint32_t k = 0; k < keep->n; k++) {
                const float *q = keep->r[k];
                if (py < q[1] || py >= q[3]) continue;
                int a = (int)floor(q[0] * r->scale) - r->x0, b = (int)ceil(q[2] * r->scale) - r->x0;
                if (a < 0) a = 0;
                if (b > w) b = w;
                if (a < b) memset(mask + a, 1, (size_t)(b - a)), any = 1;
            }
        }
        if (!any) {
            darken_row(row, w);
            continue;
        }
        for (int x = 0; x < w;) { /* runs outside the pictures */
            int e = x;
            while (e < w && !mask[e]) e++;
            darken_row(row + x, e - x);
            while (e < w && mask[e]) e++;
            x = e;
        }
    }
}

static void render(const msg *m)
{
    const pdfp_render *r = (const pdfp_render *)m->data;
    pdfp_rendered out = { r->slot, -1, 0 };
    if (m->head.size < sizeof *r || r->slot >= PDFP_SLOTS) return;
    if (!D.doc || m->head.doc != D.id || r->page >= D.npages || !r->w || !r->h || r->w > PDFP_TILE ||
        r->h > PDFP_TILE || !(r->scale > 0)) {
        send_msg(PDFP_RENDERED, m->head.doc, m->head.ticket, &out, sizeof out, NULL, 0);
        return;
    }
    FPDF_PAGE page = page_at(r->page);
    if (page) {
        uint8_t *px = Q.slots + (size_t)r->slot * PDFP_SLOT_BYTES;
        FPDF_BITMAP bmp = FPDFBitmap_CreateEx((int)r->w, (int)r->h, FPDFBitmap_BGRx, px, (int)r->w * 4);
        if (bmp) {
            FPDFBitmap_FillRect(bmp, 0, 0, (int)r->w, (int)r->h, 0xFFFFFFFF);
            FS_MATRIX mx = { r->scale, 0, 0, r->scale, (float)-r->x0, (float)-r->y0 };
            FS_RECTF clip = { 0, 0, (float)r->w, (float)r->h };
            FPDF_RenderPageBitmapWithMatrix(bmp, page, &mx, &clip, FPDF_ANNOT);
            FPDFBitmap_Destroy(bmp);
            out.err = 0;
            if (r->colors == PDFP_COLORS_DARK) {
                darken_tile((uint32_t *)px, r, NULL);
                out.dark = 1;
            } else if (r->colors == PDFP_COLORS_MIXED) {
                const look *lk = look_at(r->page, page);
                if (!lk->pictures) {
                    darken_tile((uint32_t *)px, r, lk);
                    out.dark = 1;
                }
            }
        }
    }
    send_msg(PDFP_RENDERED, m->head.doc, m->head.ticket, &out, sizeof out, NULL, 0);
}

/* A page's characters with their cells, for selection and copy. A page
 * that can't be read answers with no characters. */
static void text_page(const msg *m)
{
    pdfp_text head = { 0, 0 };
    if (m->head.size >= sizeof head.page) memcpy(&head.page, m->data, sizeof head.page);
    pdfp_char *out = NULL;
    FPDF_PAGE page = D.doc && m->head.doc == D.id && head.page < D.npages ? page_at(head.page) : NULL;
    FPDF_TEXTPAGE tp = page ? FPDFText_LoadPage(page) : NULL;
    if (tp) {
        int count = FPDFText_CountChars(tp);
        if (count > PDFP_TEXT_MAX) count = PDFP_TEXT_MAX;
        out = count > 0 ? malloc((size_t)count * sizeof *out) : NULL;
        xform xf = page_xform(page);
        for (int i = 0; out && i < count; i++) {
            pdfp_char *c = &out[head.n++];
            memset(c, 0, sizeof *c);
            c->cp = FPDFText_GetUnicode(tp, i) & PDFP_CH_CODE;
            if (FPDFText_IsHyphen(tp, i) == 1) c->cp |= PDFP_CH_HYPHEN;
            FS_RECTF box;
            if (FPDFText_IsGenerated(tp, i) == 1) {
                c->cp |= PDFP_CH_GEN;
            } else if (FPDFText_GetLooseCharBox(tp, i, &box)) {
                float q[4];
                map_box(&xf, box.left, box.bottom, box.right, box.top, q);
                c->l = q[0], c->t = q[1], c->r = q[2], c->b = q[3];
            }
        }
        FPDFText_ClosePage(tp);
    }
    send_msg(PDFP_TEXTED, m->head.doc, m->head.ticket, &head, sizeof head, out, head.n * (uint32_t)sizeof(pdfp_char));
    free(out);
}

static void find_start(const msg *m)
{
    S.active = 0;
    pdfp_find f;
    if (m->head.size < sizeof f + sizeof(unsigned short) || !D.doc || m->head.doc != D.id) return;
    memcpy(&f, m->data, sizeof f);
    size_t n = (m->head.size - sizeof f) / sizeof(unsigned short);
    if (n > 1023) n = 1023;
    memcpy(S.query, m->data + sizeof f, n * sizeof(unsigned short));
    S.query[n] = 0; /* the reader thread leaves room for a NUL after every payload */
    if (!S.query[0]) return; /* an empty query only stops the search */
    S.gen = f.gen;
    S.doc = D.id;
    S.flags = f.flags;
    S.from = f.from < D.npages ? f.from : 0;
    S.done = S.total = 0;
    S.active = D.npages > 0;
}

/* Search text, as copy sees it (pdftext.c): one space for any run of
 * spaces and line breaks, words split by a hyphen at a line's end joined,
 * ligatures spelled out, lowercase unless case matters. at[k] is the
 * character index (PDFP_TEXT numbering) that norm[k] came from. */
typedef struct normtext {
    wchar_t  *s;
    uint32_t *at;
    uint32_t  n, cap;
} normtext;

static int norm_put(normtext *t, wchar_t c, uint32_t at)
{
    if (t->n + 1 >= t->cap) {
        uint32_t cap = t->cap ? t->cap * 2 : 4096;
        wchar_t *s = realloc(t->s, cap * sizeof *s);
        if (s) t->s = s;
        uint32_t *a = s ? realloc(t->at, cap * sizeof *a) : NULL;
        if (!s || !a) return 0;
        t->at = a;
        t->cap = cap;
    }
    t->s[t->n] = c;
    t->at[t->n++] = at;
    return 1;
}

static void norm_char(normtext *t, uint32_t cp, uint32_t at)
{
    static const wchar_t *const lig[] = { L"ff", L"fi", L"fl", L"ffi", L"ffl", L"st", L"st" };
    if (cp >= 0xFB00 && cp <= 0xFB06) {
        for (const wchar_t *p = lig[cp - 0xFB00]; *p; p++) norm_put(t, *p, at);
    } else if (cp > 0xFFFF) {
        norm_put(t, (wchar_t)(0xD800 + ((cp - 0x10000) >> 10)), at);
        norm_put(t, (wchar_t)(0xDC00 + ((cp - 0x10000) & 0x3FF)), at);
    } else {
        norm_put(t, (wchar_t)cp, at);
    }
}

/* chars: the page's text (NULL for a query, given as q). */
static void normalize(normtext *t, FPDF_TEXTPAGE tp, const unsigned short *q, int match_case)
{
    t->n = 0;
    int count = tp ? FPDFText_CountChars(tp) : (int)wcslen((const wchar_t *)q);
    int64_t hyphen = -1; /* a word-splitting hyphen waiting for the next letter */
    for (int i = 0; i < count; i++) {
        uint32_t cp = tp ? FPDFText_GetUnicode(tp, i) : q[i];
        int gen = tp && FPDFText_IsGenerated(tp, i) == 1;
        if (cp == 2) cp = '-';
        if (cp == '\r' || cp == '\n' || cp == ' ' || cp == '\t' || cp == 0xA0 || (gen && cp == ' ')) {
            if (hyphen < 0 && t->n && t->s[t->n - 1] != ' ') norm_put(t, ' ', (uint32_t)i);
            continue;
        }
        if (cp < 0x20 || cp == 0xFFFE || cp == 0xFFFF || gen) continue;
        if (tp && (cp == '-' || cp == 0x2010 || cp == 0xAD) && FPDFText_IsHyphen(tp, i) == 1) {
            hyphen = i;
            continue;
        }
        if (hyphen >= 0) { /* before a lowercase letter it only split the word */
            if (!iswlower((wint_t)cp)) norm_put(t, '-', (uint32_t)hyphen);
            hyphen = -1;
        }
        if (cp == 0xAD) continue;
        norm_char(t, cp, (uint32_t)i);
    }
    while (t->n && t->s[t->n - 1] == ' ') t->n--;
    if (!match_case && t->n) {
        /* any script's capitals, not only ASCII; same length in and out for lowercasing */
        static wchar_t *low;
        static uint32_t low_cap;
        if (low_cap < t->n) {
            wchar_t *p = realloc(low, t->cap * sizeof *p);
            if (!p) return;
            low = p;
            low_cap = t->cap;
        }
        if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, t->s, (int)t->n, low, (int)t->n, NULL, NULL, 0) ==
            (int)t->n)
            memcpy(t->s, low, t->n * sizeof *low);
    }
}

static int word_at(const normtext *t, uint32_t k) { return k < t->n && (iswalnum(t->s[k]) || t->s[k] == '_'); }

/* Searches the next page and reports it; the last page also reports the end. */
static void find_step(void)
{
    static pdfp_hit hits[PDFP_FIND_PAGE_MAX];
    static normtext page_text, query;
    uint32_t pi = (S.from + S.done) % D.npages;
    pdfp_found head = { S.gen, pi, 0 };
    /* a page the renderer keeps is used as it is; others are opened just for this,
     * so a search through the document doesn't push the shown pages out */
    FPDF_PAGE page = NULL;
    int own = 1;
    for (int i = 0; i < 4; i++)
        if (D.pages[i].page && D.pages[i].index == pi) page = D.pages[i].page, own = 0;
    if (!page) page = FPDF_LoadPage(D.doc, (int)pi);
    FPDF_TEXTPAGE tp = page ? FPDFText_LoadPage(page) : NULL;
    int match_case = (S.flags & PDFP_FIND_CASE) != 0;
    normalize(&query, NULL, S.query, match_case);
    if (tp) normalize(&page_text, tp, NULL, match_case);
    if (tp && query.n && page_text.n >= query.n) {
        xform xf = page_xform(page);
        const wchar_t *hay = page_text.s, *needle = query.s;
        uint32_t m = query.n;
        for (uint32_t k = 0; k + m <= page_text.n && head.n < PDFP_FIND_PAGE_MAX && S.total + head.n < PDFP_FIND_MAX;) {
            if (hay[k] != needle[0] || wmemcmp(hay + k, needle, m) ||
                ((S.flags & PDFP_FIND_WORD) && ((k && word_at(&page_text, k - 1)) || word_at(&page_text, k + m)))) {
                k++;
                continue;
            }
            int at = (int)page_text.at[k], count = (int)(page_text.at[k + m - 1] - page_text.at[k]) + 1;
            k += m;
            pdfp_hit *h = &hits[head.n++];
            h->index = (uint32_t)at;
            h->count = (uint32_t)count;
            int boxed = 0;
            for (int k = at; k < at + count; k++) {
                FS_RECTF box;
                if (FPDFText_IsGenerated(tp, k) == 1 || !FPDFText_GetLooseCharBox(tp, k, &box)) continue;
                float q[4];
                map_box(&xf, box.left, box.bottom, box.right, box.top, q);
                if (!boxed++) {
                    h->l = q[0], h->t = q[1], h->r = q[2], h->b = q[3];
                } else {
                    if (q[0] < h->l) h->l = q[0];
                    if (q[1] < h->t) h->t = q[1];
                    if (q[2] > h->r) h->r = q[2];
                    if (q[3] > h->b) h->b = q[3];
                }
            }
            if (!boxed) h->l = h->t = h->r = h->b = 0;
        }
    }
    if (tp) FPDFText_ClosePage(tp);
    if (page && own) FPDF_ClosePage(page);
    send_msg(PDFP_FOUND, S.doc, 0, &head, sizeof head, hits, head.n * (uint32_t)sizeof(pdfp_hit));
    S.total += head.n;
    S.done++;
    if (S.done >= D.npages || S.total >= PDFP_FIND_MAX) {
        pdfp_found end = { S.gen, S.done, S.total };
        send_msg(PDFP_FIND_DONE, S.doc, 0, &end, sizeof end, NULL, 0);
        S.active = 0;
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    HANDLE section = (HANDLE)(uintptr_t)_wcstoui64(cmd, NULL, 10);
    Q.slots = section ? MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, (SIZE_T)PDFP_SLOTS * PDFP_SLOT_BYTES) : NULL;
    Q.in = GetStdHandle(STD_INPUT_HANDLE);
    Q.out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!Q.slots || !Q.in || !Q.out) return 2;
    InitializeCriticalSection(&Q.lock);
    Q.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    FPDF_LIBRARY_CONFIG cfg = { 2, NULL, NULL, 0 };
    FPDF_InitLibraryWithConfig(&cfg);
    HANDLE t = CreateThread(NULL, 0, reader, NULL, 0, NULL);
    if (!t) return 3;

    for (;;) {
        msg *m = pop();
        if (!m) {
            if (Q.eof) break;
            if (S.active) {
                find_step(); /* only while nothing else waits */
                continue;
            }
            WaitForSingleObject(Q.wake, INFINITE);
            continue;
        }
        switch (m->head.type) {
        case PDFP_OPEN:
        case PDFP_THUMB_OPEN:
            if (m->head.size >= 2 && !(m->head.size % 2) &&
                !((wchar_t *)m->data)[m->head.size / 2 - 1]) open_doc(m);
            break;
        case PDFP_CLOSE: close_doc(); break;
        case PDFP_RENDER:
            if (((pdfp_render *)m->data)->epoch >= (uint32_t)Q.latest_epoch) render(m);
            break;
        case PDFP_TEXT: text_page(m); break;
        case PDFP_FIND: find_start(m); break;
        case PDFP_QUIT: free(m); goto done;
        default: break;
        }
        free(m);
    }
done:
    close_doc();
    FPDF_DestroyLibrary();
    return 0;
}
