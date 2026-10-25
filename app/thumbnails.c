/* Visible-first thumbnails. One worker owns dedicated restricted image/PDF
 * helpers; preview helpers remain independent. The UI never reads a file or
 * waits for a decode. Shared pixels are copied before the next request. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "thumbnails.h"
#include "fpe.h"
#include "imageproto.h"
#include "pdfproto.h"
#include "mediaproto.h"
#include "mediautil.h"
#define WM_THUMB_RESULT (WM_APP + 90)
#define THUMB_TIMEOUT 10000

typedef struct helper {
    HANDLE process, job, section, to, from;
    uint8_t *pixels;
    uint32_t doc;
} helper;
typedef struct result {
    thumb_want want;
    uint32_t w, h;
    int error;
    uint8_t *pixels;
} result;
static struct {
    HWND hwnd, owner;
    UINT changed;
    HANDLE worker, stop, wake, accepted, finished;
    CRITICAL_SECTION lock;
    thumb_want queue[THUMB_WANTS], wanted[THUMB_WANTS], current;
    unsigned count, nwanted;
    uint32_t cache_mb;
    thumb_cache cache;
    uint64_t decoded;
} T;
static wchar_t *copy_path(const wchar_t *s)
{
    size_t n = (wcslen(s) + 1) * sizeof(wchar_t);
    wchar_t *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}
static int same(const thumb_want *a, const thumb_want *b)
{
    return a->size == b->size && a->mtime == b->mtime && a->edge == b->edge && a->pdf == b->pdf &&
           fpe_path_equal(a->path, b->path);
}
static void dispose(void *p) { DeleteObject((HBITMAP)p); }
static void close_helper(helper *h)
{
    if (h->job)
        TerminateJobObject(h->job, 1);
    else if (h->process)
        TerminateProcess(h->process, 1);
    if (h->pixels)
        UnmapViewOfFile(h->pixels);
    HANDLE handles[] = {h->process, h->job, h->section, h->to, h->from};
    for (unsigned i = 0; i < sizeof handles / sizeof *handles; ++i)
        if (handles[i])
            CloseHandle(handles[i]);
    memset(h, 0, sizeof *h);
}
static HANDLE helper_token(void)
{
    HANDLE own = NULL, low = NULL;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT, &own))
        return NULL;
    if (!CreateRestrictedToken(own, DISABLE_MAX_PRIVILEGE, 0, NULL, 0, NULL, 0, NULL, &low))
        low = NULL;
    CloseHandle(own);
    if (!low)
        return NULL;
    SID_IDENTIFIER_AUTHORITY label = SECURITY_MANDATORY_LABEL_AUTHORITY;
    PSID sid = NULL;
    if (AllocateAndInitializeSid(&label, 1, SECURITY_MANDATORY_LOW_RID, 0, 0, 0, 0, 0, 0, 0, &sid)) {
        TOKEN_MANDATORY_LABEL tml = {{sid, SE_GROUP_INTEGRITY}};
        if (!SetTokenInformation(low, TokenIntegrityLevel, &tml, (DWORD)(sizeof tml + GetLengthSid(sid)))) {
            CloseHandle(low);
            low = NULL;
        }
        FreeSid(sid);
    } else {
        CloseHandle(low);
        low = NULL;
    }
    return low;
}

static int start_helper(helper *h, int pdf)
{
    if (h->process && WaitForSingleObject(h->process, 0) == WAIT_TIMEOUT)
        return 0;
    close_helper(h);
    wchar_t dir[MAX_PATH], exe[MAX_PATH + 16], cmd[MAX_PATH + 64];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    wchar_t *slash = n && n < MAX_PATH ? wcsrchr(dir, L'\\') : NULL;
    if (!slash)
        return -1;
    slash[1] = 0;
    _snwprintf(exe, MAX_PATH + 16, L"%ls%ls", dir, pdf==2 ? L"folderwing-media.exe" : pdf ? L"folderwing-pdf.exe" : L"folderwing-image.exe");
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE ci = NULL, co = NULL, token = NULL;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    int attrs_ready = 0, ok = 0;
    PROCESS_INFORMATION pi = {0};
    uint32_t bytes = pdf==2 ? MEDIAP_SECTION_BYTES : pdf ? PDFP_SLOTS * PDFP_SLOT_BYTES : IMGP_BANKS * IMGP_BANK_BYTES;
    h->section = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, bytes, NULL);
    h->pixels = h->section ? MapViewOfFile(h->section, FILE_MAP_READ, 0, 0, 0) : NULL;
    if (!h->pixels || !CreatePipe(&ci, &h->to, &sa, 1 << 20) || !CreatePipe(&h->from, &co, &sa, 1 << 20))
        goto done;
    SetHandleInformation(h->to, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(h->from, HANDLE_FLAG_INHERIT, 0);
    token = helper_token();
    if (!token)
        goto done;
    HANDLE inherit[] = {ci, co, h->section};
    SIZE_T asize = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &asize);
    attrs = malloc(asize);
    if (!attrs || !InitializeProcThreadAttributeList(attrs, 1, 0, &asize))
        goto done;
    attrs_ready = 1;
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, NULL,
                                   NULL))
        goto done;
    STARTUPINFOEXW si = {{sizeof si}};
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = ci;
    si.StartupInfo.hStdOutput = co;
    si.lpAttributeList = attrs;
    _snwprintf(cmd, MAX_PATH + 64, L"\"%ls\" %llu", exe, (unsigned long long)(uintptr_t)h->section);
    if (!CreateProcessAsUserW(token, exe, cmd, NULL, NULL, TRUE,
                              CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL, dir,
                              &si.StartupInfo, &pi))
        goto done;
    h->job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = {0};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                                           JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
                                           JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    lim.ProcessMemoryLimit = (SIZE_T)(pdf==2?1536:512) << 20;
    if (!h->job || !SetInformationJobObject(h->job, JobObjectExtendedLimitInformation, &lim, sizeof lim) ||
        !AssignProcessToJobObject(h->job, pi.hProcess))
        goto done;
    h->process = pi.hProcess;
    if (ResumeThread(pi.hThread) == (DWORD)-1)
        goto done;
    ok = 1;
done:
    if (attrs_ready)
        DeleteProcThreadAttributeList(attrs);
    free(attrs);
    if (token)
        CloseHandle(token);
    if (ci)
        CloseHandle(ci);
    if (co)
        CloseHandle(co);
    if (pi.hThread)
        CloseHandle(pi.hThread);
    if (!ok) {
        if (pi.hProcess && h->process != pi.hProcess) {
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hProcess);
        }
        close_helper(h);
    }
    return ok ? 0 : -1;
}
static int stopped(void) { return WaitForSingleObject(T.stop, 0) == WAIT_OBJECT_0; }
static int transfer(helper *h, int reading, void *data, DWORD n, uint64_t until)
{
    uint8_t *p = data;
    while (n) {
        if (stopped() || GetTickCount64() >= until)
            return -1;
        DWORD got = 0;
        if (reading) {
            DWORD available;
            if (!PeekNamedPipe(h->from, NULL, 0, NULL, &available, NULL))
                return -1;
            if (!available) {
                if (WaitForSingleObject(T.stop, 10) == WAIT_OBJECT_0)
                    return -1;
                continue;
            }
            if (!ReadFile(h->from, p, n < available ? n : available, &got, NULL))
                return -1;
        } else if (!WriteFile(h->to, p, n, &got, NULL))
            return -1;
        if (!got)
            return -1;
        n -= got;
        p += got;
    }
    return 0;
}
static int send(helper *h, uint32_t type, const void *p, uint32_t n, uint64_t until)
{
    imgp_head head = {type, n, h->doc, h->doc};
    return transfer(h, 0, &head, sizeof head, until) || (n && transfer(h, 0, (void *)p, n, until)) ? -1 : 0;
}
static int receive(helper *h, uint32_t type, void *p, uint32_t cap, uint32_t *bytes, uint64_t until)
{
    imgp_head head;
    if (transfer(h, 1, &head, sizeof head, until) || head.type != type || head.doc != h->doc ||
        head.size > cap || transfer(h, 1, p, head.size, until))
        return -1;
    *bytes = head.size;
    return 0;
}
/* A video thumbnail owns a fresh muted source, independent of the live player.
 * No ACK is sent: its one published frame remains immutable until copied. */
