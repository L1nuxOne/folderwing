#include "../src/namecodec.h"
/* The image reader. Decode and file reads never run on the UI thread. */
#include "imageview.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <windowsx.h>
#include <wincodec.h>
#include <shellapi.h>
#include "app.h"
#include "theme.h"
#include "menu.h"
#include "imageproto.h"
#include "imageutil.h"
#include "previewlaunch.h"
#include "previewreader.h"
#include "pipewriter.h"
#define WM_IMG_MSG (WM_APP + 70)
#define WM_IMG_GONE (WM_APP + 71)
#define WM_IMG_LAUNCHED (WM_APP + 72)
#define WM_IMG_RESULT (WM_APP + 73)
#define TIMER_RENDER 1
#define TIMER_WATCHDOG 2
#define TIMER_CLIPBOARD 3
#define TIMER_ANIMATION 4
typedef struct msg {
    struct msg *next;
    imgp_head head;
    uint8_t data[];
} msg;
typedef struct picture {
    HBITMAP bitmap;
    uint8_t *pixels;
    uint32_t w, h;
    imgp_render region;
} picture;
static struct {
    HWND hwnd;
    int cw, ch, bw, bh;
    HDC bdc;
    HBITMAP bmp;
    HGDIOBJ old_bmp;
    HANDLE process, job, section, to, from, child_in, child_out;
    preview_reader *reader;
    pipe_writer *writer;
    preview_launch *launcher;
    uint32_t launch_gen;
    DWORD pid;
    uint8_t *pixels;
    int gone;
    wchar_t *path;
    uint64_t size;
    int64_t mtime;
    uint32_t doc, ticket, epoch, sharp_epoch, cache_mb, background, rotation, flip;
    uint32_t frame, shown_frame, delay_ms, passes, copy_ticket;
    int playing, ended;
    uint32_t page_desired,page_ticket;int page_pending;
    uint32_t browse_position,browse_count;int browse_previous,browse_next,browse_state,decode_result;
    imgp_info info;
    int opened, metadata, fit, panning, pan_x, pan_y, copying;
    double scale, left, top, pan_left, pan_top;
    picture thumb, sharp;
    struct {
        uint32_t ticket, doc, epoch;
        int busy, kind;
        imgp_render req;
    } banks[IMGP_BANKS];
    HGLOBAL clipboard;
    uint64_t clipboard_until;
    DWORD clipboard_sequence;
    uint32_t clipboard_w, clipboard_h;
    uint64_t started;
    HRESULT err;
} R;
static int head_h(void) { return app_px((R.metadata ? 114 : 54)+28+((R.info.animated || R.info.pages>1) ? 28 : 0)); }
static int view_h(void)
{
    int h = R.ch - head_h();
    return h > 0 ? h : 1;
}
static void request_render(void);
static void helper_stop(void);
static void page_send(void);
int imageview_animated(void) { return R.opened && R.info.animated && R.info.frames>1; }
static void animation_schedule(void) {
    KillTimer(R.hwnd,TIMER_ANIMATION);
    if(imageview_animated() && R.playing && R.sharp.bitmap && R.sharp_epoch==R.epoch && IsWindowVisible(R.hwnd) && SUCCEEDED(R.err))
        SetTimer(R.hwnd,TIMER_ANIMATION,R.delay_ms?R.delay_ms:100,NULL);
}
static void exe_dir(wchar_t *buf, size_t cap)
{
    DWORD n = GetModuleFileNameW(NULL, buf, (DWORD)cap);
    wchar_t *s = n && n < cap ? wcsrchr(buf, L'\\') : NULL;
    if (s)
        s[1] = 0;
    else
        buf[0] = 0;
}
int imageview_available(void)
{
    wchar_t dir[MAX_PATH], p[MAX_PATH + 16];
    exe_dir(dir, MAX_PATH);
    _snwprintf(p, MAX_PATH + 16, L"%lsfolderwing-image.exe", dir);
    p[MAX_PATH + 15] = 0;
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}
static int read_all(HANDLE h, void *buf, DWORD n)
{
    uint8_t *p = buf;
    while (n) {
        DWORD g;
        if (!ReadFile(h, p, n, &g, NULL) || !g)
            return -1;
        p += g;
        n -= g;
    }
    return 0;
}
static DWORD WINAPI from_helper(void *arg)
{
    preview_reader *reader = arg;
    HANDLE in = reader->pipe;
    HWND to = reader->window;
    uint32_t generation = reader->generation;
    for (;;) {
        imgp_head h;
        if (read_all(in, &h, sizeof h) || h.size > sizeof(imgp_info) ||
            !(((h.type == IMGP_OPENED || h.type == IMGP_PAGED) && h.size == sizeof(imgp_info)) ||
              (h.type == IMGP_RENDERED && h.size == sizeof(imgp_rendered))))
            break;
        msg *m = malloc(sizeof *m + h.size);
        if (!m || read_all(in, m->data, h.size)) {
            free(m);
            break;
        }
        m->head = h;
        if (InterlockedCompareExchange(&reader->cancelled, 0, 0) ||
            !PostMessageW(to, WM_IMG_MSG, generation, (LPARAM)m))
            free(m);
    }
    if (!InterlockedCompareExchange(&reader->cancelled, 0, 0)) PostMessageW(to, WM_IMG_GONE, generation, 0);
    preview_reader_finish(reader);
    return 0;
}
static int send_req(uint32_t type, uint32_t doc, uint32_t ticket, const void *data, uint32_t size)
{
    if (!R.writer) return -1;
    size_t n = sizeof(imgp_head) + size;
    uint8_t *bytes = malloc(n);
    if (!bytes) { PostMessageW(R.hwnd, WM_IMG_GONE, R.launch_gen, 0); return -1; }
    imgp_head h = {type, size, doc, ticket};
    memcpy(bytes, &h, sizeof h);
    if (size) memcpy(bytes + sizeof h, data, size);
    uint32_t tag = type == IMGP_OPEN ? 1u : type == IMGP_CLOSE ? 2u : type == IMGP_PAGE ? 4u : 0u;
    uint32_t drop = (type == IMGP_OPEN || type == IMGP_CLOSE) ? 7u : 0u;
    int rc = pipe_writer_send(R.writer, bytes, n, tag, drop);
    free(bytes);
    if (rc) PostMessageW(R.hwnd, WM_IMG_GONE, R.launch_gen, 0);
    return rc;
}
static void helper_stop(void)
{
    ++R.launch_gen;
    preview_launch_cancel(&R.launcher);
    pipe_writer_cancel(&R.writer);
    preview_reader_cancel(&R.reader);
    if (R.job)
        TerminateJobObject(R.job, 1);
    else if (R.process)
        TerminateProcess(R.process, 1);
    HANDLE *handles[] = {&R.to, &R.from, &R.process, &R.job, &R.child_in, &R.child_out, &R.section};
    if (R.pixels)
        UnmapViewOfFile(R.pixels);
    R.pixels = NULL;
    for (size_t i = 0; i < sizeof handles / sizeof *handles; ++i)
        if (*handles[i]) {
            CloseHandle(*handles[i]);
            *handles[i] = NULL;
        }
    memset(R.banks, 0, sizeof R.banks);
    R.pid = 0;
    if (R.hwnd) {
        MSG m;
        while (PeekMessageW(&m, R.hwnd, WM_IMG_MSG, WM_IMG_MSG, PM_REMOVE))
            free((void *)m.lParam);
        while (PeekMessageW(&m, R.hwnd, WM_IMG_GONE, WM_IMG_LAUNCHED, PM_REMOVE)) {
        }
    }
}
/* Pipes and pixels now, the process on a thread: requests written before
 * it runs wait in the pipe. */