static int decode_media(helper *h,result *r){
 close_helper(h);if(!media_path_valid(r->want.path)||start_helper(h,2))return -1;++h->doc;
 uint64_t until=GetTickCount64()+THUMB_TIMEOUT;uint32_t chars=(uint32_t)(wcslen(r->want.path)+1),n=chars*2;
 uint8_t *payload=malloc(sizeof(mediap_open)+n);if(!payload)goto broken;
 mediap_open open={r->want.edge,r->want.edge,MEDIAP_THUMB|MEDIAP_MUTED,0};memcpy(payload,&open,sizeof open);memcpy(payload+sizeof open,r->want.path,n);
 int error=send(h,MEDIAP_OPEN,payload,sizeof open+n,until);free(payload);if(error)goto broken;
 int ready=0,framed=0;mediap_frame frame={0};
 while(!ready||!framed){mediap_head head;union {mediap_state state;mediap_frame frame;} answer;
  if(transfer(h,1,&head,sizeof head,until)||head.doc!=h->doc||head.size>sizeof answer||!((head.type==MEDIAP_STATE&&head.size==sizeof(mediap_state))||(head.type==MEDIAP_FRAME&&head.size==sizeof(mediap_frame)))||transfer(h,1,&answer,head.size,until))goto broken;
  if(head.type==MEDIAP_STATE){if(answer.state.state==MEDIAP_FAILED)goto broken;if(answer.state.state==MEDIAP_READY){if(!answer.state.has_video)goto broken;ready=1;}}
  else{if(!media_frame_valid(&answer.frame)||answer.frame.width>THUMB_EDGE_MAX||answer.frame.height>THUMB_EDGE_MAX)goto broken;frame=answer.frame;framed=1;}
 }
 r->w=frame.width;r->h=frame.height;size_t bytes=(size_t)r->w*r->h*4;r->pixels=malloc(bytes);if(!r->pixels)goto broken;MemoryBarrier();memcpy(r->pixels,h->pixels+(size_t)frame.bank*MEDIAP_BANK_BYTES,bytes);for(size_t i=3;i<bytes;i+=4)r->pixels[i]=255;close_helper(h);return 0;
 broken:close_helper(h);return -1;
}
static int decode(helper *h, result *r)
{
    if(fpe_wsl_requires_native(r->want.path))return -1;
    int pdf = r->want.pdf;
    if(pdf==2)return decode_media(h,r);
    if (start_helper(h, pdf))
        return -1;
    ++h->doc;
    uint64_t until = GetTickCount64() + THUMB_TIMEOUT;
    uint32_t n = (uint32_t)(wcslen(r->want.path) + 1) * 2, got = 0;
    if (n > IMGP_PATH_MAX * 2u)
        return -1;
    if (!pdf) {
        uint8_t *payload = malloc(sizeof(imgp_open) + n);
        if (!payload)
            return -1;
        imgp_open open = {64};
        memcpy(payload, &open, sizeof open);
        memcpy(payload + sizeof open, r->want.path, n);
        int err = send(h, IMGP_OPEN, payload, sizeof open + n, until);
        free(payload);
        imgp_info info;
        if (err || receive(h, IMGP_OPENED, &info, sizeof info, &got, until) || got != sizeof info)
            goto broken;
        if (info.err || !info.w || !info.h) {
            send(h, IMGP_CLOSE, NULL, 0, until);
            return -1;
        }
        thumb_fit(info.w, info.h, r->want.edge, 0, &r->w, &r->h);
        imgp_render request = {.bank = 0, .cw = info.w, .ch = info.h, .w = r->w, .h = r->h};
        imgp_rendered answer;
        if (send(h, IMGP_RENDER, &request, sizeof request, until) ||
            receive(h, IMGP_RENDERED, &answer, sizeof answer, &got, until) || got != sizeof answer ||
            answer.err || answer.bank || answer.w != r->w || answer.h != r->h)
            goto broken;
        if (send(h, IMGP_CLOSE, NULL, 0, until))
            goto broken;
    } else {
        struct {
            pdfp_opened info;
            pdfp_page page;
        } answer;
        if (send(h, PDFP_THUMB_OPEN, r->want.path, n, until) ||
            receive(h, PDFP_OPENED, &answer, sizeof answer, &got, until) || got < sizeof(pdfp_opened))
            goto broken;
        if (answer.info.err || answer.info.npages != 1 || got != sizeof answer) {
            send(h, PDFP_CLOSE, NULL, 0, until);
            return -1;
        }
        thumb_fit(answer.page.w, answer.page.h, r->want.edge, 1, &r->w, &r->h);
        if (!r->w || !r->h)
            goto broken;
        pdfp_render request = {
            .page = 0,
            .slot = 0,
            .epoch = h->doc,
            .scale = (float)(r->want.edge / (answer.page.w > answer.page.h ? answer.page.w : answer.page.h)),
            .w = r->w,
            .h = r->h,
            .colors = PDFP_COLORS_ORIGINAL};
        pdfp_rendered rendered;
        if (send(h, PDFP_RENDER, &request, sizeof request, until) ||
            receive(h, PDFP_RENDERED, &rendered, sizeof rendered, &got, until) || got != sizeof rendered ||
            rendered.err || rendered.slot)
            goto broken;
        if (send(h, PDFP_CLOSE, NULL, 0, until))
            goto broken;
    }
    if (!r->w || !r->h || r->w > THUMB_EDGE_MAX || r->h > THUMB_EDGE_MAX)
        goto broken;
    size_t bytes = (size_t)r->w * r->h * 4;
    r->pixels = malloc(bytes);
    if (!r->pixels)
        return -1;
    memcpy(r->pixels, h->pixels, bytes);
    if (pdf)
        for (size_t i = 3; i < bytes; i += 4)
            r->pixels[i] = 255;
    return 0;
broken:
    close_helper(h);
    return -1;
}
static DWORD WINAPI worker(void *unused)
{
    helper image = {0}, pdf = {0}, media = {0};
    HANDLE finished = T.finished;
    HANDLE waits[] = {T.stop, T.wake};
    for (;;) {
        if (stopped())
            break;
        result *r = calloc(1, sizeof *r);
        if (!r)
            break;
        EnterCriticalSection(&T.lock);
        if (T.count) {
            r->want = T.queue[0];
            memmove(T.queue, T.queue + 1, --T.count * sizeof *T.queue);
            T.current = r->want;
        }
        LeaveCriticalSection(&T.lock);
        if (!r->want.path) {
            free(r);
            DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 30000);
            if (wait == WAIT_OBJECT_0)
                break;
            if (wait == WAIT_TIMEOUT) {
                close_helper(&image);
                close_helper(&pdf);
                close_helper(&media);
            }
            continue;
        }
        r->error = decode(r->want.pdf==2 ? &media : r->want.pdf ? &pdf : &image, r);
        if (stopped() || !PostMessageW(T.hwnd, WM_THUMB_RESULT, 0, (LPARAM)r)) {
            free(r->want.path);
            free(r->pixels);
            free(r);
            break;
        }
        /* At most one posted pixel buffer, even when the UI is in a modal menu. */
        HANDLE accepted[] = {T.stop, T.accepted};
        if (WaitForMultipleObjects(2, accepted, FALSE, INFINITE) == WAIT_OBJECT_0)
            break;
    }
    /* No global/queue/window access after this signal. Destruction of the
     * worker's private decoder jobs can take tens of milliseconds; finish it
     * here while the UI releases its cache and transport immediately. */
    SetEvent(finished);
    close_helper(&image);
    close_helper(&pdf);
                close_helper(&media);
    return 0;
}
static void title(void)
{
    unsigned ready, failed;
    size_t bytes;
    uint64_t decoded;
    thumbnails_stats(&ready, &failed, &bytes, &decoded);
    wchar_t s[160];
    _snwprintf(s, 160, L"Thumbnails | ready %u | unavailable %u | bytes %llu | decoded %llu", ready, failed,
               (unsigned long long)bytes, (unsigned long long)decoded);
    SetWindowTextW(T.hwnd, s);
}
static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_THUMB_RESULT) {
        result *r = (result *)l;
        int wanted = 0, pinned = 0;
        for (unsigned i = 0; i < T.nwanted; ++i)
            if (same(&r->want, &T.wanted[i])) {
                wanted = 1;
                pinned = T.wanted[i].visible;
                break;
            }
        /* Identity, size and mtime guard both rapid navigation and replacements. */
        if (wanted) {
            HBITMAP bmp = NULL;
            size_t bytes = 0;
            if (!r->error && r->pixels) {
                BITMAPINFO bi = {
                    .bmiHeader = {sizeof(BITMAPINFOHEADER), (LONG)r->w, -(LONG)r->h, 1, 32, BI_RGB}};
                void *pixels = NULL;
                bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &pixels, NULL, 0);
                if (bmp) {
                    bytes = (size_t)r->w * r->h * 4;
                    memcpy(pixels, r->pixels, bytes);
                } else
                    r->error = -1;
            }
            if (!thumb_cache_put(&T.cache, r->want.path, r->want.size, r->want.mtime, r->want.edge, r->w,
                                 r->h, r->error, bmp, bytes, pinned) &&
                bmp)
                DeleteObject(bmp);
            ++T.decoded;
            title();
            PostMessageW(T.owner, T.changed, 0, 0);
        }
        EnterCriticalSection(&T.lock);
        memset(&T.current, 0, sizeof T.current);
        LeaveCriticalSection(&T.lock);
        free(r->want.path);
        free(r->pixels);
        free(r);
        SetEvent(T.accepted);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
void thumbnails_configure(uint32_t mb)
{
    if (mb < 8)
        mb = 8;
    if (mb > 128)
        mb = 128;
    if (T.cache_mb == mb)
        return;
    T.cache_mb = mb;
    if (T.hwnd) {
        thumb_cache_clear(&T.cache);
        T.cache.limit = (size_t)mb << 20;
        for (unsigned i = 0; i < T.nwanted; ++i)
            free(T.wanted[i].path);
        T.nwanted = 0;
        title();
    }
}
int thumbnails_start(HWND owner, UINT changed)
{
    if (T.hwnd)
        return 0;
    T.owner = owner;
    T.changed = changed;
    WNDCLASSW wc = {
        .lpfnWndProc = proc, .hInstance = GetModuleHandleW(NULL), .lpszClassName = L"OpenPilotThumbnails"};
    RegisterClassW(&wc);
    T.hwnd = CreateWindowExW(0, wc.lpszClassName, L"Thumbnails", WS_CHILD, 0, 0, 0, 0, owner, NULL,
                             wc.hInstance, NULL);
    if (!T.hwnd)
        return -1;
    InitializeCriticalSection(&T.lock);
    thumb_cache_init(&T.cache, (size_t)(T.cache_mb ? T.cache_mb : 32) << 20, dispose);
    T.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    T.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    T.accepted = CreateEventW(NULL, FALSE, FALSE, NULL);
    T.finished = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (T.stop && T.wake && T.accepted && T.finished)
        T.worker = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (!T.worker) {
        thumbnails_stop();
        return -1;
    }
    title();
    return 0;
}
void thumbnails_stop(void)
{
    if (!T.hwnd)
        return;
    if (T.stop)
        SetEvent(T.stop);
    if (T.worker) {
        CancelSynchronousIo(T.worker);
        WaitForSingleObject(T.finished, INFINITE);
    }
    MSG m;
    while (PeekMessageW(&m, T.hwnd, WM_THUMB_RESULT, WM_THUMB_RESULT, PM_REMOVE)) {
        result *r = (result *)m.lParam;
        free(r->want.path);
        free(r->pixels);
        free(r);
    }
    for (unsigned i = 0; i < T.count; ++i)
        free(T.queue[i].path);
    for (unsigned i = 0; i < T.nwanted; ++i)
        free(T.wanted[i].path);
    thumb_cache_clear(&T.cache);
    HANDLE handles[] = {T.worker, T.stop, T.wake, T.accepted, T.finished};
    for (unsigned i = 0; i < 5; ++i)
        if (handles[i])
            CloseHandle(handles[i]);
    DestroyWindow(T.hwnd);
    DeleteCriticalSection(&T.lock);
    uint32_t mb = T.cache_mb;
    memset(&T, 0, sizeof T);
    T.cache_mb = mb;
}
void thumbnails_submit(const thumb_want *items, unsigned n)
{
    if (!T.hwnd)
        return;
    if (n > THUMB_WANTS)
        n = THUMB_WANTS;
    thumb_want canonical[THUMB_WANTS];
    unsigned nc = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (!items[i].path || !items[i].edge || items[i].edge > THUMB_EDGE_MAX)
            continue;
        unsigned k;
        for (k = 0; k < nc; ++k)
            if (same(&items[i], &canonical[k]))
                break;
        if (k < nc) {
            if (items[i].visible)
                canonical[k].visible = 1;
        } else
            canonical[nc++] = items[i];
    }
    items = canonical;
    n = nc;
    int unchanged = n == T.nwanted;
    for (unsigned i = 0; unchanged && i < n; ++i)
        if (!same(&items[i], &T.wanted[i]) || items[i].visible != T.wanted[i].visible)
            unchanged = 0;
    if (unchanged)
        return;
    for (unsigned i = 0; i < T.nwanted; ++i)
        free(T.wanted[i].path);
    T.nwanted = 0;
    for (unsigned i = 0; i < THUMB_MAX; ++i)
        T.cache.entries[i].pinned = 0;
    /* Build immutable UI identity set before waking the worker. Deduplicate the
     * same file shown in two panes, retaining the higher (visible) priority. */
    for (unsigned i = 0; i < n; ++i) {
        if (!items[i].path || !items[i].edge || items[i].edge > THUMB_EDGE_MAX)
            continue;
        unsigned k;
        for (k = 0; k < T.nwanted; ++k)
            if (same(&items[i], &T.wanted[k]))
                break;
        if (k < T.nwanted) {
            if (items[i].visible)
                T.wanted[k].visible = 1;
            continue;
        }
        wchar_t *p = copy_path(items[i].path);
        if (!p)
            continue;
        T.wanted[T.nwanted] = items[i];
        T.wanted[T.nwanted++].path = p;
    }
    EnterCriticalSection(&T.lock);
    for (unsigned i = 0; i < T.count; ++i)
        free(T.queue[i].path);
    T.count = 0;
    for (int priority = 1; priority >= 0; --priority)
        for (unsigned i = 0; i < T.nwanted; ++i) {
            thumb_want *it = &T.wanted[i];
            if (it->visible != priority)
                continue;
            thumb_entry *e = thumb_cache_find(&T.cache, it->path, it->size, it->mtime, it->edge);
            if (e) {
                e->pinned = it->visible;
                continue;
            }
            if (T.current.path && same(it, &T.current))
                continue;
            wchar_t *p = copy_path(it->path);
            if (!p)
                continue;
            T.queue[T.count] = *it;
            T.queue[T.count++].path = p;
        }
    LeaveCriticalSection(&T.lock);
    title();
    SetEvent(T.wake);
}
thumb_entry *thumbnails_get(const wchar_t *path, uint64_t size, int64_t mtime, uint32_t edge)
{
    return T.hwnd ? thumb_cache_find(&T.cache, path, size, mtime, edge) : NULL;
}
void thumbnails_stats(unsigned *ready, unsigned *failed, size_t *bytes, uint64_t *decoded)
{
    *ready = *failed = 0;
    for (unsigned i = 0; i < THUMB_MAX; ++i)
        if (T.cache.entries[i].path) {
            if (T.cache.entries[i].data)
                ++*ready;
            else
                ++*failed;
        }
    *bytes = T.cache.bytes;
    *decoded = T.decoded;
}