static int helper_start(void)
{
    if (R.process || R.launcher)
        return 0;
    int64_t t = fpe_prof_begin();
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    int ok = 0;
    R.section = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE | SEC_COMMIT, 0,
                                   (DWORD)IMGP_BANKS * IMGP_BANK_BYTES, NULL);
    R.pixels = R.section ? MapViewOfFile(R.section, FILE_MAP_READ, 0, 0, 0) : NULL;
    if (!R.pixels || !CreatePipe(&R.child_in, &R.to, &sa, 1 << 20) ||
        !CreatePipe(&R.from, &R.child_out, &sa, 1 << 20))
        goto out;
    SetHandleInformation(R.to, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(R.from, HANDLE_FLAG_INHERIT, 0);
    R.writer = pipe_writer_begin(R.to, 1 << 20, R.hwnd, WM_IMG_GONE, R.launch_gen + 1);
    if (!R.writer) goto out;
    R.reader = preview_reader_begin(R.from, R.hwnd, R.launch_gen + 1, from_helper);
    if (!R.reader) goto out;
    CloseHandle(R.from); R.from = NULL;
    R.launcher = preview_launch_begin(L"folderwing-image.exe", R.child_in, R.child_out, R.section,
                                      (SIZE_T)512 << 20, R.hwnd, WM_IMG_LAUNCHED, ++R.launch_gen);
    ok = R.launcher != NULL;
    if (ok) {
        CloseHandle(R.child_in); CloseHandle(R.child_out);
        R.child_in = R.child_out = NULL;
    }
out:
    if (!ok)
        helper_stop();
    R.gone = 0;
    FPE_PROF_END("image.helper_start", t);
    return ok ? 0 : -1;
}

static void picture_drop(picture *p)
{
    if (p->bitmap)
        DeleteObject(p->bitmap);
    memset(p, 0, sizeof *p);
}
static void dimensions(uint32_t *w, uint32_t *h)
{
    *w = R.info.w;
    *h = R.info.h;
    if (R.rotation & 1) {
        uint32_t t = *w;
        *w = *h;
        *h = t;
    }
}
static void clamp_view(void)
{
    uint32_t w, h;
    dimensions(&w, &h);
    double mx = w * R.scale - R.cw, my = h * R.scale - view_h();
    if (mx < 0)
        mx = 0;
    if (my < 0)
        my = 0;
    if (R.left < 0)
        R.left = 0;
    if (R.left > mx)
        R.left = mx;
    if (R.top < 0)
        R.top = 0;
    if (R.top > my)
        R.top = my;
}
static void title_update(void)
{
    wchar_t text[512];
    const wchar_t *name = R.path ? wcsrchr(R.path, L'\\') : NULL;
    name = name ? name + 1 : R.path;
    if (!R.path)
        wcscpy(text, L"Image preview");
    else if (FAILED(R.err))
        _snwprintf(text, 512, R.opened&&R.info.pages>1?L"%ls | Page unavailable (0x%08lX)":L"%ls | Image unavailable (0x%08lX)", name, (unsigned long)R.err);
    else if (!R.opened)
        _snwprintf(text, 512, L"%ls | Loading image", name);
    else {
        uint32_t w, h;
        dimensions(&w, &h);
        _snwprintf(text, 512, L"%ls | %ls | %u x %u | %.0f%% | %ls%ls%ls", name, R.info.format, w, h,
                   R.scale * 100,
                   R.fit == 1   ? L"Fit"
                   : R.fit == 2 ? L"Fit width"
                                : L"Zoom",
                   R.rotation ? L" | Rotated" : L"", R.flip ? L" | Flipped" : L"");
    }
    text[511] = 0;
    if(R.opened && R.info.animated) {
        size_t n=wcslen(text);
        _snwprintf(text+n,512-n,L" | Frame %u/%u | %ls",R.shown_frame+1,R.info.frames,R.playing?L"Playing":L"Paused");
        text[511]=0;
    }
    if(R.opened && R.info.pages>1) {
        size_t n=wcslen(text);int loading=SUCCEEDED(R.err)&&(R.page_pending||!R.sharp.bitmap||R.sharp.region.frame!=R.page_desired);
        _snwprintf(text+n,512-n,L" | Page %u / %u%ls",R.page_desired+1,R.info.pages,loading?L" | Loading page":L"");text[511]=0;
    }
    if(R.path && R.browse_state==0 && R.browse_position) {
        size_t n=wcslen(text);_snwprintf(text+n,512-n,L" | Image %u / %u",R.browse_position,R.browse_count);text[511]=0;
    }
    if(R.path&&fpe_wsl_native(R.path)){wchar_t display[512];fpe_lx_display(text,display,512,1);wcscpy(text,display);}
    SetPropW(R.hwnd,L"Image.PageCount",(HANDLE)(UINT_PTR)(R.opened?R.info.pages:0));
    SetPropW(R.hwnd,L"Image.Page",(HANDLE)(UINT_PTR)(R.opened&&R.info.pages&&R.sharp.bitmap&&R.sharp.region.frame==R.page_desired&&SUCCEEDED(R.err)?R.page_desired+1:0));
    SetPropW(R.hwnd,L"Image.PageRequested",(HANDLE)(UINT_PTR)(R.opened&&R.info.pages?R.page_desired+1:0));
    SetPropW(R.hwnd,L"Image.PageState",(HANDLE)(INT_PTR)(!R.opened||!R.info.pages||FAILED(R.err)||R.gone?-1:R.page_pending||!R.sharp.bitmap||R.sharp.region.frame!=R.page_desired?1:0));
    SetPropW(R.hwnd,L"Image.PreviousPage",(HANDLE)(UINT_PTR)(imageview_page_can(-1)&&fn_active(fn_find("image.previous_page"))));
    SetPropW(R.hwnd,L"Image.NextPage",(HANDLE)(UINT_PTR)(imageview_page_can(1)&&fn_active(fn_find("image.next_page"))));
    SetWindowTextW(R.hwnd, text);
}
static void changed(void)
{
    KillTimer(R.hwnd,TIMER_ANIMATION);
    R.epoch++;
    clamp_view();
    title_update();
    InvalidateRect(R.hwnd, NULL, FALSE);
    if (R.opened)
        SetTimer(R.hwnd, TIMER_RENDER, 35, NULL);
}
static void fit_view(int width)
{
    uint32_t w, h;
    dimensions(&w, &h);
    R.fit = width ? 2 : 1;
    R.scale = image_fit(w, h, R.cw - 16, view_h() - 16, width);
    R.left = R.top = 0;
    changed();
}
static void zoom_at(double scale, int x, int y)
{
    if (!R.opened)
        return;
    if (scale < 0.001)
        scale = 0.001;
    if (scale > 4096)
        scale = 4096;
    uint32_t w, h;
    dimensions(&w, &h);
    double ox = w * R.scale < R.cw ? (R.cw - w * R.scale) / 2 : 0,
           oy = h * R.scale < view_h() ? (view_h() - h * R.scale) / 2 : 0;
    double ix = (x - ox + R.left) / R.scale, iy = (y - oy + R.top) / R.scale;
    R.scale = scale;
    R.fit = 0;
    ox = w * scale < R.cw ? (R.cw - w * scale) / 2 : 0;
    oy = h * scale < view_h() ? (view_h() - h * scale) / 2 : 0;
    R.left = ix * scale - x + ox;
    R.top = iy * scale - y + oy;
    changed();
}
static int render_send(int kind, imgp_render r)
{
    int bank = -1;
    for (unsigned i = 0; i < IMGP_BANKS; ++i)
        if (!R.banks[i].busy) {
            bank = (int)i;
            break;
        }
    if (bank < 0)
        return -1;
    r.frame=R.frame;
    r.bank = (uint32_t)bank;
    r.straight = kind == 3;
    r.rotation = R.rotation;
    r.flip = R.flip;
    R.banks[bank].busy = 1;
    R.banks[bank].ticket = ++R.ticket;
    if(kind==3) R.copy_ticket=R.ticket;
    R.banks[bank].doc = R.doc;
    R.banks[bank].epoch = R.epoch;
    R.banks[bank].kind = kind;
    R.banks[bank].req = r;
    if (send_req(IMGP_RENDER, R.doc, R.ticket, &r, sizeof r)) {
        R.banks[bank].busy = 0;
        return -1;
    }
    return 0;
}
static void request_render(void)
{
    if (!R.opened || R.page_pending || R.gone || FAILED(R.err) || R.cw <= 0 || R.ch <= head_h())
        return;
    uint32_t w, h;
    dimensions(&w, &h);
    if (R.copying == 1) return;
    if (R.copying == 2) {
        imgp_render r = {0};
        r.cw = r.w = w;
        r.ch = r.h = h;
        if (!render_send(3, r))
            R.copying = 1;
        return;
    }
    int thumb_pending = 0, sharp_pending = 0;
    for (unsigned i = 0; i < IMGP_BANKS; ++i)
        if (R.banks[i].busy && R.banks[i].doc == R.doc) {
            if (R.banks[i].kind == 1)
                thumb_pending = 1;
            if (R.banks[i].kind == 2 && R.banks[i].epoch == R.epoch)
                sharp_pending = 1;
        }
    if (!R.thumb.bitmap && !thumb_pending) {
        double s = w > h ? 512.0 / w : 512.0 / h;
        if (s > 1)
            s = 1;
        imgp_render r = {0};
        r.cw = w;
        r.ch = h;
        r.w = (uint32_t)ceil(w * s);
        r.h = (uint32_t)ceil(h * s);
        render_send(1, r);
        return; /* first show a small image before the expensive viewport */
    }
    if (sharp_pending || (R.sharp.bitmap && R.sharp_epoch == R.epoch))
        return;
    imgp_render r = {0};
    r.x = (uint32_t)floor(R.left / R.scale);
    r.y = (uint32_t)floor(R.top / R.scale);
    if (r.x >= w)
        r.x = w - 1;
    if (r.y >= h)
        r.y = h - 1;
    r.cw = (uint32_t)ceil((R.cw + 2) / R.scale);
    r.ch = (uint32_t)ceil((view_h() + 2) / R.scale);
    if (r.cw > w - r.x)
        r.cw = w - r.x;
    if (r.ch > h - r.y)
        r.ch = h - r.y;
    double s = R.scale;
    if (r.cw * s > IMGP_EDGE)
        s = (double)IMGP_EDGE / r.cw;
    if (r.ch * s > IMGP_EDGE)
        s = (double)IMGP_EDGE / r.ch;
    /* Each cached viewport is limited to half the byte budget. */
    double maxpixels = ((double)R.cache_mb * 1024 * 1024 - 1024 * 1024) / 8;
    if ((double)r.cw * r.ch * s * s > maxpixels)
        s = sqrt(maxpixels / ((double)r.cw * r.ch));
    r.w = (uint32_t)ceil(r.cw * s);
    r.h = (uint32_t)ceil(r.ch * s);
    if (r.w > IMGP_EDGE)
        r.w = IMGP_EDGE;
    if (r.h > IMGP_EDGE)
        r.h = IMGP_EDGE;
    if (r.w && r.h)
        render_send(2, r);
}
static int picture_set(picture *p, const imgp_rendered *reply, const imgp_render *region)
{
    if (!reply->w || !reply->h || reply->w > IMGP_EDGE || reply->h > IMGP_EDGE || reply->bank >= IMGP_BANKS)
        return -1;
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = reply->w;
    bi.bmiHeader.biHeight = -(int)reply->h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    picture next = {0};
    next.bitmap = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&next.pixels, NULL, 0);
    if (!next.bitmap)
        return -1;
    next.w = reply->w;
    next.h = reply->h;
    next.region = *region;
    memcpy(next.pixels, R.pixels + (size_t)reply->bank * IMGP_BANK_BYTES, (size_t)reply->w * reply->h * 4);
    picture_drop(p);
    *p = next;
    return 0;
}
static void clipboard_publish(void)
{
    if (!R.clipboard)
        return;
    if (GetClipboardSequenceNumber() != R.clipboard_sequence) {
        GlobalFree(R.clipboard);
        R.clipboard = NULL;
        KillTimer(R.hwnd, TIMER_CLIPBOARD);
        app_status(NULL, 1, L"Image copy cancelled because the clipboard changed");
        return;
    }
    if (OpenClipboard(R.hwnd)) {
        EmptyClipboard();
        int ok = SetClipboardData(CF_DIBV5, R.clipboard) != NULL;
        CloseClipboard();
        if (!ok)
            GlobalFree(R.clipboard);
        R.clipboard = NULL;
        KillTimer(R.hwnd, TIMER_CLIPBOARD);
        if (ok)
            app_status(NULL, 0, L"Copied image pixels (%u x %u)", R.clipboard_w, R.clipboard_h);
        else
            app_status(NULL, 1, L"Image could not be copied");
    } else if (GetTickCount64() >= R.clipboard_until) {
        GlobalFree(R.clipboard);
        R.clipboard = NULL;
        KillTimer(R.hwnd, TIMER_CLIPBOARD);
        app_status(NULL, 1, L"The clipboard is busy; try copying again");
    } else
        SetTimer(R.hwnd, TIMER_CLIPBOARD, 40, NULL);
}
static void copy_pixels(const imgp_rendered *p)
{
    size_t bytes = (size_t)p->w * p->h * 4;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPV5HEADER) + bytes);
    if (!memory) {
        app_status(NULL, 1, L"Not enough memory to copy the image");
        return;
    }
    BITMAPV5HEADER *v = GlobalLock(memory);
    if (!v) {
        GlobalFree(memory);
        return;
    }
    memset(v, 0, sizeof *v);
    v->bV5Size = sizeof *v;
    v->bV5Width = p->w;
    v->bV5Height = -(LONG)p->h;
    v->bV5Planes = 1;
    v->bV5BitCount = 32;
    v->bV5Compression = BI_BITFIELDS;
    v->bV5RedMask = 0x00ff0000;
    v->bV5GreenMask = 0x0000ff00;
    v->bV5BlueMask = 0x000000ff;
    v->bV5AlphaMask = 0xff000000;
    v->bV5CSType = LCS_sRGB;
    v->bV5Intent = LCS_GM_IMAGES;
    uint8_t *dst = (uint8_t *)(v + 1);
    memcpy(dst, R.pixels + (size_t)p->bank * IMGP_BANK_BYTES, bytes);
    GlobalUnlock(memory);
    if (R.clipboard)
        GlobalFree(R.clipboard);
    R.clipboard = memory;
    R.clipboard_w = p->w;
    R.clipboard_h = p->h;
    R.clipboard_until = GetTickCount64() + 1500;
    clipboard_publish();
}
static void fill(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    RECT r = {x, y, x + w, y + h};
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}
static void draw_picture(HDC dc, picture *p, int x, int y, int w, int h)
{
    if (!p->bitmap || w <= 0 || h <= 0)
        return;
    HDC src = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(src, p->bitmap);
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(dc, x, y, w, h, src, 0, 0, p->w, p->h, blend);
    SelectObject(src, old);
    DeleteDC(src);
}
static void paint(HDC dc)
{
    fill(dc, 0, 0, R.cw, R.ch, C_BG);
    int hh = head_h();
    if (R.background == 0 && R.opened) {
        uint32_t w, h;
        dimensions(&w, &h);
        int x0 = w * R.scale < R.cw ? (int)((R.cw - w * R.scale) / 2) : 0;
        int y0 = hh + (h * R.scale < view_h() ? (int)((view_h() - h * R.scale) / 2) : 0);
        int x1 = x0 + (int)ceil(w * R.scale), y1 = y0 + (int)ceil(h * R.scale);
        if (x1 > R.cw)
            x1 = R.cw;
        if (y1 > R.ch)
            y1 = R.ch;
        int sz = app_px(16);
        HBRUSH a = CreateSolidBrush(RGB(60, 60, 62)), b = CreateSolidBrush(RGB(43, 43, 45));
        for (int y = y0; y < y1; y += sz)
            for (int x = x0; x < x1; x += sz) {
                RECT r = {x, y, x + sz < x1 ? x + sz : x1, y + sz < y1 ? y + sz : y1};
                FillRect(dc, &r, ((x - x0) / sz + (y - y0) / sz) & 1 ? a : b);
            }
        DeleteObject(a);
        DeleteObject(b);
    } else if (R.background)
        fill(dc, 0, hh, R.cw, R.ch - hh, R.background == 1 ? RGB(255, 255, 255) : RGB(24, 24, 24));
    SaveDC(dc);
    IntersectClipRect(dc, 0, hh, R.cw, R.ch);
    if (R.opened) {
        uint32_t w, h;
        dimensions(&w, &h);
        double ox = w * R.scale < R.cw ? (R.cw - w * R.scale) / 2 : -R.left;
        double oy = h * R.scale < view_h() ? (view_h() - h * R.scale) / 2 : -R.top;
        if ((!R.info.animated && !R.info.pages || R.thumb.region.frame==R.frame) && w * R.scale <= 32767 && h * R.scale <= 32767)
            draw_picture(dc, &R.thumb, (int)ox, hh + (int)oy, (int)ceil(w * R.scale), (int)ceil(h * R.scale));
        if (R.sharp.bitmap) {
            imgp_render *r = &R.sharp.region;
            draw_picture(dc, &R.sharp, (int)floor(ox + r->x * R.scale), hh + (int)floor(oy + r->y * R.scale),
                         (int)ceil(r->cw * R.scale), (int)ceil(r->ch * R.scale));
        }
    }
    RestoreDC(dc, -1);
    fill(dc, 0, 0, R.cw, hh, C_HEADER);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, app_font());
    SetTextColor(dc, C_TEXT);
    wchar_t title[512];
    GetWindowTextW(R.hwnd, title, 512);
    RECT text = {app_px(10), app_px(6), R.cw - app_px(10), app_px(28)};
    DrawTextW(dc, title, -1, &text, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    wchar_t line[512];
    if (FAILED(R.err)) {
        const wchar_t *reason = R.err == HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE)
                                    ? L"This path exceeds the image preview path limit."
                                : R.err == WINCODEC_ERR_COMPONENTNOTFOUND
                                    ? L"No Windows codec is installed for this image format."
                                : R.err == WINCODEC_ERR_IMAGESIZEOUTOFRANGE
                                    ? L"Image exceeds the configured file limit or pixel limit."
                                : R.err == HRESULT_FROM_WIN32(ERROR_TIMEOUT)
                                    ? L"Image loading timed out. Move away and select it again to retry."
                                    : L"This image could not be decoded. It may be damaged or unsupported.";
        wcscpy(line, reason);
        SetTextColor(dc, C_ERROR);
    } else if (R.opened)
        _snwprintf(line, 512, L"%.2f MB  |  %ls  |  %ls%ls", R.info.bytes / (1024.0 * 1024),
                   R.background == 0   ? L"Checkerboard"
                   : R.background == 1 ? L"White background"
                                       : L"Dark background",
                   R.info.color == 1   ? L"ICC to sRGB"
                   : R.info.color == 2 ? L"Colour profile unavailable"
                                       : L"sRGB assumed",
                   R.info.partial ? L"  |  SVG has unsupported content; view source" : R.info.frames > 1 && !R.info.animated && !R.info.pages ? L"  |  First frame" : L"");
    else
        wcscpy(line, L"Loading image...");
    line[511] = 0;
    text.top = app_px(29);
    text.bottom = app_px(51);
    DrawTextW(dc, line, -1, &text, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    const wchar_t *browse_labels[]={L"Previous image",L"Next image"};
    for(int i=0;i<2;i++){
        RECT button={app_px(10+i*110),app_px(55),app_px(110+i*110),app_px(79)};
        fill(dc,button.left,button.top,button.right-button.left,button.bottom-button.top,C_BG);
        SetTextColor(dc,(i?R.browse_next:R.browse_previous)?C_TEXT:C_DIM);
        DrawTextW(dc,browse_labels[i],-1,&button,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }
    SetTextColor(dc,C_TEXT);
    if(R.browse_state>0)wcscpy(line,L"Building image list...");
    else if(R.browse_state<0)wcscpy(line,L"Image browsing unavailable");
    else if(R.browse_position)swprintf(line,512,L"%u / %u",R.browse_position,R.browse_count);
    else swprintf(line,512,L"%u images",R.browse_count);
    RECT count={app_px(230),app_px(55),R.cw-app_px(10),app_px(79)};
    DrawTextW(dc,line,-1,&count,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
    if(R.info.animated && R.opened) {
        static const wchar_t *labels[]={L"Previous",L"",L"Next"};
        for(int i=0;i<3;++i) {
            RECT button={app_px(10+i*90),app_px(83),app_px(94+i*90),app_px(107)};
            fill(dc,button.left,button.top,button.right-button.left,button.bottom-button.top,C_BG);
            DrawTextW(dc,i==1?(R.playing?L"Pause":L"Play"):labels[i],-1,&button,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        }
    }
    if(R.opened && R.info.pages>1) {
        const wchar_t *labels[]={L"Previous page",L"Next page"};
        for(int i=0;i<2;i++) {
            RECT button={app_px(10+i*110),app_px(83),app_px(110+i*110),app_px(107)};
            fill(dc,button.left,button.top,button.right-button.left,button.bottom-button.top,C_BG);
            SetTextColor(dc,imageview_page_can(i?1:-1)&&fn_active(fn_find(i?"image.next_page":"image.previous_page"))?C_TEXT:C_DIM);
            DrawTextW(dc,labels[i],-1,&button,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        }
        SetTextColor(dc,C_TEXT);swprintf(line,512,L"Page %u / %u",R.page_desired+1,R.info.pages);
        RECT count={app_px(230),app_px(83),R.cw-app_px(10),app_px(107)};
        DrawTextW(dc,line,-1,&count,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
    }
    if (R.metadata && R.opened) {
        _snwprintf(line, 512, L"Camera: %ls   Taken: %ls", R.info.camera[0] ? R.info.camera : L"unavailable",
                   R.info.date[0] ? R.info.date : L"unavailable");
        line[511] = 0;
        text.top = app_px(84+((R.info.animated||R.info.pages>1)?28:0));
        text.bottom = app_px(108+((R.info.animated||R.info.pages>1)?28:0));
        DrawTextW(dc, line, -1, &text, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        _snwprintf(line, 512, L"EXIF orientation: %u   Frames/pages: %u   Source: %ls", R.info.orientation,
                   R.info.frames, R.path);
        line[511] = 0;
        text.top = app_px(110+((R.info.animated||R.info.pages>1)?28:0));
        text.bottom = app_px(136+((R.info.animated||R.info.pages>1)?28:0));
        DrawTextW(dc, line, -1, &text, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
}
int imageview_page_can(int direction) {
    return R.path&&R.opened&&!R.gone&&R.info.pages>1&&
        (direction>0?R.page_desired+1<R.info.pages:direction<0&&R.page_desired>0);
}
void imageview_controls_refresh(void) {if(R.hwnd){title_update();InvalidateRect(R.hwnd,NULL,FALSE);}}
static void page_send(void) {
    if(R.page_pending||R.gone)return;
    imgp_page page={R.page_desired};R.page_pending=1;R.page_ticket=++R.ticket;
    R.started=GetTickCount64();SetTimer(R.hwnd,TIMER_WATCHDOG,500,NULL);
    if(send_req(IMGP_PAGE,R.doc,R.page_ticket,&page,sizeof page)){R.page_pending=0;R.err=E_FAIL;}
    imageview_controls_refresh();
}
static void page_change(int direction) {
    if(!imageview_page_can(direction))return;
    R.page_desired=direction>0?R.page_desired+1:R.page_desired-1;
    if(R.copying||R.clipboard)app_status(NULL,0,L"Image copy cancelled because the page changed");
    R.copying=0;R.copy_ticket=0;
    if(R.clipboard){GlobalFree(R.clipboard);R.clipboard=NULL;}
    KillTimer(R.hwnd,TIMER_CLIPBOARD);KillTimer(R.hwnd,TIMER_RENDER);
    picture_drop(&R.thumb);picture_drop(&R.sharp);
    R.left=R.top=0;R.err=S_OK;R.epoch++;
    page_send();imageview_controls_refresh();
}
static void frame_change(uint32_t frame) {
    if(R.copying) {
        R.copying=0;
        app_status(NULL,0,L"Image copy cancelled because the frame changed");
    }
    R.frame=frame;
    changed();request_render();
}
void imageview_action(int action)
{
    if (!R.hwnd || !R.path)
        return;
    if(action==IMAGE_PREVIOUS_PAGE||action==IMAGE_NEXT_PAGE){page_change(action==IMAGE_NEXT_PAGE?1:-1);return;}
    if (action == IMAGE_OPEN) {
        if(!fpe_wsl_requires_native(R.path))ShellExecuteW(app_window(),L"open",R.path,NULL,NULL,SW_SHOWNORMAL);
        return;
    }
    if (action == IMAGE_BACKGROUND) {
        R.background = (R.background + 1) % 3;
        changed();
        return;
    }
    if (action == IMAGE_METADATA) {
        R.metadata = !R.metadata;
        if (R.fit)
            fit_view(R.fit == 2);
        else
            changed();
        return;
    }
    if (!R.opened)
        return;
    switch (action) {
    case IMAGE_PLAY:
        if(imageview_animated()) {
            R.playing=!R.playing;R.passes=0;
            if(R.playing && R.ended) {R.ended=0;frame_change(0);}
            else animation_schedule();
            title_update();InvalidateRect(R.hwnd,NULL,FALSE);
        }
        break;
    case IMAGE_PREVIOUS:
    case IMAGE_NEXT:
        if(imageview_animated()) {
            R.playing=0;R.passes=0;R.ended=0;
            frame_change(action==IMAGE_NEXT?(R.frame+1)%R.info.frames:(R.frame?R.frame-1:R.info.frames-1));
        }
        break;
    case IMAGE_FIT:
        fit_view(0);
        break;
    case IMAGE_WIDTH:
        fit_view(1);
        break;
    case IMAGE_ACTUAL:
        zoom_at(1, R.cw / 2, view_h() / 2);
        break;
    case IMAGE_ZOOM_IN:
        zoom_at(R.scale * 1.25, R.cw / 2, view_h() / 2);
        break;
    case IMAGE_ZOOM_OUT:
        zoom_at(R.scale * 0.8, R.cw / 2, view_h() / 2);
        break;
    case IMAGE_ROTATE:
    case IMAGE_FLIP:
        if (action == IMAGE_ROTATE)
            R.rotation = (R.rotation + 1) % 4;
        else
            R.flip = !R.flip;
        picture_drop(&R.thumb);
        picture_drop(&R.sharp);
        R.copying = 0;
        fit_view(R.fit == 2);
        break;
    case IMAGE_COPY: {
        if(R.page_pending||FAILED(R.err)){app_status(NULL,1,L"Wait for an available image page before copying pixels");break;}
        if(R.info.animated) { R.playing=0;KillTimer(R.hwnd,TIMER_ANIMATION);title_update();InvalidateRect(R.hwnd,NULL,FALSE); }
        uint32_t w, h;
        dimensions(&w, &h);
        if (w > IMGP_EDGE || h > IMGP_EDGE || (uint64_t)w * h * 4 > (uint64_t)R.cache_mb * 1024 * 1024) {
            app_status(NULL, 1, L"Image is too large to copy as pixels; copy the file instead");
            break;
        }
        if (!R.copying && !R.clipboard) {
            R.clipboard_sequence = GetClipboardSequenceNumber();
            R.copying = 2;
            app_status(NULL, 0, L"Copying image pixels...");
            request_render();
        }
        break;
    }
    }
}
static void key(WPARAM vk, int sys)
{
    int ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (!sys && !ctrl) {
        switch (vk) {
        case VK_ESCAPE:
            app_preview_close();
            return;
        case VK_TAB:
            app_focus_cycle(shift ? -1 : 1);
            return;
        case VK_UP:
            R.top -= app_px(60);
            changed();
            return;
        case VK_DOWN:
            R.top += app_px(60);
            changed();
            return;
        case VK_LEFT:
            R.left -= app_px(60);
            changed();
            return;
        case VK_RIGHT:
            R.left += app_px(60);
            changed();
            return;
        case VK_PRIOR:
            R.top -= view_h() * 0.8;
            changed();
            return;
        case VK_SPACE:
            if(imageview_animated()) { imageview_action(IMAGE_PLAY);return; }
            R.top += view_h() * 0.8 * (shift ? -1 : 1);
            changed();return;
        case VK_NEXT:
            R.top += view_h() * 0.8 * (shift ? -1 : 1);
            changed();
            return;
        case VK_HOME:
            R.left = R.top = 0;
            changed();
            return;
        case VK_END:
            R.top = 1e12;
            changed();
            return;
        }
    }
    fn_ctx c;
    app_preview_ctx(&c, R.hwnd, FN_FROM_KEY);
    uint8_t mods =
        (ctrl ? FN_CTRL : 0) | (shift ? FN_SHIFT : 0) | (sys || GetKeyState(VK_MENU) < 0 ? FN_ALT : 0);
    fn_key((uint8_t)vk, mods, &c);
    app_ctx_free(&c);
}
static LRESULT CALLBACK reader_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_SIZE:
        R.cw = LOWORD(l);
        R.ch = HIWORD(l);
        if (R.opened) {
            if (R.fit)
                fit_view(R.fit == 2);
            else
                changed();
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        int64_t t = fpe_prof_begin();
        if (!R.bdc)
            R.bdc = CreateCompatibleDC(dc);
        if (!R.bmp || R.bw != R.cw || R.bh != R.ch) {
            if (R.bmp) {
                SelectObject(R.bdc, R.old_bmp);
                DeleteObject(R.bmp);
            }
            R.bmp = CreateCompatibleBitmap(dc, R.cw > 0 ? R.cw : 1, R.ch > 0 ? R.ch : 1);
            R.old_bmp = SelectObject(R.bdc, R.bmp);
            R.bw = R.cw;
            R.bh = R.ch;
        }
        paint(R.bdc);
        BitBlt(dc, 0, 0, R.cw, R.ch, R.bdc, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        FPE_PROF_END("image.paint", t);
        return 0;
    }
    case WM_IMG_LAUNCHED:
        if ((uint32_t)w != R.launch_gen || !R.launcher) return 0;
        preview_launch_take(&R.launcher, &R.process, &R.job, &R.pid);
        if (!R.process) {
            helper_stop();
            R.gone = 1;
            R.err = E_FAIL;
            title_update();
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_IMG_GONE:
        if ((uint32_t)w != R.launch_gen) return 0;
        KillTimer(h,TIMER_ANIMATION);
        R.playing=0;
        if (!R.gone) {
            helper_stop();
            R.gone = 1;
            R.err = E_FAIL;
            KillTimer(h, TIMER_WATCHDOG);
            title_update();
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_IMG_RESULT:
        if(w==R.doc&&R.path)app_image_result((int)l);return 0;
    case WM_IMG_MSG: {
        msg *mm = (msg *)l;
        if ((uint32_t)w != R.launch_gen) { free(mm); return 0; }
        R.started = GetTickCount64();
        if (mm->head.type == IMGP_OPENED && mm->head.doc == R.doc) {
            memcpy(&R.info, mm->data, sizeof R.info);
            R.err = (HRESULT)R.info.err;
            R.opened = SUCCEEDED(R.err) && R.info.w && R.info.h && R.info.w <= IMGP_MAX_DIM &&
                       R.info.h <= IMGP_MAX_DIM && (uint64_t)R.info.w * R.info.h <= IMGP_MAX_PIXELS;
            R.info.format[23] = R.info.camera[95] = R.info.date[63] = 0;
            if (R.opened) {
                R.ended=0;R.playing=R.info.animated;R.frame=R.shown_frame=R.passes=0;R.delay_ms=100;
                if(R.fit)fit_view(R.fit==2);else{R.left=R.top=0;changed();}
                request_render();
            } else {
                if(SUCCEEDED(R.err))R.err=WINCODEC_ERR_BADIMAGE;
                KillTimer(h, TIMER_WATCHDOG);
                title_update();
                InvalidateRect(h, NULL, FALSE);
            }
            R.decode_result=R.opened?-1:0;imageview_report_result();
        } else if(mm->head.type==IMGP_PAGED && mm->head.doc==R.doc && R.page_pending && mm->head.ticket==R.page_ticket) {
            imgp_info info;memcpy(&info,mm->data,sizeof info);R.page_pending=0;
            if(info.page!=R.page_desired)page_send();
            else {
                R.err=(HRESULT)info.err;
                if(SUCCEEDED(R.err) && (!info.w||!info.h||info.w>IMGP_MAX_DIM||info.h>IMGP_MAX_DIM||
                   (uint64_t)info.w*info.h>IMGP_MAX_PIXELS||info.pages!=R.info.pages||info.animated))R.err=WINCODEC_ERR_BADIMAGE;
                if(SUCCEEDED(R.err)) {
                    R.info=info;R.info.format[23]=R.info.camera[95]=R.info.date[63]=0;R.frame=info.page;
                    if(R.fit)fit_view(R.fit==2);else changed();request_render();
                }
                imageview_controls_refresh();
            }
        } else if (mm->head.type == IMGP_RENDERED) {
            imgp_rendered p;
            memcpy(&p, mm->data, sizeof p);
            if (p.bank < IMGP_BANKS && R.banks[p.bank].busy && R.banks[p.bank].ticket == mm->head.ticket) {
                int kind = R.banks[p.bank].kind;
                uint32_t epoch = R.banks[p.bank].epoch;
                imgp_render region = R.banks[p.bank].req;
                R.banks[p.bank].busy = 0;
                if (mm->head.doc == R.doc && (kind!=3 || (R.copying==1 && mm->head.ticket==R.copy_ticket)) &&
                    (epoch == R.epoch ||
                     (kind == 3 && region.frame==R.frame && region.rotation == R.rotation && region.flip == R.flip)) &&
                    p.w == region.w && p.h == region.h) {
                    if (SUCCEEDED((HRESULT)p.err)) {
                        if (kind == 3) {
                            copy_pixels(&p);
                            R.copying = 0;
                        } else if (picture_set(kind == 1 ? &R.thumb : &R.sharp, &p, &region))
                            R.err = E_OUTOFMEMORY;
                        else if (kind == 2) {
                            R.sharp_epoch = epoch;R.shown_frame=region.frame;R.delay_ms=p.delay_ms;
                            if(R.decode_result<0){R.decode_result=1;imageview_report_result();}
                            animation_schedule();
                        }
                    } else if ((HRESULT)p.err != E_ABORT) {
                        R.playing=0;KillTimer(h,TIMER_ANIMATION);
                        R.err = (HRESULT)p.err;
                        if(kind!=3&&!R.sharp.bitmap&&R.decode_result<0&&(!R.info.pages||!R.page_desired)){R.decode_result=0;imageview_report_result();}
                        R.copying = 0;
                    }
                    title_update();
                    InvalidateRect(h, NULL, FALSE);
                }
                request_render();
            }
        }
        free(mm);
        return 0;
    }
    case WM_SHOWWINDOW:
        if(w) animation_schedule();else KillTimer(h,TIMER_ANIMATION);
        break;
    case WM_TIMER:
        if(w==TIMER_ANIMATION) {
            KillTimer(h,w);
            if(!R.playing || !imageview_animated() || !IsWindowVisible(h)) return 0;
            if(IsIconic(GetAncestor(h,GA_ROOT))) { animation_schedule();return 0; }
            if(R.frame+1==R.info.frames) {
                if(R.info.loops && ++R.passes>=R.info.loops) { R.playing=0;R.ended=1;title_update();InvalidateRect(h,NULL,FALSE);return 0; }
            }
            frame_change((R.frame+1)%R.info.frames);return 0;
        }
        if (w == TIMER_CLIPBOARD) {
            clipboard_publish();
            return 0;
        }
        if (w == TIMER_RENDER) {
            KillTimer(h, w);
            request_render();
            return 0;
        }
        if (w == TIMER_WATCHDOG) {
            int busy = !R.opened || R.page_pending;
            for (unsigned i = 0; i < IMGP_BANKS; ++i)
                busy |= R.banks[i].busy;
            if (busy && GetTickCount64() - R.started > 15000) {
                helper_stop();
                R.gone = 1;
                R.err = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                KillTimer(h, w);
                title_update();
                InvalidateRect(h, NULL, FALSE);
            } else if (!busy)
                R.started = GetTickCount64();
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
        {int64_t t=fpe_prof_begin();app_focus_window(h);FPE_PROF_END("image.focus",t);}
        if(m==WM_LBUTTONDOWN && GET_Y_LPARAM(l)>=app_px(55) && GET_Y_LPARAM(l)<app_px(79)) {
            int x=GET_X_LPARAM(l);if(x>=app_px(10)&&x<app_px(110)&&R.browse_previous)app_image_button(-1);
            else if(x>=app_px(120)&&x<app_px(220)&&R.browse_next)app_image_button(1);return 0;
        }
        if(m==WM_LBUTTONDOWN && R.opened && R.info.pages>1 && GET_Y_LPARAM(l)>=app_px(83) && GET_Y_LPARAM(l)<app_px(107)) {
            int x=GET_X_LPARAM(l);if(x>=app_px(10)&&x<app_px(110))app_image_page_button(-1);
            else if(x>=app_px(120)&&x<app_px(220))app_image_page_button(1);return 0;
        }
        if(m==WM_LBUTTONDOWN && R.info.animated && GET_Y_LPARAM(l)>=app_px(83) && GET_Y_LPARAM(l)<app_px(107)) {
            int x=GET_X_LPARAM(l),slot=(x-app_px(10))/app_px(90);
            if(x>=app_px(10) && slot<3 && x<app_px(94+90*slot)) imageview_action(slot==0?IMAGE_PREVIOUS:slot==1?IMAGE_PLAY:IMAGE_NEXT);
            return 0;
        }
        if (GET_Y_LPARAM(l) >= head_h()) {
            R.panning = 1;
            R.pan_x = GET_X_LPARAM(l);
            R.pan_y = GET_Y_LPARAM(l);
            R.pan_left = R.left;
            R.pan_top = R.top;
            SetCapture(h);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (R.panning) {
            R.left = R.pan_left + R.pan_x - GET_X_LPARAM(l);
            R.top = R.pan_top + R.pan_y - GET_Y_LPARAM(l);
            changed();
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
        if (R.panning) {
            R.panning = 0;
            ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:
        R.panning = 0;
        return 0;
    case WM_LBUTTONDBLCLK:
        fit_view(0);
        return 0;
    case WM_MOUSEWHEEL: {
        double steps = (short)HIWORD(w) / 120.0;
        if (LOWORD(w) & MK_CONTROL) {
            POINT p = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(h, &p);
            zoom_at(R.scale * pow(1.25, steps), p.x, p.y - head_h());
        } else {
            if (LOWORD(w) & MK_SHIFT)
                R.left -= steps * app_px(100);
            else
                R.top -= steps * app_px(100);
            changed();
        }
        return 0;
    }
    case WM_MOUSEHWHEEL:
        R.left += (short)HIWORD(w) / 120.0 * app_px(100);
        changed();
        return 0;
    case WM_KEYDOWN:
        key(w, 0);
        return 0;
    case WM_SYSKEYDOWN:
        key(w, 1);
        return 0;
    case WM_CHAR:
    case WM_SYSCHAR:
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_CONTEXTMENU: {
        fn_ctx c;
        app_preview_ctx(&c, R.hwnd, FN_FROM_MENU);
        if (!app_menu_context_current(&c)) { app_ctx_free(&c); return 0; }
        HMENU menu = CreatePopupMenu();
        fn_menu(menu, FN_MENU_IMAGE, &c);
        POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (l == -1) {
            pt.x = R.cw / 2;
            pt.y = head_h();
            ClientToScreen(h, &pt);
        }
        UINT id = app_menu_track(menu, h, pt);
        DestroyMenu(menu);
        if (id && app_menu_context_current(&c))
            fn_menu_run(id, &c);
        app_ctx_free(&c);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}
int imageview_start(HWND parent)
{
    if (R.hwnd)
        return 0;
    memset(&R, 0, sizeof R);
    R.cache_mb = 64;
    R.scale = 1;
    WNDCLASSW cls = {0};
    cls.lpfnWndProc = reader_proc;
    cls.hInstance = GetModuleHandleW(NULL);
    cls.lpszClassName = L"OpenPilotImage";
    cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    cls.style = CS_DBLCLKS;
    RegisterClassW(&cls);
    R.hwnd = CreateWindowExW(0, cls.lpszClassName, L"Image preview", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0,
                             parent, NULL, cls.hInstance, NULL);
    if (!R.hwnd) {
        imageview_stop();
        return -1;
    }
    return 0;
}
void imageview_clear(void)
{
    if (R.to)
        send_req(IMGP_CLOSE, ++R.doc, 0, NULL, 0);
    else
        ++R.doc;
    if (R.clipboard) {
        GlobalFree(R.clipboard);
        R.clipboard = NULL;
    }
    KillTimer(R.hwnd, TIMER_ANIMATION);
    R.playing=R.ended=0;R.frame=R.shown_frame=R.passes=0;R.page_desired=R.page_ticket=0;R.page_pending=0;
    memset(&R.info,0,sizeof R.info);
    KillTimer(R.hwnd, TIMER_CLIPBOARD);
    KillTimer(R.hwnd, TIMER_RENDER);
    KillTimer(R.hwnd, TIMER_WATCHDOG);
    picture_drop(&R.thumb);
    picture_drop(&R.sharp);
    free(R.path);
    R.path = NULL;
    R.opened = R.copying = 0;
    R.err = S_OK;
    R.rotation = R.flip = 0;
    R.decode_result=-1;
    R.browse_position=R.browse_count=0;R.browse_previous=R.browse_next=0;R.browse_state=-1;
    R.epoch++;
    if (R.hwnd) {
        title_update();
        InvalidateRect(R.hwnd, NULL, FALSE);
    }
}
void imageview_stop(void)
{
    imageview_clear();
    helper_stop();
    if (R.hwnd)
        DestroyWindow(R.hwnd);
    if (R.bdc) {
        SelectObject(R.bdc, R.old_bmp);
        if (R.bmp)
            DeleteObject(R.bmp);
        DeleteDC(R.bdc);
    }
    memset(&R, 0, sizeof R);
}
HWND imageview_window(void) { return R.hwnd; }
void imageview_configure(uint32_t cache_mb, uint32_t background)
{
    uint32_t cap = cache_mb < 16 ? 16 : cache_mb > 128 ? 128 : cache_mb;
    int reload = R.path && R.cache_mb != cap;
    R.cache_mb = cap;
    R.background = background % 3;
    if (reload) {
        wchar_t *path = _wcsdup(R.path);
        uint64_t size = R.size;
        int64_t mtime = R.mtime;
        if (path) {
            int fit=R.fit;double scale=R.scale;
            imageview_clear();
            imageview_open(path, size, mtime);R.fit=fit;R.scale=scale;
            free(path);
        }
    } else if (R.hwnd)
        InvalidateRect(R.hwnd, NULL, FALSE);
}
static void open_image(const wchar_t *path,uint64_t size,int64_t mtime,int preserve)
{
    if (!R.hwnd || !path)
        return;
    if (R.path && fpe_path_equal(R.path, path) && R.size == size && R.mtime == mtime && !R.gone)
        return;
    int fit=preserve&&R.path?R.fit:1;double scale=preserve&&R.path?R.scale:1;
    imageview_clear();R.fit=fit;R.scale=scale;R.left=R.top=0;
    if (R.gone)
        helper_stop();
    R.path = _wcsdup(path);
    R.size = size;
    R.mtime = mtime;
    if(fpe_wsl_requires_native(path)){R.err=HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);R.decode_result=0;title_update();InvalidateRect(R.hwnd,NULL,FALSE);imageview_report_result();return;}
    if(R.path&&wcslen(path)>=IMGP_PATH_MAX){
        R.err=HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);R.decode_result=0;
        title_update();InvalidateRect(R.hwnd,NULL,FALSE);imageview_report_result();return;
    }
    if (!R.path || helper_start()) {
        R.err = E_FAIL;
        title_update();
        InvalidateRect(R.hwnd, NULL, FALSE);
        return;
    }
    uint32_t n = sizeof(imgp_open) + (uint32_t)(wcslen(path) + 1) * 2;
    uint8_t *p = malloc(n);
    if (!p) {
        R.err = E_OUTOFMEMORY;
        title_update();
        return;
    }
    imgp_open open = {R.cache_mb};
    memcpy(p, &open, sizeof open);
    memcpy(p + sizeof open, path, n - sizeof open);
    if (send_req(IMGP_OPEN, R.doc, 0, p, n))
        R.err = E_FAIL;
    free(p);
    R.started = GetTickCount64();
    SetTimer(R.hwnd, TIMER_WATCHDOG, 500, NULL);
    title_update();
    InvalidateRect(R.hwnd, NULL, FALSE);
}

void imageview_open(const wchar_t *path,uint64_t size,int64_t mtime){open_image(path,size,mtime,0);}
void imageview_open_browse(const wchar_t *path,uint64_t size,int64_t mtime){open_image(path,size,mtime,1);}
const wchar_t *imageview_path(void){return R.path;}
int imageview_matches(uint64_t size,int64_t mtime){return R.path&&R.size==size&&R.mtime==mtime;}
void imageview_browse(uint32_t position,uint32_t count,int previous,int next,int state)
{
    if(!R.hwnd)return;
    if(R.browse_position==position&&R.browse_count==count&&R.browse_previous==previous&&R.browse_next==next&&R.browse_state==state)return;
    R.browse_position=position;R.browse_count=count;R.browse_previous=previous;R.browse_next=next;R.browse_state=state;
    SetPropW(R.hwnd,L"Image.Position",(HANDLE)(UINT_PTR)position);SetPropW(R.hwnd,L"Image.Count",(HANDLE)(UINT_PTR)count);
    SetPropW(R.hwnd,L"Image.Previous",(HANDLE)(INT_PTR)previous);SetPropW(R.hwnd,L"Image.Next",(HANDLE)(INT_PTR)next);
    SetPropW(R.hwnd,L"Image.BrowseState",(HANDLE)(INT_PTR)state);title_update();InvalidateRect(R.hwnd,NULL,FALSE);
}
void imageview_report_result(void)
{
    if(R.hwnd&&R.path&&R.decode_result>=0)PostMessageW(R.hwnd,WM_IMG_RESULT,R.doc,R.decode_result);
}
